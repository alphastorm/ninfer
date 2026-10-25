#pragma once

#include "core/paged_kv_cache.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ninfer::models::context {

class LogicalKVPageStore;
class HostKVExtentStore;

class HostKVExtentCapability {
public:
    HostKVExtentCapability() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

    [[nodiscard]] friend bool operator==(HostKVExtentCapability,
                                         HostKVExtentCapability) noexcept = default;

private:
    HostKVExtentCapability(const HostKVExtentStore* owner, std::uint32_t index,
                           std::uint32_t generation) noexcept
        : owner_(owner), index_(index), generation_(generation) {}

    const HostKVExtentStore* owner_ = nullptr;
    std::uint32_t index_            = 0;
    std::uint32_t generation_       = 0;

    friend class HostKVExtentStore;
};

struct HostKVPageReplica {
    HostKVExtentCapability extent;
    std::uint32_t page_offset       = 0;
    std::uint32_t membership_node   = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t content_epoch     = 0;
    std::uint32_t committed_columns = 0;
};

class LogicalKVPageHandle {
public:
    LogicalKVPageHandle() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

    [[nodiscard]] friend bool operator==(LogicalKVPageHandle,
                                         LogicalKVPageHandle) noexcept = default;

private:
    LogicalKVPageHandle(const LogicalKVPageStore* owner, std::uint32_t index,
                        std::uint32_t generation) noexcept
        : owner_(owner), index_(index), generation_(generation) {}

    const LogicalKVPageStore* owner_ = nullptr;
    std::uint32_t index_             = 0;
    std::uint32_t generation_        = 0;

    friend class LogicalKVPageStore;
    friend class HostKVExtentStore;
};

class LogicalKVPageStore {
public:
    class ReadLease {
    public:
        ReadLease() noexcept = default;
        ~ReadLease();
        ReadLease(ReadLease&& other) noexcept;
        ReadLease& operator=(ReadLease&& other) noexcept;
        ReadLease(const ReadLease&)            = delete;
        ReadLease& operator=(const ReadLease&) = delete;

        [[nodiscard]] LogicalKVPageHandle page() const noexcept { return page_; }

        [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }

        [[nodiscard]] std::uint32_t coverage() const noexcept { return coverage_; }

        void reset() noexcept;
    private:
        ReadLease(LogicalKVPageStore& store, LogicalKVPageHandle page, std::uint64_t epoch,
                  std::uint32_t coverage, bool host) noexcept
            : store_(&store), page_(page), epoch_(epoch), coverage_(coverage), host_(host) {}

        LogicalKVPageStore* store_ = nullptr;
        LogicalKVPageHandle page_;
        std::uint64_t epoch_    = 0;
        std::uint32_t coverage_ = 0;
        bool host_              = false;
        friend class LogicalKVPageStore;
    };

    class Restore {
    public:
        ~Restore();
        Restore(Restore&& other) noexcept;
        Restore& operator=(Restore&&)      = delete;
        Restore(const Restore&)            = delete;
        Restore& operator=(const Restore&) = delete;

        [[nodiscard]] LogicalKVPageHandle page() const noexcept { return source_.page(); }

        [[nodiscard]] DeviceKVPageHandle device() const noexcept { return destination_.handle(); }

        void publish();
    private:
        Restore(LogicalKVPageStore& store, ReadLease source, DeviceKVPageLease destination) noexcept
            : store_(&store), source_(std::move(source)), destination_(std::move(destination)) {}

        LogicalKVPageStore* store_ = nullptr;
        ReadLease source_;
        DeviceKVPageLease destination_;
        friend class LogicalKVPageStore;
    };

    [[nodiscard]] ReadLease read_device(LogicalKVPageHandle page, std::uint32_t coverage);
    [[nodiscard]] ReadLease read_host(LogicalKVPageHandle page, std::uint32_t coverage);

    LogicalKVPageStore(DeviceKVPagePool& physical, std::uint32_t logical_capacity);

