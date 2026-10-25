#pragma once

#include "core/host_kv_arena.h"
#include "models/common/context/logical_kv_store.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ninfer::models::context {

class HostKVExtentStore;

struct HostKVPageReplicaRelease {
    LogicalKVPageStore* pages = nullptr;
    LogicalKVPageHandle page;
};

class HostKVExtentReservation {
public:
    HostKVExtentReservation() noexcept = default;
    ~HostKVExtentReservation();

    HostKVExtentReservation(HostKVExtentReservation&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), descriptor_(other.descriptor_),
          generation_(other.generation_), page_store_(other.page_store_),
          allocation_(std::move(other.allocation_)), imported_(other.imported_) {}

    HostKVExtentReservation& operator=(HostKVExtentReservation&&)      = delete;
    HostKVExtentReservation(const HostKVExtentReservation&)            = delete;
    HostKVExtentReservation& operator=(const HostKVExtentReservation&) = delete;

    [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

private:
    HostKVExtentStore* owner_       = nullptr;
    std::uint32_t descriptor_       = 0;
    std::uint32_t generation_       = 0;
    LogicalKVPageStore* page_store_ = nullptr;
    std::optional<HostKVAllocation> allocation_;
    bool imported_ = false;

    friend class HostKVExtentStore;
};

// Owns typed Host extents and the ordered logical pages represented by each allocation. It has no
// checkpoint, retention, or scheduling policy.
class HostKVExtentStore {
public:
    // Owns unpublished Host-only pages, then their temporary references after publish.
    // Histories retain their own references; destruction retires every unadopted page.
    class Import {
    public:
        ~Import();
        Import(Import&&) noexcept;
        Import& operator=(Import&&) = delete;
        Import(const Import&) = delete;
        Import& operator=(const Import&) = delete;
        [[nodiscard]] std::span<const LogicalKVPageHandle> pages() const noexcept { return pages_; }
        [[nodiscard]] HostKVAllocationView writable_view();
        void publish();
    private:
        Import(HostKVExtentStore&, LogicalKVPageStore&, HostKVExtentReservation,
               std::vector<LogicalKVPageHandle>);
        HostKVExtentStore* owner_;
        LogicalKVPageStore* store_;
        HostKVExtentReservation reservation_;
        std::vector<LogicalKVPageHandle> pages_;
        friend class HostKVExtentStore;
    };

    HostKVExtentStore(HostKVArena& arena, std::uint32_t descriptor_capacity);

    HostKVExtentStore(const HostKVExtentStore&)            = delete;
    HostKVExtentStore& operator=(const HostKVExtentStore&) = delete;
    HostKVExtentStore(HostKVExtentStore&&)                 = delete;
    HostKVExtentStore& operator=(HostKVExtentStore&&)      = delete;

    [[nodiscard]] std::uint32_t capacity() const noexcept {
        return static_cast<std::uint32_t>(extents_.size());
    }

    [[nodiscard]] std::uint32_t occupied() const noexcept { return capacity() - free_count_; }

    void restrict_coverage(LogicalKVPageStore& pages, LogicalKVPageHandle handle,
                           std::uint32_t coverage);

    [[nodiscard]] const HostKVPageLayout& page_layout(const LogicalKVPageStore& pages) const;

    [[nodiscard]] std::optional<HostKVExtentReservation>
    prepare(LogicalKVPageStore& pages, std::span<const LogicalKVPageHandle> membership);
    [[nodiscard]] std::optional<Import>
    prepare_import(LogicalKVPageStore& pages, std::span<const std::uint32_t> coverage);

    [[nodiscard]] HostKVAllocationView writable_view(HostKVExtentReservation& reservation);

    [[nodiscard]] std::vector<DeviceKVPageHandle>
    device_sources(const HostKVExtentReservation& reservation) const;

    void device_sources(const HostKVExtentReservation& reservation,
                        std::span<DeviceKVPageHandle> out) const;

    [[nodiscard]] std::uint32_t page_count(const HostKVExtentReservation& reservation) const {
        validate(reservation);
        return extents_[reservation.descriptor_].page_count;
    }

    [[nodiscard]] HostKVExtentCapability publish(HostKVExtentReservation&& reservation) noexcept;

    void abort(HostKVExtentReservation& reservation) noexcept;

    [[nodiscard]] bool valid(HostKVExtentCapability capability) const noexcept {
        return capability.owner_ == this && capability.index_ < extents_.size() &&
               extents_[capability.index_].state == ExtentState::Published &&
               extents_[capability.index_].generation == capability.generation_;
    }

