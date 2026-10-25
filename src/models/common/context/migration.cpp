#include "models/common/context/migration.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace ninfer::models::context {
namespace {
using Direction = runtime::ContextTransferDirection;
using Resource  = runtime::ContextTransferResource;

void check_work(TransferWork planned, TransferWork submitted) {
    if (planned != submitted) throw std::logic_error("KV transfer plan and submission disagree");
}
} // namespace

MigrationContext::MigrationContext(DeviceContext& device, std::size_t pool_count)
    : device_(&device), source_ready_(device), completed_(device), measured_((pool_count + 1) * 3) {
    timers_.reserve(measured_.size());
    for (std::size_t i = 0; i < measured_.size(); ++i)
        timers_.emplace_back(device, device.transfer_stream());
}

MigrationBatch::MigrationBatch(MigrationContext& context, HostKVExtentStore* host)
    : context_(&context), host_(host) {
    if (context.occupied_) throw std::logic_error("context migration slot is occupied");
    context.occupied_ = true;
    for (auto& value : context.measured_) value = {};
}

MigrationBatch::MigrationBatch(MigrationBatch&& other) noexcept
    : context_(std::exchange(other.context_, nullptr)), host_(other.host_), stage_(other.stage_),
      cancelled_(other.cancelled_), state_(std::move(other.state_)),
      state_copies_(std::move(other.state_copies_)), state_groups_(std::move(other.state_groups_)),
      clear_(other.clear_), bindings_(std::move(other.bindings_)),
      backups_(std::move(other.backups_)) {}

MigrationBatch::~MigrationBatch() {
    if (!context_) return;
    if (stage_ == Stage::Submitting || (stage_ == Stage::Submitted && !ready())) std::terminate();
    release_reservations();
    context_->occupied_ = false;
}

void MigrationBatch::require_reserved() const {
    if (stage_ != Stage::Reserved) throw std::logic_error("migration already submitted");
}

void MigrationBatch::own_state(StateImageStore& store, StateImageStore::Handle image) {
    require_reserved();
    if (state_) throw std::logic_error("migration already owns a State destination");
    state_.emplace(store, image);
}

StateImageStore::Destination& MigrationBatch::state_destination() {
    if (!state_) throw std::logic_error("migration has no State destination");
    return *state_;
}

void MigrationBatch::add_state_copy(StateImageStore::Transfer transfer, bool keep_source_replica) {
    require_reserved();
    state_copies_.push_back({std::move(transfer), keep_source_replica});
}

void MigrationBatch::add_state_group(StateImageStore& store, StateImageStorage& storage,
                                     StateImageStore::Handle source, LayoutRegion group) {
    require_reserved();
    state_groups_.push_back(
        {store.read(source), &storage, group, store.physical_slot(state_destination().image())});
}

void MigrationBatch::clear_state(StateImageStorage& storage, StateImageStore& store) {
    require_reserved();
    clear_ = Clear{&storage, store.physical_slot(state_destination().image())};
}

void MigrationBatch::add_binding(runtime::KvPoolId pool, KVBinding binding) {
    require_reserved();
    if (this->binding(pool)) throw std::logic_error("duplicate KV binding in migration");
    bindings_.push_back({pool, std::move(binding), {}});
}

KVBinding* MigrationBatch::binding(runtime::KvPoolId pool) noexcept {
    for (auto& value : bindings_)
        if (value.pool == pool) return &value.reservation;
    return nullptr;
}

const KVBinding* MigrationBatch::binding(runtime::KvPoolId pool) const noexcept {
    for (const auto& value : bindings_)
        if (value.pool == pool) return &value.reservation;
    return nullptr;
}

void MigrationBatch::add_backup(runtime::KvPoolId pool, LogicalKVPageStore& pages,
                                std::vector<LogicalKVPageHandle> logical,
                                HostKVExtentReservation destination, bool drop_device) {
    require_reserved();
    if (!host_) throw std::logic_error("KV backup has no Host store");
    auto physical   = host_->device_sources(destination);
    const auto work = pages.physical_pool().host_transfer_work(physical);
    backups_.push_back({pool, &pages, std::move(logical), std::move(physical),
                        std::move(destination), drop_device, work});
}