    LogicalKVPageStore(const LogicalKVPageStore&)            = delete;
    LogicalKVPageStore& operator=(const LogicalKVPageStore&) = delete;
    LogicalKVPageStore(LogicalKVPageStore&&)                 = delete;
    LogicalKVPageStore& operator=(LogicalKVPageStore&&)      = delete;

    [[nodiscard]] DeviceKVPagePool& physical_pool() noexcept { return *physical_; }

    [[nodiscard]] const DeviceKVPagePool& physical_pool() const noexcept { return *physical_; }

    [[nodiscard]] std::uint32_t capacity() const noexcept {
        return static_cast<std::uint32_t>(pages_.size());
    }

    [[nodiscard]] std::uint32_t occupied() const noexcept { return capacity() - free_count_; }

    void materialize(DeviceKVPageReservation& reservation,
                     std::span<LogicalKVPageHandle> destinations,
                     std::optional<LogicalKVPageHandle> preferred_predecessor = std::nullopt);

    [[nodiscard]] LogicalKVPageHandle
    materialize_transfer_destination(DeviceKVPageReservation& reservation,
                                     std::uint32_t committed_columns);

    void publish_transfer_destination(LogicalKVPageHandle handle) noexcept;

    void abort_transfer_destination(LogicalKVPageHandle handle,
                                    DeviceKVPageReservation& reservation) noexcept;

    [[nodiscard]] bool valid(LogicalKVPageHandle handle) const noexcept;

    [[nodiscard]] std::uint32_t descriptor_index(LogicalKVPageHandle handle) const {
        (void)require(handle);
        return handle.index_;
    }

    [[nodiscard]] DeviceKVPageHandle physical(LogicalKVPageHandle handle) const;

    [[nodiscard]] bool device_resident(LogicalKVPageHandle handle) const {
        return require(handle).device_replica.has_value();
    }

    [[nodiscard]] bool host_resident(LogicalKVPageHandle handle) const {
        return require(handle).host_replica.has_value();
    }

    [[nodiscard]] const HostKVPageReplica& host_replica(LogicalKVPageHandle handle) const;

    [[nodiscard]] std::uint64_t content_epoch(LogicalKVPageHandle handle) const {
        return require(handle).content_epoch;
    }

    [[nodiscard]] std::uint32_t committed_columns(LogicalKVPageHandle handle) const {
        return require(handle).committed_columns;
    }

    [[nodiscard]] std::uint32_t required_columns(LogicalKVPageHandle handle) const {
        return require(handle).required_columns;
    }

    [[nodiscard]] bool device_covers(LogicalKVPageHandle handle, std::uint32_t columns) const {
        const auto& page = require(handle);
        return page.device_replica && page.device_columns >= columns;
    }

    [[nodiscard]] bool host_covers(LogicalKVPageHandle handle, std::uint32_t columns) const;

    [[nodiscard]] std::uint32_t required_references_above(LogicalKVPageHandle handle,
                                                          std::uint32_t columns) const;

    // Each history contributes its visible and retained prefix separately. Updating one
    // history cannot remove another history's coverage or protection.
    void update_required(LogicalKVPageHandle handle, std::uint32_t before, std::uint32_t after);
    void update_protection(LogicalKVPageHandle handle, std::uint32_t before, std::uint32_t after);

    [[nodiscard]] bool can_write(LogicalKVPageHandle handle, std::uint64_t writer,
                                 std::uint32_t begin, std::uint32_t own_coverage) const;
    void prepare_write(LogicalKVPageHandle handle, std::uint64_t writer, std::uint32_t begin,
                       std::uint32_t own_coverage);
    void release_writer(LogicalKVPageHandle handle, std::uint64_t writer);

    [[nodiscard]] std::uint64_t writer_identity(LogicalKVPageHandle handle) const {
        return require(handle).writer_identity;
    }

    [[nodiscard]] std::uint32_t history_references(LogicalKVPageHandle handle) const {
        return require(handle).references;
    }

    [[nodiscard]] std::uint32_t active_history_references(LogicalKVPageHandle handle) const {
        return require(handle).active_references;
    }

    [[nodiscard]] std::uint32_t protected_columns(LogicalKVPageHandle handle) const {
        return require(handle).protected_columns;
    }

