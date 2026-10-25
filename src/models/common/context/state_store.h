#pragma once

#include "core/host_context_arena.h"
#include "core/state_image.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ninfer::models::context {

enum class StateImageRole : std::uint8_t {
    Free,
    ActiveMutable,
    CheckpointImmutable,
    ReservedDestination,
    Writing,
};

enum class StateReplicaResidency : std::uint8_t {
    None,
    DeviceOnly,
    HostOnly,
    Both,
};

enum class StateTransferDirection : std::uint8_t {
    DeviceToHost,
    HostToDevice,
};

struct StateImageSelectors {
    std::int32_t source      = -1;
    std::int32_t destination = -1;
};

// Logical identities share one physical schema; Host replicas own their arena allocation.
class StateImageStore {
public:
    class Handle {
    public:
        Handle() noexcept = default;

        [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

        [[nodiscard]] friend bool operator==(Handle, Handle) noexcept = default;

    private:
        Handle(const StateImageStore* owner, std::uint32_t index, std::uint32_t generation) noexcept
            : owner_(owner), index_(index), generation_(generation) {}

        const StateImageStore* owner_ = nullptr;
        std::uint32_t index_          = 0;
        std::uint32_t generation_     = 0;

        friend class StateImageStore;
    };

    class ReadLease {
    public:
        ReadLease() noexcept = default;
        ~ReadLease();
        ReadLease(ReadLease&& other) noexcept;
        ReadLease& operator=(ReadLease&& other) noexcept;
        ReadLease(const ReadLease&)            = delete;
        ReadLease& operator=(const ReadLease&) = delete;

        [[nodiscard]] Handle image() const noexcept { return image_; }

        [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }

        [[nodiscard]] std::int32_t device_slot() const;
        [[nodiscard]] ConstMemorySpan host_view() const;
        void reset() noexcept;
    private:
        ReadLease(StateImageStore& store, Handle image, std::uint64_t epoch) noexcept
            : store_(&store), image_(image), epoch_(epoch) {}

        StateImageStore* store_ = nullptr;
        Handle image_;
        std::uint64_t epoch_ = 0;
        friend class StateImageStore;
    };

    // Owns a new logical image until the native continuation adopts it.
    class Destination {
    public:
        Destination(StateImageStore& store, Handle image);
        ~Destination();
        Destination(Destination&& other) noexcept;
        Destination& operator=(Destination&&)      = delete;
        Destination(const Destination&)            = delete;
        Destination& operator=(const Destination&) = delete;

        [[nodiscard]] Handle image() const noexcept { return image_; }

        [[nodiscard]] StateImageSelectors begin_write(Handle source);
        [[nodiscard]] Handle release() noexcept;
    private:
        StateImageStore* store_ = nullptr;
        Handle image_;
        Handle write_source_;
    };

    // Physical reservations and source readership belong to this move-only copy. MigrationBatch
    // retains it until its completion fence permits publication or rollback.
    class Transfer {
    public:
        Transfer() noexcept = default;
        ~Transfer();
        Transfer(Transfer&& other) noexcept;
        Transfer& operator=(Transfer&&)      = delete;
        Transfer(const Transfer&)            = delete;
        Transfer& operator=(const Transfer&) = delete;

        [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

        [[nodiscard]] StateTransferDirection direction() const noexcept { return direction_; }

        [[nodiscard]] TransferWork work() const;
        void enqueue(CudaExecutionView execution) const;
        void publish(bool keep_source_replica);
    private:
        Transfer(StateImageStore& owner, std::uint64_t id, StateTransferDirection direction,
                 ReadLease source, Handle destination) noexcept;
        StateImageStore* owner_           = nullptr;
        std::uint64_t id_                 = 0;
        StateTransferDirection direction_ = StateTransferDirection::DeviceToHost;
        ReadLease source_;
        Handle destination_;
        std::optional<std::int32_t> device_slot_;
        std::optional<HostContextAllocation> host_slot_;
        friend class StateImageStore;
    };

    [[nodiscard]] ReadLease read(Handle image);

    StateImageStore(StateImageStorage& device, HostContextArena* host,
                    std::uint32_t logical_capacity);

    StateImageStore(const StateImageStore&)            = delete;
    StateImageStore& operator=(const StateImageStore&) = delete;
    StateImageStore(StateImageStore&&)                 = delete;
    StateImageStore& operator=(StateImageStore&&)      = delete;

    [[nodiscard]] std::uint32_t capacity() const noexcept;

    [[nodiscard]] std::uint32_t occupied() const noexcept;

    [[nodiscard]] std::uint32_t device_occupied() const noexcept;

    [[nodiscard]] std::uint32_t device_capacity() const noexcept;

    [[nodiscard]] std::uint32_t host_occupied() const noexcept;

    [[nodiscard]] std::optional<Handle> reserve_destination() noexcept;

    [[nodiscard]] std::optional<Handle> reserve_logical_destination() noexcept;
    // Fresh unpublished Host-only image. Destination owns rollback until snapshot adoption.
    [[nodiscard]] std::optional<Handle> reserve_host_destination() noexcept;
    [[nodiscard]] MemorySpan writable_host_view(Handle);
    void complete_host_initialization(Handle);

