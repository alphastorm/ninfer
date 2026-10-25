#pragma once

#include "core/device.h"
#include "models/common/context/host_kv_store.h"
#include "models/common/context/kv_history.h"
#include "models/common/context/state_store.h"
#include "runtime/contract/resources.h"

#include <optional>
#include <vector>

namespace ninfer::models::context {

class MigrationBatch;

// One reusable execution slot. Events and timing storage are allocated at Program startup.
class MigrationContext {
public:
    MigrationContext(DeviceContext& device, std::size_t pool_count);
    MigrationContext(const MigrationContext&)            = delete;
    MigrationContext& operator=(const MigrationContext&) = delete;

private:
    DeviceContext* device_;
    CudaCompletionEvent source_ready_;
    CudaCompletionEvent completed_;
    std::vector<CudaEventTimer> timers_;
    std::vector<runtime::ContextTransferObservation> measured_;
    bool occupied_ = false;
    friend class MigrationBatch;
};

// Owns all readers, pending replicas and execution rows until the batch's fence retires.
// Cancellation discards publication; it does not return in-flight storage to an allocator.
class MigrationBatch {
public:
    MigrationBatch(MigrationContext& context, HostKVExtentStore* host);
    ~MigrationBatch();
    MigrationBatch(MigrationBatch&& other) noexcept;
    MigrationBatch& operator=(MigrationBatch&&)      = delete;
    MigrationBatch(const MigrationBatch&)            = delete;
    MigrationBatch& operator=(const MigrationBatch&) = delete;

    void own_state(StateImageStore& store, StateImageStore::Handle image);
    [[nodiscard]] StateImageStore::Destination& state_destination();
    void add_state_copy(StateImageStore::Transfer transfer, bool keep_source_replica);
    void add_state_group(StateImageStore& store, StateImageStorage& storage,
                         StateImageStore::Handle source, LayoutRegion group);
    void clear_state(StateImageStorage& storage, StateImageStore& store);
    void add_binding(runtime::KvPoolId pool, KVBinding binding);
    [[nodiscard]] KVBinding* binding(runtime::KvPoolId pool) noexcept;
    [[nodiscard]] const KVBinding* binding(runtime::KvPoolId pool) const noexcept;
    void add_backup(runtime::KvPoolId pool, LogicalKVPageStore& pages,
                    std::vector<LogicalKVPageHandle> logical, HostKVExtentReservation destination,
                    bool drop_device);
    [[nodiscard]] std::uint32_t spill_page_groups() const noexcept;
    [[nodiscard]] std::uint32_t tail_copy_page_groups() const noexcept;

    // Finalize physical runs, inherit the compute dependency, then enqueue all copies and
    // mapping uploads. Pending H2D destinations can feed COW within this ordered batch.
    void reset_plan();
    void submit();
    [[nodiscard]] bool ready() const;
    [[nodiscard]] std::vector<runtime::ContextTransferObservation> observations() const;
    void publish();

    void cancel() noexcept { cancelled_ = true; }

    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }

    void discard();
    // Error/shutdown only. A partial submission without a recorded fence drains its stream.
    void drain();

private:
    enum class Stage { Reserved, Submitting, Submitted, Published, Discarded };

    struct StateCopy {
        StateImageStore::Transfer transfer;
        bool keep_source;
    };

    struct StateGroup {
        StateImageStore::ReadLease source;
        StateImageStorage* storage;
        LayoutRegion group;
        std::int32_t destination;
    };

    struct HostRun {
        HostKVAllocationConstView host;
        std::vector<DeviceKVPageHandle> device;
        TransferWork work;
    };

    struct Binding {
        runtime::KvPoolId pool;
        KVBinding reservation;
        std::vector<HostRun> restores;
    };

    struct Backup {
        runtime::KvPoolId pool;
        LogicalKVPageStore* pages;
        std::vector<LogicalKVPageHandle> logical;
        std::vector<DeviceKVPageHandle> physical;
        HostKVExtentReservation destination;
        bool drop_device;
        TransferWork work;
    };

    struct Clear {
        StateImageStorage* storage;
        std::int32_t slot;
    };

    void require_reserved() const;
    void release_reservations();
    [[nodiscard]] std::size_t timer_index(runtime::ContextTransferResource resource,
                                          runtime::ContextTransferDirection direction) const;

    MigrationContext* context_;
    HostKVExtentStore* host_;
    Stage stage_    = Stage::Reserved;
    bool cancelled_ = false;
    std::optional<StateImageStore::Destination> state_;
    std::vector<StateCopy> state_copies_;
    std::vector<StateGroup> state_groups_;
    std::optional<Clear> clear_;
    std::vector<Binding> bindings_;
    std::vector<Backup> backups_;
};

} // namespace ninfer::models::context