    [[nodiscard]] std::uint32_t source_pins(LogicalKVPageHandle handle) const {
        return require(handle).readers();
    }

    [[nodiscard]] bool can_pin_source(LogicalKVPageHandle handle) const noexcept;

    [[nodiscard]] Restore reserve_device_replica(LogicalKVPageHandle handle,
                                                 DeviceKVPageReservation& reservation);

    [[nodiscard]] bool drop_device_replica(LogicalKVPageHandle handle) noexcept;

    [[nodiscard]] bool host_replica_current(LogicalKVPageHandle handle) const noexcept;

    [[nodiscard]] bool can_drop_device_replica(LogicalKVPageHandle handle) const noexcept;

    void retain_active_reference(LogicalKVPageHandle handle);

    void release_active_reference(LogicalKVPageHandle handle);

    void retain_reference(LogicalKVPageHandle handle);

    void pin_source(LogicalKVPageHandle handle);

    void unpin_source(LogicalKVPageHandle handle);

    void commit_coverage(LogicalKVPageHandle handle, std::uint32_t columns);

    [[nodiscard]] bool can_release_reference(LogicalKVPageHandle handle) const noexcept;

    [[nodiscard]] bool release_reference(LogicalKVPageHandle handle) noexcept;

private:
    [[nodiscard]] bool can_attach_host_replica(LogicalKVPageHandle handle, std::uint64_t epoch,
                                               std::uint32_t coverage) const noexcept;

    void attach_host_replica(LogicalKVPageHandle handle, HostKVPageReplica replica,
                             HostKVExtentStore& owner);

    void rebind_host_replica(LogicalKVPageHandle handle, HostKVPageReplica expected,
                             HostKVPageReplica replacement) noexcept;

    [[nodiscard]] bool detach_host_replica(LogicalKVPageHandle handle,
                                           HostKVExtentCapability extent) noexcept;

    struct Page {
        std::uint32_t generation        = 1;
        std::uint64_t content_epoch     = 0;
        std::uint32_t committed_columns = 0;
        std::uint32_t references        = 0;
        std::uint32_t active_references = 0;
        std::uint32_t protected_columns = 0;
        std::uint32_t required_columns  = 0;
        std::uint32_t device_columns    = 0;
        std::uint64_t writer_identity   = 0;
        std::array<std::uint32_t, kPagedKVPageSize> required{};
        std::array<std::uint32_t, kPagedKVPageSize> protections{};
        std::uint32_t device_readers = 0;
        std::uint32_t host_readers   = 0;
        bool host_transfer_pending   = false;

        [[nodiscard]] std::uint32_t readers() const noexcept {
            return device_readers + host_readers;
        }

        bool destination_pinned = false;
        bool occupied           = false;
        std::optional<DeviceKVPageLease> device_replica;
        std::optional<HostKVPageReplica> host_replica;
        HostKVExtentStore* host_owner = nullptr;
    };

    [[nodiscard]] static std::uint64_t next_epoch(std::uint64_t epoch) noexcept {
        ++epoch;
        return epoch == 0 ? 1 : epoch;
    }

    [[nodiscard]] Page& require(LogicalKVPageHandle handle) {
        if (!valid(handle)) { throw std::invalid_argument("logical KV page handle is stale"); }
        return pages_[handle.index_];
    }

    [[nodiscard]] const Page& require(LogicalKVPageHandle handle) const {
        if (!valid(handle)) { throw std::invalid_argument("logical KV page handle is stale"); }
        return pages_[handle.index_];
    }

    void release_descriptor(LogicalKVPageHandle handle, Page& page) noexcept;
    void retire_unowned(LogicalKVPageHandle handle) noexcept;

    DeviceKVPagePool* physical_ = nullptr;
    std::vector<Page> pages_;
    std::vector<std::uint32_t> free_;
    std::vector<DeviceKVPageLease> materialization_scratch_;
    std::uint32_t free_count_ = 0;

    friend class HostKVExtentStore;
};
} // namespace ninfer::models::context
