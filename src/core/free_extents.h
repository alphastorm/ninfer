#pragma once

#include "core/host_placement.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ninfer {

// The ordered, coalesced free extents of one address space and the first-fit placement the Host
// context arena performs on them. It owns no memory, so a planner or a test can run the exact
// allocation sequence a pinned backing would see.
class FreeExtents {
public:
    struct Extent {
        std::size_t offset                                            = 0;
        std::size_t bytes                                             = 0;
        friend bool operator==(const Extent&, const Extent&) noexcept = default;
    };

    // Allocations round up to `alignment`, a power of two, and must reach `minimum_bytes`.
    FreeExtents(std::size_t alignment, std::size_t minimum_bytes) noexcept
        : alignment_(alignment),
          minimum_bytes_((minimum_bytes + alignment - 1) & ~(alignment - 1)) {}

    // One free extent covering [0, capacity_bytes). At most N allocated extents separate N+1
    // free runs, so the metadata for that many is reserved once; inserts never grow it.
    void reset(std::size_t capacity_bytes) {
        extents_.clear();
        if (capacity_bytes == 0) { return; }
        extents_.reserve(capacity_bytes / minimum_bytes_ + 1);
        extents_.push_back({0, capacity_bytes});
    }

    [[nodiscard]] std::size_t minimum_bytes() const noexcept { return minimum_bytes_; }

    [[nodiscard]] std::span<const Extent> extents() const noexcept { return extents_; }

    [[nodiscard]] std::size_t free_bytes() const noexcept {
        std::size_t total = 0;
        for (const auto& extent : extents_) { total += extent.bytes; }
        return total;
    }

    // The rounded size `bytes` occupies, or 0 when the request is empty, overflows or stays
    // below the minimum: such a request is never allocated.
    [[nodiscard]] std::size_t allocation_bytes(std::size_t bytes) const noexcept {
        if (bytes == 0 || bytes > std::numeric_limits<std::size_t>::max() - (alignment_ - 1)) {
            return 0;
        }
        const std::size_t rounded = (bytes + alignment_ - 1) & ~(alignment_ - 1);
        return rounded < minimum_bytes_ ? 0 : rounded;
    }

    // The index of the first extent holding `rounded` bytes.
    [[nodiscard]] std::optional<std::size_t> find(std::size_t rounded) const noexcept {
        for (std::size_t index = 0; index < extents_.size(); ++index) {
            if (extents_[index].bytes >= rounded) { return index; }
        }
        return std::nullopt;
    }

    // Carves `rounded` bytes from the start of the first extent holding them.
    [[nodiscard]] std::optional<std::size_t> take(std::size_t rounded) noexcept {
        const auto index = find(rounded);
        if (!index) { return std::nullopt; }
        Extent& extent           = extents_[*index];
        const std::size_t offset = extent.offset;
        extent.offset += rounded;
        extent.bytes -= rounded;
        if (extent.bytes == 0) {
            extents_.erase(extents_.begin() + static_cast<std::ptrdiff_t>(*index));
        }
        return offset;
    }

    // Returns an extent, coalescing it with its neighbours.
    void insert(Extent extent) {
        const auto position = std::lower_bound(
            extents_.begin(), extents_.end(), extent.offset,
            [](const Extent& candidate, std::size_t offset) { return candidate.offset < offset; });
        auto inserted = extents_.insert(position, extent);
        if (inserted != extents_.begin()) {
            auto previous = inserted - 1;
            if (previous->offset + previous->bytes == inserted->offset) {
                previous->bytes += inserted->bytes;
                extents_.erase(inserted);
                inserted = previous;
            }
        }
        const auto next = inserted + 1;
        if (next != extents_.end() && inserted->offset + inserted->bytes == next->offset) {
            inserted->bytes += next->bytes;
            extents_.erase(next);
        }
    }

    // Takes a run as the Host KV extent store reserves pages: the whole remaining group first,
    // halving the group until one fits, down to a single allocation. All or nothing.
    [[nodiscard]] std::optional<std::vector<Extent>> take_run(HostAllocationRun run) {
        std::vector<Extent> taken;
        if (run.count == 0) { return taken; }
        FreeExtents trial = *this;
        if (!trial.take_run_in_place(run, &taken)) { return std::nullopt; }
        *this = std::move(trial);
        return taken;
    }

    // Whether every run can be taken, in order, once the `freed` ranges have returned.
    [[nodiscard]] bool can_place(std::span<const HostAllocationRun> runs,
                                 std::span<const HostRange> freed = {}) const {
        FreeExtents trial(alignment_, minimum_bytes_);
        trial.extents_ = returned(freed);
        for (const auto& run : runs) {
            if (!trial.take_run_in_place(run, nullptr)) { return false; }
        }
        return true;
    }

    // How many allocations of `bytes` the extents hold, filling each in order.
    [[nodiscard]] std::uint64_t placeable_count(std::size_t bytes) const noexcept {
        const std::size_t rounded = allocation_bytes(bytes);
        if (rounded == 0) { return 0; }
        std::uint64_t count = 0;
        for (const auto& extent : extents_) { count += extent.bytes / rounded; }
        return count;
    }

private:
    // The caller owns the simulation/rollback copy. A feasibility query neither copies it
    // again for each run nor constructs an unused allocation-result vector.
    [[nodiscard]] bool take_run_in_place(HostAllocationRun run, std::vector<Extent>* taken) {
        if (run.count == 0) { return true; }
        if (allocation_bytes(run.bytes) == 0) { return false; }
        std::uint32_t remaining = run.count;
        while (remaining) {
            std::uint32_t group = remaining;
            std::optional<std::size_t> offset;
            std::size_t group_bytes = 0;
            while (group) {
                group_bytes = run.bytes > std::numeric_limits<std::size_t>::max() / group
                                  ? 0
                                  : allocation_bytes(run.bytes * group);
                if (group_bytes && (offset = take(group_bytes))) { break; }
                group /= 2;
            }
            if (!offset) { return false; }
            if (taken) { taken->push_back({*offset, group_bytes}); }
            remaining -= group;
        }
        return true;
    }

    // The extents after the `freed` ranges return: one ordered merge, coalescing neighbours as
    // insert() does, so a large return costs one pass rather than one insert per range.
    [[nodiscard]] std::vector<Extent> returned(std::span<const HostRange> freed) const {
        std::vector<Extent> coming;
        coming.reserve(freed.size());
        for (const auto& range : freed) {
            if (range.bytes) { coming.push_back({range.offset, range.bytes}); }
        }
        std::sort(coming.begin(), coming.end(),
                  [](const Extent& a, const Extent& b) { return a.offset < b.offset; });
        std::vector<Extent> merged;
        merged.reserve(extents_.size() + coming.size());
        const auto append = [&](Extent extent) {
            if (!merged.empty() && merged.back().offset + merged.back().bytes == extent.offset) {
                merged.back().bytes += extent.bytes;
            } else {
                merged.push_back(extent);
            }
        };
        auto current = extents_.begin();
        auto next    = coming.begin();
        while (current != extents_.end() || next != coming.end()) {
            if (next == coming.end() ||
                (current != extents_.end() && current->offset < next->offset)) {
                append(*current++);
            } else {
                append(*next++);
            }
        }
        return merged;
    }

    std::size_t alignment_;
    std::size_t minimum_bytes_;
    std::vector<Extent> extents_;
};

} // namespace ninfer
