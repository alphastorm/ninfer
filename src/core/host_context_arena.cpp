#include "core/host_context_arena.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ninfer {

HostContextAllocation::~HostContextAllocation() { (void)release(); }

HostContextAllocation::HostContextAllocation(HostContextAllocation&& other) noexcept
    : owner_(other.owner_), offset_(other.offset_), bytes_(other.bytes_),
      reserved_(other.reserved_) {
    other.disarm();
}

HostContextAllocation& HostContextAllocation::operator=(HostContextAllocation&& other) noexcept {
    if (this == &other) { return *this; }
    (void)release();
    owner_    = other.owner_;
    offset_   = other.offset_;
    bytes_    = other.bytes_;
    reserved_ = other.reserved_;
    other.disarm();
    return *this;
}

std::byte* HostContextAllocation::data() const noexcept {
    return valid() ? static_cast<std::byte*>(owner_->backing_->data()) + offset_ : nullptr;
}

void HostContextAllocation::publish() noexcept {
    if (!reserved()) { return; }
    owner_->reserved_bytes_ -= bytes_;
    reserved_ = false;
}

bool HostContextAllocation::release() noexcept {
    if (!valid()) { return false; }
    owner_->release(offset_, bytes_, reserved_);
    disarm();
    return true;
}

void HostContextAllocation::disarm() noexcept {
    owner_    = nullptr;
    offset_   = 0;
    bytes_    = 0;
    reserved_ = false;
}

namespace {
std::size_t checked_minimum(std::size_t minimum_allocation_bytes) {
    if (minimum_allocation_bytes == 0 ||
        minimum_allocation_bytes >
            std::numeric_limits<std::size_t>::max() - (HostContextArena::alignment - 1)) {
        throw std::invalid_argument("Host context minimum allocation is invalid");
    }
    return minimum_allocation_bytes;
}
} // namespace

HostContextArena::HostContextArena(std::size_t capacity_bytes, std::size_t minimum_allocation_bytes)
    : capacity_bytes_(capacity_bytes), free_(alignment, checked_minimum(minimum_allocation_bytes)) {
    if (capacity_bytes_ == 0) { return; }
    // At most N allocated extents separate N+1 free runs; releases and splits never allocate
    // metadata. The unused trailing bytes remain part of the charged physical backing.
    free_.reset(capacity_bytes_);
    backing_.emplace(capacity_bytes_);
}

bool HostContextArena::can_allocate(std::size_t bytes) const noexcept {
    const std::size_t rounded = free_.allocation_bytes(bytes);
    return rounded != 0 && free_.find(rounded).has_value();
}

std::optional<HostContextAllocation> HostContextArena::allocate(std::size_t bytes) noexcept {
    const std::size_t rounded = free_.allocation_bytes(bytes);
    if (rounded == 0) { return std::nullopt; }
    const auto offset = free_.take(rounded);
    if (!offset) { return std::nullopt; }
    occupied_bytes_ += rounded;
    reserved_bytes_ += rounded;
    peak_occupied_bytes_ = std::max(peak_occupied_bytes_, occupied_bytes_);
    ++allocation_count_;
    return HostContextAllocation(*this, *offset, rounded);
}

std::pair<HostContextAllocation, HostContextAllocation>
HostContextArena::split(HostContextAllocation&& allocation, std::size_t byte_offset) {
    if (allocation.owner_ != this) {
        throw std::invalid_argument("Cannot split a foreign or released Host context allocation");
    }
    if (byte_offset % alignment != 0 || byte_offset < free_.minimum_bytes() ||
        byte_offset > allocation.bytes_ || allocation.bytes_ - byte_offset < free_.minimum_bytes()) {
        throw std::out_of_range("Host context split must retain two aligned complete geometries");
    }
    HostContextAllocation left(*this, allocation.offset_, byte_offset, allocation.reserved_);
    HostContextAllocation right(*this, allocation.offset_ + byte_offset,
                                allocation.bytes_ - byte_offset, allocation.reserved_);
    allocation.disarm();
    ++allocation_count_;
    return {std::move(left), std::move(right)};
}

void HostContextArena::release(std::size_t offset, std::size_t bytes, bool reserved) noexcept {
    occupied_bytes_ -= bytes;
    if (reserved) { reserved_bytes_ -= bytes; }
    --allocation_count_;
    free_.insert({offset, bytes});
}

bool HostContextArena::can_place(std::span<const HostAllocationRun> runs,
                                std::span<const HostRange> freed) const {
    return free_.can_place(runs, freed);
}

std::uint64_t HostContextArena::placeable_count(std::size_t bytes) const noexcept {
    return free_.placeable_count(bytes);
}

HostRange HostContextArena::range_of(const std::byte* data, std::size_t bytes) const {
    if (!backing_) throw std::out_of_range("Host context arena has no backing");
    const auto base = reinterpret_cast<std::uintptr_t>(backing_->data());
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    if (address < base || address - base > capacity_bytes_ ||
        bytes > capacity_bytes_ - (address - base)) {
        throw std::out_of_range("Host range is outside the context arena");
    }
    return {address - base, bytes};
}

} // namespace ninfer