std::uint32_t MigrationBatch::spill_page_groups() const noexcept {
    std::uint32_t count = 0;
    for (const auto& backup : backups_)
        if (backup.drop_device) count += backup.logical.size();
    return count;
}

std::uint32_t MigrationBatch::tail_copy_page_groups() const noexcept {
    std::uint32_t count = 0;
    for (const auto& value : bindings_) count += value.reservation.needs_tail_copy();
    return count;
}

std::size_t MigrationBatch::timer_index(Resource resource, Direction direction) const {
    const auto index = resource.index() * 3 + static_cast<std::size_t>(direction);
    if (index >= context_->timers_.size()) throw std::logic_error("unknown migration pool ID");
    return index;
}

void MigrationBatch::reset_plan() {
    require_reserved();
    release_reservations();
}

void MigrationBatch::submit() {
    require_reserved();
    // Capture Host runs while their byte ranges are held. Later extent partitioning may change
    // capabilities but cannot return these ranges while the per-page readers remain alive.
    for (auto& value : bindings_) {
        auto& binding       = value.reservation;
        const auto restores = binding.restores();
        for (std::size_t begin = 0; begin < restores.size();) {
            if (!host_) throw std::logic_error("KV restore has no Host store");
            const auto first = binding.pages().host_replica(restores[begin].page());
            auto end         = begin + 1;
            while (end < restores.size()) {
                const auto next = binding.pages().host_replica(restores[end].page());
                if (next.extent != first.extent ||
                    next.page_offset != first.page_offset + end - begin)
                    break;
                ++end;
            }
            HostRun run{.host = host_->view(first.extent).subview(first.page_offset, end - begin)};
            run.device.reserve(end - begin);
            for (auto i = begin; i < end; ++i) run.device.push_back(restores[i].device());
            run.work = binding.pages().physical_pool().host_transfer_work(run.device);
            value.restores.push_back(std::move(run));
            begin = end;
        }
        binding.prepare_tail_copy();
    }
    stage_       = Stage::Submitting;
    auto& device = *context_->device_;
    context_->source_ready_.record(device.stream());
    context_->source_ready_.wait(device.transfer_stream());
    const auto measure = [&](Resource resource, Direction direction, auto&& enqueue) {
        const auto index   = timer_index(resource, direction);
        auto& observed     = context_->measured_[index];
        observed.resource  = resource;
        observed.direction = direction;
        context_->timers_[index].start();
        enqueue(observed);
        context_->timers_[index].record_stop();
    };
    for (auto direction : {Direction::DeviceToHost, Direction::HostToDevice}) {
        const auto matches = [&](const StateCopy& copy) {
            return (copy.transfer.direction() == StateTransferDirection::DeviceToHost) ==
                   (direction == Direction::DeviceToHost);
        };
        if (std::none_of(state_copies_.begin(), state_copies_.end(), matches)) continue;
        measure({}, direction, [&](auto& observed) {
            for (auto& copy : state_copies_) {
                if (!matches(copy)) continue;
                const auto work = copy.transfer.work();
                copy.transfer.enqueue(device.transfer_view());
                observed.work.payload_bytes += work.payload_bytes;
                observed.work.copy_operations += work.copy_operations;
                ++observed.image_count;
            }
        });
    }
    if (!state_groups_.empty())
        measure({}, Direction::DeviceToDevice, [&](auto& observed) {
            for (const auto& copy : state_groups_) {
                copy.storage->copy_group(copy.group, copy.source.device_slot(), copy.destination,
                                         device.transfer_view());
                observed.work.payload_bytes += copy.group.bytes;
                observed.work.copy_operations += copy.group.bytes != 0;
                ++observed.image_count;
            }
        });
    if (clear_) clear_->storage->clear(clear_->slot, device.transfer_view());
    const auto pool_count = context_->measured_.size() / 3 - 1;
    for (std::size_t index = 0; index < pool_count; ++index) {
        const runtime::KvPoolId pool{static_cast<std::uint16_t>(index)};
        if (std::any_of(backups_.begin(), backups_.end(),
                        [&](const auto& b) { return b.pool == pool; })) {
            measure({pool}, Direction::DeviceToHost, [&](auto& observed) {
                for (auto& backup : backups_) {
                    if (backup.pool != pool) continue;
                    const auto actual = backup.pages->physical_pool().copy_to_host(
                        backup.physical, host_->writable_view(backup.destination),
                        device.transfer_stream());
                    check_work(backup.work, actual);
                    observed.work.payload_bytes += actual.payload_bytes;
                    observed.work.copy_operations += actual.copy_operations;
                    observed.page_groups += backup.logical.size();
                }
            });
        }
        for (auto& value : bindings_) {
            if (value.pool != pool) continue;
            auto& binding  = value.reservation;
            auto& physical = binding.pages().physical_pool();
            if (!value.restores.empty())
                measure({pool}, Direction::HostToDevice, [&](auto& observed) {
                    for (const auto& run : value.restores) {
                        const auto actual =
                            physical.copy_from_host(run.host, run.device, device.transfer_stream());
                        check_work(run.work, actual);
                        observed.work.payload_bytes += actual.payload_bytes;
                        observed.work.copy_operations += actual.copy_operations;
                        observed.page_groups += run.device.size();
                    }
                });
            if (binding.needs_tail_copy())
                measure({pool}, Direction::DeviceToDevice, [&](auto& observed) {
                    observed.work =
                        physical.copy_page(binding.tail_source(), binding.tail_destination(),
                                           device.transfer_stream());
                    observed.page_groups = 1;
                });
            binding.enqueue_mapping(device.transfer_stream());
        }
    }
    context_->completed_.record(device.transfer_stream());
    stage_ = Stage::Submitted;
}

