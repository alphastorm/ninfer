#include "models/common/context/host_kv_store.h"

#include <algorithm>
#include <exception>

namespace ninfer::models::context {

HostKVExtentReservation::~HostKVExtentReservation() {
    if (owner_ != nullptr) { owner_->abort(*this); }
}

HostKVExtentStore::Import::Import(HostKVExtentStore& owner, LogicalKVPageStore& store,
                                 HostKVExtentReservation reservation,
                                 std::vector<LogicalKVPageHandle> pages)
    : owner_(&owner), store_(&store), reservation_(std::move(reservation)),
      pages_(std::move(pages)) {}

HostKVExtentStore::Import::Import(Import&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), store_(other.store_),
      reservation_(std::move(other.reservation_)), pages_(std::move(other.pages_)) {}

HostKVExtentStore::Import::~Import() {
    if (!owner_ || reservation_.valid()) return; // Reservation abort owns unpublished pages.
    for (auto page : pages_)
        if (!store_->release_reference(page)) std::terminate();
    // A history retains adopted pages. Only the remaining orphan pages may lose their Host bytes.
    std::erase_if(pages_, [&](auto page) {
        return !store_->valid(page) || store_->history_references(page) != 0;
    });
    if (!pages_.empty() && !owner_->release_page_replicas(*store_, pages_)) std::terminate();
}

HostKVAllocationView HostKVExtentStore::Import::writable_view() {
    if (!owner_) throw std::logic_error("Host KV import was moved");
    return owner_->writable_view(reservation_);
}

void HostKVExtentStore::Import::publish() {
    if (!owner_ || !reservation_.valid()) throw std::logic_error("Host KV import already published");
    (void)owner_->publish(std::move(reservation_));
}

HostKVExtentStore::HostKVExtentStore(HostKVArena& arena, std::uint32_t descriptor_capacity)
    : arena_(&arena), extents_(descriptor_capacity), free_(descriptor_capacity),
      free_count_(descriptor_capacity), memberships_(descriptor_capacity),
      free_memberships_(descriptor_capacity), free_membership_count_(descriptor_capacity),
      release_marks_(descriptor_capacity), extent_marks_(descriptor_capacity) {
    if (descriptor_capacity == 0) {
        throw std::invalid_argument("Host KV extent descriptor capacity is zero");
    }
    for (std::uint32_t index = 0; index < descriptor_capacity; ++index) {
        free_[index]             = descriptor_capacity - 1U - index;
        free_memberships_[index] = descriptor_capacity - 1U - index;
    }
    affected_extents_.reserve(descriptor_capacity);
    partition_runs_.reserve(descriptor_capacity);
}

void HostKVExtentStore::restrict_coverage(LogicalKVPageStore& pages, LogicalKVPageHandle handle,
                                          std::uint32_t coverage) {
    auto& page = pages.require(handle);
    if (!page.host_replica || page.readers() || page.destination_pinned ||
        coverage > page.host_replica->committed_columns) {
        throw std::logic_error("Host KV coverage cannot be restricted");
    }
    auto& replica = *page.host_replica;
    if (replica.extent.owner_ != this) {
        throw std::logic_error("Host KV coverage belongs to another store");
    }
    auto& membership = memberships_.at(replica.membership_node);
    if (membership.page != handle || membership.coverage != replica.committed_columns) {
        throw std::logic_error("Host KV membership changed before restriction");
    }
    membership.coverage       = coverage;
    replica.committed_columns = coverage;
}

const HostKVPageLayout& HostKVExtentStore::page_layout(const LogicalKVPageStore& pages) const {
    const HostKVPageLayout* layout = arena_->layout_for(pages.physical_pool().geometry());
    if (layout == nullptr) {
        throw std::invalid_argument("Host KV page store has no arena layout");
    }
    return *layout;
}

std::optional<HostKVExtentReservation>
HostKVExtentStore::prepare(LogicalKVPageStore& pages,
                           std::span<const LogicalKVPageHandle> membership) {
    if (membership.empty() || free_count_ == 0 || membership.size() > free_membership_count_) {
        return std::nullopt;
    }
    for (const LogicalKVPageHandle page : membership) {
        if (!pages.can_pin_source(page) || pages.require(page).host_readers ||
            pages.require(page).host_transfer_pending || pages.host_replica_current(page)) {
            return std::nullopt;
        }
    }

    const HostKVPageLayout& layout = page_layout(pages);
    std::optional<HostKVAllocation> allocation =
        arena_->allocate(layout, static_cast<std::uint32_t>(membership.size()));
    if (!allocation) { return std::nullopt; }

    const std::uint32_t descriptor = free_[--free_count_];
    Extent& extent                 = extents_[descriptor];
    if (extent.state != ExtentState::Free) { std::terminate(); }
    extent.state      = ExtentState::Reserved;
    extent.page_store = &pages;
    for (const LogicalKVPageHandle page : membership) {
        const std::uint32_t node = take_membership();
        if (node == kInvalidIndex) { std::terminate(); }
        Membership& entry = memberships_[node];
        entry.page        = page;
        entry.epoch       = pages.content_epoch(page);
        entry.coverage    = pages.required_columns(page);
        entry.extent      = descriptor;
        entry.offset      = extent.page_count;
        entry.next        = kInvalidIndex;
        if (extent.tail == kInvalidIndex) {
            extent.head = node;
        } else {
            memberships_[extent.tail].next = node;
        }
        extent.tail = node;
        ++extent.page_count;
    }
    for (const LogicalKVPageHandle page : membership) {
        pages.pin_source(page);
        pages.require(page).host_transfer_pending = true;
    }

    HostKVExtentReservation reservation;
    reservation.owner_      = this;
    reservation.descriptor_ = descriptor;
    reservation.generation_ = extent.generation;
    reservation.page_store_ = &pages;
    reservation.allocation_ = std::move(allocation);
    return reservation;
}

std::optional<HostKVExtentStore::Import>
HostKVExtentStore::prepare_import(LogicalKVPageStore& pages,
                                  std::span<const std::uint32_t> coverage) {
    if (coverage.empty() || std::any_of(coverage.begin(), coverage.end(), [](auto value) {
            return value == 0 || value > static_cast<std::uint32_t>(kPagedKVPageSize);
        }))
        throw std::invalid_argument("Host KV import coverage is invalid");
    if (!free_count_ || coverage.size() > free_membership_count_ ||
        coverage.size() > pages.free_count_)
        return std::nullopt;
    std::vector<LogicalKVPageHandle> handles;
    handles.reserve(coverage.size());
    auto allocation = arena_->allocate(page_layout(pages), coverage.size());
    if (!allocation) return std::nullopt;
    const auto descriptor = free_[--free_count_];
    auto& extent = extents_[descriptor];
    extent.state = ExtentState::Reserved;
    extent.page_store = &pages;
    HostKVExtentReservation reservation;
    reservation.owner_ = this;
    reservation.descriptor_ = descriptor;
    reservation.generation_ = extent.generation;
    reservation.page_store_ = &pages;
    reservation.allocation_ = std::move(allocation);
    reservation.imported_ = true;
    // All allocations and validation precede descriptor mutation.
    for (auto columns : coverage) {
        const auto index = pages.free_[--pages.free_count_];
        auto& page = pages.pages_[index];
        page.content_epoch = LogicalKVPageStore::next_epoch(page.content_epoch);
        page.committed_columns = columns;
        page.references = 1; // Import owns this reference until histories retain their own.
        page.destination_pinned = true;
        page.occupied = true;
        const LogicalKVPageHandle handle(&pages, index, page.generation);
        handles.push_back(handle);
        const auto node = take_membership();
        if (node == kInvalidIndex) std::terminate();
        auto& entry = memberships_[node];
        entry = {.page = handle, .epoch = page.content_epoch, .coverage = columns,
                 .extent = descriptor, .offset = extent.page_count, .next = kInvalidIndex};
        if (extent.tail == kInvalidIndex) extent.head = node;
        else memberships_[extent.tail].next = node;
        extent.tail = node;
        ++extent.page_count;
    }
    return Import(*this, pages, std::move(reservation), std::move(handles));
}

HostKVAllocationView HostKVExtentStore::writable_view(HostKVExtentReservation& reservation) {
    validate(reservation);
    return arena_->writable_view(*reservation.allocation_);
}

std::vector<DeviceKVPageHandle>
HostKVExtentStore::device_sources(const HostKVExtentReservation& reservation) const {
    validate(reservation);
    std::vector<DeviceKVPageHandle> out;
    out.resize(extents_[reservation.descriptor_].page_count);
    device_sources(reservation, out);
    return out;
}

void HostKVExtentStore::device_sources(const HostKVExtentReservation& reservation,
                                       std::span<DeviceKVPageHandle> out) const {
    validate(reservation);
    const Extent& extent = extents_[reservation.descriptor_];
    if (out.size() != extent.page_count) {
        throw std::invalid_argument("Host KV device-source output has the wrong size");
    }
    std::uint32_t node = extent.head;
    for (std::size_t index = 0; index < out.size(); ++index) {
        if (node == kInvalidIndex) { std::terminate(); }
        out[index] = extent.page_store->physical(memberships_[node].page);
        node       = memberships_[node].next;
    }
    if (node != kInvalidIndex) { std::terminate(); }
}

HostKVExtentCapability HostKVExtentStore::publish(HostKVExtentReservation&& reservation) noexcept {
    if (!valid(reservation)) { std::terminate(); }
    Extent& extent = extents_[reservation.descriptor_];
    if (reservation.imported_) {
        extent.allocation = std::move(reservation.allocation_);
        extent.allocation->publish();
        extent.state = ExtentState::Published;
        const HostKVExtentCapability capability(this, reservation.descriptor_, extent.generation);
        auto node = extent.head;
        for (std::uint32_t i = 0; i < extent.page_count; ++i) {
            const auto& entry = memberships_[node];
            auto& page = extent.page_store->require(entry.page);
            if (!page.destination_pinned || page.device_replica || page.host_replica ||
                page.references != 1 || page.readers() || page.content_epoch != entry.epoch)
                std::terminate();
            try {
                extent.page_store->attach_host_replica(entry.page,
                    {.extent = capability, .page_offset = i, .membership_node = node,
                     .content_epoch = entry.epoch, .committed_columns = entry.coverage}, *this);
            } catch (...) { std::terminate(); }
            page.destination_pinned = false;
            node = entry.next;
        }
        consume(reservation);
        return capability;
    }
    std::vector<LogicalKVPageHandle> readers;
    readers.reserve(extent.page_count);
    std::vector<LogicalKVPageHandle> replaced;
    replaced.reserve(extent.page_count);
    std::uint32_t node = extent.head;
    for (std::uint32_t index = 0; index < extent.page_count; ++index) {
        if (node == kInvalidIndex) { std::terminate(); }
        const Membership& entry = memberships_[node];
        auto& pages             = *extent.page_store;
        if (pages.content_epoch(entry.page) != entry.epoch ||
            !pages.device_covers(entry.page, entry.coverage) ||
            !pages.require(entry.page).host_transfer_pending ||
            pages.require(entry.page).device_readers == 0) {
            std::terminate();
        }
        if (pages.host_resident(entry.page)) replaced.push_back(entry.page);
        readers.push_back(entry.page);
        node = entry.next;
    }
    if (node != kInvalidIndex || !release_page_replicas(*extent.page_store, replaced)) {
        std::terminate();
    }
    extent.allocation = std::move(reservation.allocation_);
    reservation.allocation_.reset();
    extent.allocation->publish();
    extent.state = ExtentState::Published;
    const HostKVExtentCapability capability(this, reservation.descriptor_, extent.generation);
    node = extent.head;
    for (std::uint32_t index = 0; index < extent.page_count; ++index) {
        Membership& entry = memberships_[node];
        try {
            extent.page_store->attach_host_replica(
                entry.page,
                HostKVPageReplica{.extent            = capability,
                                  .page_offset       = index,
                                  .membership_node   = node,
                                  .content_epoch     = entry.epoch,
                                  .committed_columns = entry.coverage},
                *this);
        } catch (...) { std::terminate(); }
        node = entry.next;
    }
    auto* pages = extent.page_store;
    consume(reservation);
    for (auto page : readers) {
        pages->require(page).host_transfer_pending = false;
        pages->unpin_source(page);
    }
    return capability;
}

void HostKVExtentStore::abort(HostKVExtentReservation& reservation) noexcept {
    if (!valid(reservation)) {
        consume(reservation);
        return;
    }
    Extent& extent     = extents_[reservation.descriptor_];
    std::uint32_t node = extent.head;
    for (std::uint32_t index = 0; index < extent.page_count; ++index) {
        if (node == kInvalidIndex) { std::terminate(); }
        const LogicalKVPageHandle page = memberships_[node].page;
        if (reservation.imported_) {
            auto& value = extent.page_store->require(page);
            if (!value.destination_pinned || value.device_replica || value.host_replica ||
                value.references != 1 || value.readers())
                std::terminate();
            extent.page_store->release_descriptor(page, value);
            node = memberships_[node].next;
            continue;
        }
        if (!extent.page_store->valid(page) || extent.page_store->source_pins(page) == 0) {
            std::terminate();
        }
        try {
            extent.page_store->require(page).host_transfer_pending = false;
            extent.page_store->unpin_source(page);
        } catch (...) { std::terminate(); }
        node = memberships_[node].next;
    }
    release_descriptor(reservation.descriptor_, extent);
    reservation.allocation_.reset();
    consume(reservation);
}

bool HostKVExtentStore::can_release_page_replica(LogicalKVPageStore& pages,
                                                 LogicalKVPageHandle page) const noexcept {
    if (!pages.valid(page) || !pages.host_resident(page) || pages.require(page).host_readers != 0 ||
        (!pages.device_resident(page) && pages.history_references(page) != 0)) {
        return false;
    }
    const HostKVPageReplica replica = pages.host_replica(page);
    if (!valid(replica.extent)) { return false; }
    const Extent& extent     = extents_[replica.extent.index_];
    const std::uint32_t node = replica.membership_node;
    if (extent.page_store != &pages || replica.page_offset >= extent.page_count ||
        node >= memberships_.size() || memberships_[node].page != page ||
        memberships_[node].extent != replica.extent.index_ ||
        memberships_[node].offset != replica.page_offset) {
        return false;
    }
    return true;
}

bool HostKVExtentStore::can_release_page_replicas(
    std::span<const HostKVPageReplicaRelease> releases) const noexcept {
    begin_release_marks();
    for (const HostKVPageReplicaRelease& release : releases) {
        if (release.pages == nullptr || !mark_release(*release.pages, release.page)) {
            return false;
        }
    }
    return true;
}

bool HostKVExtentStore::can_release_page_replicas(
    LogicalKVPageStore& pages, std::span<const LogicalKVPageHandle> releases) const noexcept {
    begin_release_marks();
    for (const LogicalKVPageHandle release : releases) {
        if (!mark_release(pages, release)) { return false; }
    }
    return true;
}

bool HostKVExtentStore::release_page_replicas(std::span<const HostKVPageReplicaRelease> releases) {
    if (!can_release_page_replicas(releases)) { return false; }
    release_marked_extents();
    return true;
}

bool HostKVExtentStore::release_page_replicas(LogicalKVPageStore& pages,
                                              std::span<const LogicalKVPageHandle> releases) {
    if (!can_release_page_replicas(pages, releases)) { return false; }
    release_marked_extents();
    return true;
}

void HostKVExtentStore::begin_release_marks() const noexcept {
    affected_extents_.clear();
    ++release_stamp_;
    if (release_stamp_ == 0) {
        std::fill(release_marks_.begin(), release_marks_.end(), 0);
        std::fill(extent_marks_.begin(), extent_marks_.end(), 0);
        release_stamp_ = 1;
    }
}

bool HostKVExtentStore::mark_release(LogicalKVPageStore& pages,
                                     LogicalKVPageHandle page) const noexcept {
    if (!can_release_page_replica(pages, page)) { return false; }
    const HostKVPageReplica replica = pages.host_replica(page);
    if (!valid(replica.extent) || replica.membership_node >= memberships_.size()) { return false; }
    const Membership& membership = memberships_[replica.membership_node];
    if (membership.page != page || membership.extent != replica.extent.index_ ||
        membership.offset != replica.page_offset ||
        replica.page_offset >= extents_[replica.extent.index_].page_count ||
        extents_[replica.extent.index_].page_store != &pages) {
        return false;
    }
    if (release_marks_[replica.membership_node] == release_stamp_) { return false; }
    release_marks_[replica.membership_node] = release_stamp_;
    if (extent_marks_[replica.extent.index_] != release_stamp_) {
        extent_marks_[replica.extent.index_] = release_stamp_;
        affected_extents_.push_back(replica.extent.index_);
    }
    return true;
}

void HostKVExtentStore::release_marked_extents() {
    for (const std::uint32_t index : affected_extents_) partition_extent(index);
}

void HostKVExtentStore::partition_extent(std::uint32_t index) {
    if (index >= extents_.size()) {
        throw std::logic_error("Host KV partition extent index is out of range");
    }
    Extent& original = extents_[index];
    if (original.state != ExtentState::Published || original.page_store == nullptr ||
        !original.allocation || original.page_count == 0 ||
        original.allocation->page_count() != original.page_count) {
        throw std::logic_error("Host KV extent is not partitionable");
    }
    LogicalKVPageStore* const pages = original.page_store;
    const HostKVExtentCapability old(this, index, original.generation);

    partition_runs_.clear();
    std::uint32_t node          = original.head;
    bool any_release            = false;
    std::uint32_t retained_runs = 0;
    for (std::uint32_t offset = 0; offset < original.page_count; ++offset) {
        if (node == kInvalidIndex || node >= memberships_.size()) {
            throw std::logic_error("Host KV extent membership is truncated");
        }
        const Membership& entry = memberships_[node];
        if (!pages->valid(entry.page) || !pages->host_resident(entry.page)) {
            throw std::logic_error("Host KV extent membership is stale");
        }
        const HostKVPageReplica replica = pages->host_replica(entry.page);
        if (entry.extent != index || entry.offset != offset || replica.extent != old ||
            replica.page_offset != offset || replica.membership_node != node ||
            replica.content_epoch != entry.epoch || replica.committed_columns != entry.coverage) {
            throw std::logic_error("Host KV extent membership disagrees with its replica");
        }
        const bool release = release_marks_[node] == release_stamp_;
        if (release &&
            (pages->require(entry.page).host_readers != 0 ||
             (!pages->device_resident(entry.page) && pages->history_references(entry.page) != 0))) {
            throw std::logic_error("Host KV partition contains an unreleasable page");
        }
        if (partition_runs_.empty() || partition_runs_.back().release != release) {
            partition_runs_.push_back(PartitionRun{
                .begin = offset, .head = node, .tail = node, .count = 1, .release = release});
            if (!release) { ++retained_runs; }
        } else {
            PartitionRun& run = partition_runs_.back();
            run.tail          = node;
            ++run.count;
        }
        if (release) { any_release = true; }
        node = entry.next;
    }
    if (node != kInvalidIndex) {
        throw std::logic_error("Host KV extent membership exceeds its page count");
    }
    if (!any_release) { return; }
    const std::uint32_t additional_extents = retained_runs == 0 ? 0U : retained_runs - 1U;
    if (additional_extents > free_count_) {
        throw std::logic_error("Host KV partition descriptor capacity is exhausted");
    }
    HostKVAllocation remaining = std::move(*original.allocation);
    original.allocation.reset();
    increment_generation(original.generation);
    bool original_assigned = false;
    for (std::size_t run_index = 0; run_index < partition_runs_.size(); ++run_index) {
        const PartitionRun& run = partition_runs_[run_index];
        HostKVAllocation allocation;
        if (run_index + 1U == partition_runs_.size()) {
            allocation = std::move(remaining);
        } else {
            auto split = arena_->split(std::move(remaining), run.count);
            allocation = std::move(split.first);
            remaining  = std::move(split.second);
        }
        memberships_[run.tail].next = kInvalidIndex;

        if (run.release) {
            std::uint32_t release_node = run.head;
            for (std::uint32_t offset = 0; offset < run.count; ++offset) {
                if (release_node == kInvalidIndex) { std::terminate(); }
                Membership& entry        = memberships_[release_node];
                const std::uint32_t next = entry.next;
                if (!pages->detach_host_replica(entry.page, old)) { std::terminate(); }
                entry                                       = {};
                free_memberships_[free_membership_count_++] = release_node;
                release_node                                = next;
            }
            if (release_node != kInvalidIndex || !allocation.release()) { std::terminate(); }
            continue;
        }

        const std::uint32_t target_index = !original_assigned ? index : free_[--free_count_];
        Extent& target                   = extents_[target_index];
        if (target_index != index && target.state != ExtentState::Free) { std::terminate(); }
        original_assigned = true;
        target.state      = ExtentState::Published;
        target.page_store = pages;
        target.allocation.emplace(std::move(allocation));
        target.head       = run.head;
        target.tail       = run.tail;
        target.page_count = run.count;
        const HostKVExtentCapability replacement(this, target_index, target.generation);
        std::uint32_t retained_node = run.head;
        for (std::uint32_t offset = 0; offset < run.count; ++offset) {
            if (retained_node == kInvalidIndex) { std::terminate(); }
            Membership& entry        = memberships_[retained_node];
            const std::uint32_t next = entry.next;
            pages->rebind_host_replica(entry.page,
                                       HostKVPageReplica{.extent            = old,
                                                         .page_offset       = run.begin + offset,
                                                         .membership_node   = retained_node,
                                                         .content_epoch     = entry.epoch,
                                                         .committed_columns = entry.coverage},
                                       HostKVPageReplica{.extent            = replacement,
                                                         .page_offset       = offset,
                                                         .membership_node   = retained_node,
                                                         .content_epoch     = entry.epoch,
                                                         .committed_columns = entry.coverage});
            entry.extent  = target_index;
            entry.offset  = offset;
            retained_node = next;
        }
        if (retained_node != kInvalidIndex) { std::terminate(); }
    }

    if (!original_assigned) {
        original.state      = ExtentState::Free;
        original.page_store = nullptr;
        original.allocation.reset();
        original.head        = kInvalidIndex;
        original.tail        = kInvalidIndex;
        original.page_count  = 0;
        free_[free_count_++] = index;
    }
}

void HostKVExtentStore::release_descriptor(std::uint32_t index, Extent& extent) noexcept {
    std::uint32_t node = extent.head;
    for (std::uint32_t offset = 0; offset < extent.page_count; ++offset) {
        if (node == kInvalidIndex) { std::terminate(); }
        const std::uint32_t next                    = memberships_[node].next;
        memberships_[node]                          = {};
        free_memberships_[free_membership_count_++] = node;
        node                                        = next;
    }
    if (node != kInvalidIndex) { std::terminate(); }
    extent.state      = ExtentState::Free;
    extent.page_store = nullptr;
    extent.allocation.reset();
    extent.head       = kInvalidIndex;
    extent.tail       = kInvalidIndex;
    extent.page_count = 0;
    increment_generation(extent.generation);
    free_[free_count_++] = index;
}

} // namespace ninfer::models::context
