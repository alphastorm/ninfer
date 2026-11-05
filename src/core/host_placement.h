#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ninfer {

// One shape of a Host context write: `count` allocations of `bytes` each, a page stride or one
// StateImage. A run is reserved as the Host KV extent store reserves pages: the whole group
// first, halving it until a group fits, each group first-fit in one contiguous free extent. So a
// run fits only when the free extents can hold it that way, not when free bytes add up to its
// total.
struct HostAllocationRun {
    std::size_t bytes   = 0;
    std::uint32_t count = 0;
    friend bool operator==(const HostAllocationRun&, const HostAllocationRun&) noexcept = default;
};

// The byte total of a set of runs: a ranking quantity, never a room test.
[[nodiscard]] constexpr std::size_t total_bytes(std::span<const HostAllocationRun> runs) noexcept {
    std::size_t total = 0;
    for (const auto& run : runs) { total += run.bytes * run.count; }
    return total;
}

// A byte range of the shared Host context arena.
struct HostRange {
    std::size_t offset = 0;
    std::size_t bytes  = 0;
};

} // namespace ninfer
