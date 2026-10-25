#include "models/qwen3_5/program/program_impl.h"
#include "models/common/context/work.h"
#include "core/device.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace ninfer::models::qwen3_5::detail {

SequenceHandle ProgramImpl::sequence_handle(std::uint32_t lane) const noexcept {
    return ContractAccess::make_sequence(this, runtime::LaneId{lane}, lane_epochs[lane]);
}

bool ProgramImpl::valid_sequence(SequenceHandle handle) const noexcept {
    const auto lane = ContractAccess::lane(handle).value;
    return ContractAccess::owner(handle) == this && lane < max_concurrency &&
           ContractAccess::epoch(handle) == lane_epochs[lane] &&
           requests[lane].lifecycle != Lifecycle::Empty;
}

bool ProgramImpl::recovery_pending(SequenceHandle handle) const noexcept {
    return valid_sequence(handle) &&
           requests[ContractAccess::lane(handle).value].recovery.has_value();
}

bool ProgramImpl::valid_pending(const PendingBatch& pending) const noexcept {
    if (!pending_transaction_ || ContractAccess::owner(pending) != this ||
        ContractAccess::transaction(pending) != pending_transaction_->id ||
        pending.row_count() != pending_transaction_->size) {
        return false;
    }
    const auto rows = ContractAccess::rows(pending);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!valid_sequence(rows[i]) ||
            ContractAccess::lane(rows[i]).value != pending_transaction_->lanes[i] ||
            ContractAccess::epoch(rows[i]) != pending_transaction_->epochs[i]) {
            return false;
        }
    }
    return true;
}

void ProgramImpl::invalidate_lane(std::uint32_t lane) noexcept {
    if (++lane_epochs[lane] == 0) { ++lane_epochs[lane]; }
}

SequenceState& ProgramImpl::active_sequence(std::uint32_t lane) {
    if (lane >= max_concurrency || requests[lane].lifecycle == Lifecycle::Empty) {
        throw std::logic_error("execution lane is empty");
    }
    return sequences[lane];
}

const SequenceState& ProgramImpl::active_sequence(std::uint32_t lane) const {
    if (lane >= max_concurrency || requests[lane].lifecycle == Lifecycle::Empty) {
        throw std::logic_error("execution lane is empty");
    }
    return sequences[lane];
}

context::StateImageSelectors ProgramImpl::state_selectors(const SequenceState& sequence) const {
    return {state_store->physical_slot(sequence.continuation.image),
            state_store->physical_slot(sequence.write_state())};
}

Tensor ProgramImpl::tail_hidden(const SequenceState& sequence) const {
    return state_images->continuation_hidden_slot(
        state_store->physical_slot(sequence.continuation.image));
}

void ProgramImpl::complete_state_write(SequenceState& sequence) {
    const auto destination = sequence.write_state();
    state_store->complete_write(sequence.continuation.image, destination);
    sequence.continuation.image = destination;
    sequence.continuation.metadata.execution_frontier =
        sequence.continuation.metadata.text_kv_valid;
    sequence.pending_write = {};
}

void ProgramImpl::release_sequence_state(SequenceState& sequence) noexcept {
    try {
        if (sequence.pending_write.valid()) {
            state_store->abort_write(sequence.continuation.image, sequence.write_state());
        }
        if (state_store->valid(sequence.write_state()) &&
            !state_store->release(sequence.write_state())) {
            std::terminate();
        }
        sequence.continuation.image = {};
        sequence.pending_write      = {};
    } catch (...) { std::terminate(); }
}

void ProgramImpl::release_sequence_kv(SequenceState& sequence) noexcept { sequence.kv.reset(); }

void ProgramImpl::clear_lane(SequenceState& sequence, RequestControl& request) noexcept {
    // The caller settles real GPU readers before reaching this destruction boundary.
    const auto lane = sequence.lane;
    request.prefill.reset();
    release_sequence_kv(sequence);
    release_sequence_state(sequence);
    sequence      = {};
    sequence.lane = lane;
    request       = {};
    invalidate_lane(lane);
}

void ProgramImpl::clear_execution_failure_lanes(std::span<const std::uint32_t> lanes) noexcept {
    for (const auto lane : lanes) {
        if (lane < max_concurrency) { clear_lane(sequences[lane], requests[lane]); }
    }
}

PagedKVCache* ProgramImpl::backend_kv_cache() noexcept {
    if (speculative_backend == SpeculativeBackend::Mtp) { return decoder->mtp_cache(); }
    return dflash && dflash->full ? &*dflash->full : nullptr;
}

const PagedKVCache* ProgramImpl::backend_kv_cache() const noexcept {
    if (speculative_backend == SpeculativeBackend::Mtp) { return decoder->mtp_cache(); }
    return dflash && dflash->full ? &*dflash->full : nullptr;
}

std::uint32_t ProgramImpl::backend_kv_valid(const SequenceState& state) const noexcept {
    if (speculative_backend == SpeculativeBackend::Mtp) {
        return state.continuation.metadata.backend_kv_valid;
    }
    return speculative_backend == SpeculativeBackend::DFlash
               ? state.continuation.metadata.dflash_context_frontier
               : 0;
}

void ProgramImpl::ensure_sequence_kv_mapped(SequenceState& state, std::uint32_t main,
                                            std::uint32_t backend) {
    const auto main_begin = text_kv_histories->committed_frontier(state.kv->text.handle());
    if (main > main_begin)
        text_kv_histories->prepare_write(state.kv->text, main_begin, main, device.stream());
    if (state.kv->backend) {
        const auto backend_begin =
            backend_kv_histories->committed_frontier(state.kv->backend->handle());
        if (backend > backend_begin)
            backend_kv_histories->prepare_write(*state.kv->backend, backend_begin, backend,
                                                device.stream());
    }
}

void ProgramImpl::commit_sequence_kv(SequenceState& state, std::uint32_t main,
                                     std::uint32_t backend) {
    state.continuation.metadata.backend_kv_valid = backend;
    text_kv_histories->commit_frontier(state.kv->text.handle(), main);
    if (state.kv->backend) {
        backend_kv_histories->commit_frontier(state.kv->backend->handle(), backend);
    }
}

void ProgramImpl::trim_sequence_kv(SequenceState& state, std::uint32_t main,
                                   std::uint32_t backend) {
    text_kv_histories->trim(state.kv->text, main, device.stream());
    if (state.kv->backend) backend_kv_histories->trim(*state.kv->backend, backend, device.stream());
}

PagedKVCacheView ProgramImpl::mtp_kv_view(const SequenceState& state) const {
    return speculative_backend == SpeculativeBackend::Mtp
               ? decoder->mtp_cache()->execution_view(
                     backend_kv_histories->execution_row(state.kv->backend->handle()))
               : PagedKVCacheView{};
}

void ProgramImpl::set_device_i32(Tensor& tensor, std::int32_t value) {
    CUDA_CHECK(cudaMemcpyAsync(tensor.data, &value, sizeof(value), cudaMemcpyHostToDevice,
                               device.stream()));
}

} // namespace ninfer::models::qwen3_5::detail