    // The caller has initialized every required field, ordered on its execution stream.
    // Adopting a materialized copy retains its content epoch; native writes use complete_write.
    // Publication does not wait for that stream; cross-stream readers inherit its dependency.
    void complete_initialization(Handle handle);

    [[nodiscard]] bool valid(Handle handle) const noexcept;

    [[nodiscard]] std::uint32_t descriptor_index(Handle handle) const;

    [[nodiscard]] StateImageRole role(Handle handle) const;

    [[nodiscard]] StateReplicaResidency residency(Handle handle) const;

    [[nodiscard]] bool device_resident(Handle handle) const;

    [[nodiscard]] bool host_resident(Handle handle) const;

    [[nodiscard]] std::uint64_t content_epoch(Handle handle) const;

    [[nodiscard]] std::uint32_t source_pins(Handle handle) const;

    [[nodiscard]] std::uint32_t checkpoint_references(Handle handle) const;

    void retain_checkpoint_reference(Handle handle);

    // Content owners and readers retire independently. A last checkpoint can disappear while
    // another request still reads this immutable image; that read lease keeps its replicas alive.
    [[nodiscard]] bool release_checkpoint_owner(Handle handle) noexcept;

    [[nodiscard]] std::int32_t physical_slot(Handle handle) const;

    void make_mutable(Handle handle);
    void consume_checkpoint(Handle handle);

    [[nodiscard]] bool drop_device_replica(Handle handle) noexcept;

    [[nodiscard]] bool drop_host_replica(Handle handle) noexcept;

    // Transfers the Device replica to a new logical identity while the old immutable identity
    // keeps its Host replica. This is the zero-copy preserving branch for a Both source.
    void split_device_replica_identity(Handle source, Handle destination);

    void freeze(Handle handle);

    [[nodiscard]] StateImageSelectors begin_write(Handle source, Handle destination);

    void complete_write(Handle source, Handle destination);

    void abort_write(Handle source, Handle destination);

    [[nodiscard]] bool can_abort_write(Handle source, Handle destination) const noexcept;

    [[nodiscard]] StateImageSelectors selectors(Handle source, Handle destination) const;

    [[nodiscard]] std::optional<Transfer>
    reserve_device_to_host(Handle source, HostContextAllocation reserved = {});

    [[nodiscard]] std::optional<Transfer> reserve_host_to_device(Handle source);
    [[nodiscard]] std::optional<Transfer> reserve_host_to_device(Handle source, Handle destination);

    [[nodiscard]] bool release(Handle handle) noexcept;

    [[nodiscard]] bool can_release(Handle handle) const noexcept;

    [[nodiscard]] bool
    can_release_after_checkpoint_references(Handle handle, std::uint32_t released_references) const;

private:
    struct Object {
        std::uint32_t generation    = 1;
        std::uint64_t content_epoch = 0;
        std::optional<std::int32_t> device_slot;
        std::optional<HostContextAllocation> host_slot;
        std::uint64_t transfer_id           = 0;
        std::uint32_t checkpoint_references = 0;
        std::uint32_t source_pins           = 0;
        Handle write_source;
        bool destination_pinned = false;
        bool release_pending    = false;
        StateImageRole role     = StateImageRole::Free;
    };

    void retire_released_owner(Handle handle) noexcept;
    [[nodiscard]] bool can_release_checkpoint_owner(Handle handle) const noexcept;

    [[nodiscard]] static bool has_pending_replica(const Object& object) noexcept;

    [[nodiscard]] std::optional<Handle> allocate(StateImageRole role, bool with_device) noexcept;

    [[nodiscard]] std::optional<std::int32_t> take_device_slot() noexcept;

    void return_device_slot(std::int32_t slot) noexcept;

    [[nodiscard]] std::uint64_t next_epoch() noexcept;

    [[nodiscard]] std::uint64_t next_transfer() noexcept;

    [[nodiscard]] Object& require(Handle handle);

    [[nodiscard]] const Object& require(Handle handle) const;

    [[nodiscard]] bool valid_transfer(const Transfer& transfer) const noexcept;

    void validate_transfer(const Transfer& transfer) const;

    void publish_transfer(Transfer& transfer, bool keep_source_replica);
    void abort_transfer(Transfer& transfer) noexcept;

    static void consume(Transfer& transfer) noexcept;

    StateImageStorage* device_ = nullptr;
    HostContextArena* host_    = nullptr;
    std::vector<Object> objects_;
    std::vector<std::uint32_t> free_objects_;
    std::vector<std::int32_t> free_device_slots_;
    std::uint32_t free_object_count_   = 0;
    std::uint32_t free_device_count_   = 0;
    std::uint32_t pending_host_images_ = 0;
    std::uint64_t next_content_epoch_  = 0;
    std::uint64_t next_transfer_id_    = 0;
};

template <class Metadata>
struct Continuation {
    StateImageStore::Handle image;
    Metadata metadata;
};

} // namespace ninfer::models::context
