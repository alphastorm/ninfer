#include "models/qwen3_5/program/program_impl.h"

#include <stdexcept>
#include <type_traits>

namespace ninfer::models::qwen3_5::detail {

bool ProgramImpl::context_blocks(SequenceHandle sequence) const noexcept {
    if (!context_operation_ || !valid_sequence(sequence)) return false;
    if (!std::holds_alternative<BindOperation>(context_operation_->action)) return false;
    const auto& state = sequences[ContractAccess::lane(sequence).value];
    if (!state.kv) return false;
    auto& migration     = context_operation_->migration;
    const auto* text    = migration.binding(kTextKvPool);
    const auto* backend = migration.binding(kBackendKvPool);
    return (text && text->blocks_writer(state.kv->text.handle())) ||
           (backend && state.kv->backend && backend->blocks_writer(state.kv->backend->handle()));
}

ContextProgress ProgramImpl::poll_context(runtime::CancellationFlagView cancellation) {
    if (!context_operation_) throw std::logic_error("there is no context operation");
    auto& operation = *context_operation_;
    auto& migration = operation.migration;
    ContextProgress out{.kind = operation.kind()};
    // The slot, affected lane and all reservations remain occupied while a cancelled batch drains.
    if (cancellation.requested()) migration.cancel();
    if (!migration.ready()) return out;
    out.transfers                         = migration.observations();
    out.operations.pressure_spill_pages   = migration.spill_page_groups();
    out.operations.partial_tail_cow_pages = migration.tail_copy_page_groups();
    for (const auto& transfer : out.transfers)
        if (transfer.resource.is_state() &&
            transfer.direction == runtime::ContextTransferDirection::HostToDevice)
            out.operations.state_restores += transfer.image_count;
    if (auto* pause = std::get_if<PauseOperation>(&operation.action)) {
        out.request_timings     = pause->paused.impl_->control.timings;
        out.request_speculative = pause->paused.impl_->control.speculative_stats;
    }
    if (migration.cancelled()) {
        abort_context();
        out.complete = true;
        return out;
    }
    migration.publish();
    std::visit(
        [&](auto& action) {
            using Action = std::decay_t<decltype(action)>;
            if constexpr (std::is_same_v<Action, BindOperation>) {
                complete_binding(action, migration, out);
            } else if constexpr (std::is_same_v<Action, CaptureOperation>) {
                out.captured_checkpoints.reserve(action.points.size());
                publish_capture(action, migration, out.operations);
                for (const auto& [handle, role] : action.points)
                    out.captured_checkpoints.push_back(handle);
            } else if constexpr (std::is_same_v<Action, PauseOperation>) {
                out.paused.emplace(complete_pause(action));
            }
        },
        operation.action);
    out.complete = out.published = true;
    context_operation_.reset();
    return out;
}

void ProgramImpl::abort_context() noexcept {
    if (!context_operation_) return;
    auto& operation = *context_operation_;
    try {
        // Ordinary cancellation arrives after ready(). Error and shutdown paths may need an
        // explicit drain, including partial submission before the batch event was recorded.
        if (!operation.migration.ready()) operation.migration.drain();
        operation.migration.discard();
        std::visit(
            [&](auto& action) {
                using Action = std::decay_t<decltype(action)>;
                if constexpr (std::is_same_v<Action, BindOperation>) {
                    auto& state = sequences[action.lane];
                    release_sequence_state(state);
                    release_sequence_kv(state);
                    requests[action.lane] = {};
                    invalidate_lane(action.lane);
                    action.source_lease.reset();
                    action.carried_pins.clear();
                    for (const auto& [from, to] : action.carried_clones) {
                        if (valid_checkpoint(to))
                            (void)release_checkpoint(to);
                        else
                            checkpoints.cancel(to);
                    }
                } else if constexpr (std::is_same_v<Action, CaptureOperation>) {
                    auto& state = sequences[action.lane];
                    if (state_store->role(state.continuation.image) ==
                            context::StateImageRole::CheckpointImmutable &&
                        !state_store->checkpoint_references(state.continuation.image))
                        state_store->make_mutable(state.continuation.image);
                    for (const auto& [handle, role] : action.points)
                        if (!valid_checkpoint(handle)) checkpoints.cancel(handle);
                } else if constexpr (std::is_same_v<Action, PauseOperation>) {
                    requests[action.lane] = {};
                    invalidate_lane(action.lane);
                }
            },
            operation.action);
        context_operation_.reset();
    } catch (...) { std::terminate(); }
}

} // namespace ninfer::models::qwen3_5::detail