bool MigrationBatch::ready() const {
    if (stage_ == Stage::Submitting) return false;
    return stage_ != Stage::Submitted || context_->completed_.ready();
}

std::vector<runtime::ContextTransferObservation> MigrationBatch::observations() const {
    if (stage_ == Stage::Reserved || !ready())
        throw std::logic_error("migration observations are not ready");
    std::vector<runtime::ContextTransferObservation> result;
    for (std::size_t i = 0; i < context_->measured_.size(); ++i) {
        auto value = context_->measured_[i];
        if (!value.work.copy_operations) continue;
        value.elapsed_ns =
            std::max<std::uint64_t>(1, std::llround(context_->timers_[i].elapsed_ms() * 1.0e6));
        result.push_back(value);
    }
    return result;
}

void MigrationBatch::publish() {
    if (stage_ != Stage::Submitted || cancelled_ || !ready())
        throw std::logic_error("migration publication precedes completion");
    state_groups_.clear();
    for (auto& copy : state_copies_) copy.transfer.publish(copy.keep_source);
    state_copies_.clear();
    for (auto& backup : backups_) {
        (void)host_->publish(std::move(backup.destination));
        if (backup.drop_device)
            for (auto page : backup.logical) {
                if (!backup.pages->valid(page)) continue;
                if (!backup.pages->drop_device_replica(page))
                    throw std::logic_error("published Host copy still has a Device reader");
            }
    }
    backups_.clear();
    for (auto& value : bindings_) value.reservation.publish_restores();
    stage_ = Stage::Published;
}

void MigrationBatch::release_reservations() {
    clear_.reset();
    state_copies_.clear();
    state_groups_.clear();
    backups_.clear();
    bindings_.clear();
    state_.reset();
}

void MigrationBatch::discard() {
    if (!ready()) throw std::logic_error("cannot retire an in-flight migration");
    release_reservations();
    stage_ = Stage::Discarded;
}

void MigrationBatch::drain() {
    if (stage_ == Stage::Submitting) {
        CUDA_CHECK(cudaStreamSynchronize(context_->device_->transfer_stream()));
        stage_ = Stage::Reserved;
    } else if (stage_ == Stage::Submitted) {
        context_->completed_.synchronize();
    }
}

} // namespace ninfer::models::context