    [[nodiscard]] HostKVAllocationConstView view(HostKVExtentCapability capability) const {
        const Extent& extent = require(capability);
        return arena_->view(*extent.allocation);
    }

    [[nodiscard]] bool can_release_page_replica(LogicalKVPageStore& pages,
                                                LogicalKVPageHandle page) const noexcept;

    [[nodiscard]] bool
    can_release_page_replicas(std::span<const HostKVPageReplicaRelease> releases) const noexcept;

    [[nodiscard]] bool
    can_release_page_replicas(LogicalKVPageStore& pages,
                              std::span<const LogicalKVPageHandle> releases) const noexcept;

    [[nodiscard]] bool release_page_replicas(std::span<const HostKVPageReplicaRelease> releases);

    [[nodiscard]] bool release_page_replicas(LogicalKVPageStore& pages,
                                             std::span<const LogicalKVPageHandle> releases);

private:
    static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    enum class ExtentState : std::uint8_t {
        Free,
        Reserved,
        Published,
    };

    struct Extent {
        ExtentState state              = ExtentState::Free;
        std::uint32_t generation       = 1;
        LogicalKVPageStore* page_store = nullptr;
        std::optional<HostKVAllocation> allocation;
        std::uint32_t head       = kInvalidIndex;
        std::uint32_t tail       = kInvalidIndex;
        std::uint32_t page_count = 0;
    };

    struct Membership {
        LogicalKVPageHandle page;
        std::uint64_t epoch    = 0;
        std::uint32_t coverage = 0;
        std::uint32_t extent   = kInvalidIndex;
        std::uint32_t offset   = 0;
        std::uint32_t next     = kInvalidIndex;
    };

    struct PartitionRun {
        std::uint32_t begin = 0;
        std::uint32_t head  = kInvalidIndex;
        std::uint32_t tail  = kInvalidIndex;
        std::uint32_t count = 0;
        bool release        = false;
    };

    [[nodiscard]] bool valid(const HostKVExtentReservation& reservation) const noexcept {
        if (reservation.owner_ != this || reservation.descriptor_ >= extents_.size() ||
            reservation.page_store_ == nullptr) {
            return false;
        }
        const Extent& extent = extents_[reservation.descriptor_];
        return extent.state == ExtentState::Reserved &&
               extent.generation == reservation.generation_ &&
               extent.page_store == reservation.page_store_ && reservation.allocation_ &&
               extent.page_count != 0 && extent.head != kInvalidIndex &&
               extent.tail != kInvalidIndex;
    }

    void validate(const HostKVExtentReservation& reservation) const {
        if (!valid(reservation)) { throw std::logic_error("Host KV extent reservation is stale"); }
    }

    [[nodiscard]] const Extent& require(HostKVExtentCapability capability) const {
        if (!valid(capability)) { throw std::invalid_argument("Host KV extent is stale"); }
        return extents_[capability.index_];
    }

    static void increment_generation(std::uint32_t& generation) noexcept {
        ++generation;
        if (generation == 0) { ++generation; }
    }

    [[nodiscard]] std::uint32_t take_membership() noexcept {
        if (free_membership_count_ == 0) { return kInvalidIndex; }
        return free_memberships_[--free_membership_count_];
    }

    void begin_release_marks() const noexcept;

    [[nodiscard]] bool mark_release(LogicalKVPageStore& pages,
                                    LogicalKVPageHandle page) const noexcept;

    void release_marked_extents();

    void partition_extent(std::uint32_t index);

    void release_descriptor(std::uint32_t index, Extent& extent) noexcept;

    static void consume(HostKVExtentReservation& reservation) noexcept {
        reservation.owner_      = nullptr;
        reservation.page_store_ = nullptr;
    }

    HostKVArena* arena_ = nullptr;
    std::vector<Extent> extents_;
    std::vector<std::uint32_t> free_;
    std::uint32_t free_count_ = 0;
    std::vector<Membership> memberships_;
    std::vector<std::uint32_t> free_memberships_;
    std::uint32_t free_membership_count_ = 0;
    mutable std::vector<std::uint32_t> release_marks_;
    mutable std::vector<std::uint32_t> extent_marks_;
    mutable std::vector<std::uint32_t> affected_extents_;
    mutable std::uint32_t release_stamp_ = 0;
    std::vector<PartitionRun> partition_runs_;
};

} // namespace ninfer::models::context
