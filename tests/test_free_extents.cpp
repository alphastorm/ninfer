#include "core/free_extents.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

using Run = ninfer::HostAllocationRun;
using ninfer::FreeExtents;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <class... Runs>
std::array<Run, sizeof...(Runs)> runs(Runs... run) {
    return {{run...}};
}

constexpr std::size_t u = 256;

void test_rounding_and_first_fit() {
    FreeExtents free(u, u);
    free.reset(8 * u);
    expect(free.allocation_bytes(0) == 0 && free.allocation_bytes(1) == u &&
               free.allocation_bytes(u + 1) == 2 * u && free.free_bytes() == 8 * u,
           "requests round up to the alignment and never below the minimum");
    FreeExtents small(u, 2 * u);
    expect(small.allocation_bytes(u) == 0 && small.minimum_bytes() == 2 * u,
           "a request below the minimum geometry is refused rather than clamped");
    const auto a = free.take(u);
    const auto b = free.take(2 * u);
    const auto c = free.take(u);
    expect(a == 0 && b == u && c == 3 * u && free.free_bytes() == 4 * u &&
               free.extents().size() == 1 && free.extents()[0] == FreeExtents::Extent{4 * u, 4 * u},
           "take carves from the start of the first extent that fits");
    free.insert({*b, 2 * u});
    expect(free.extents().size() == 2 && free.extents()[0] == FreeExtents::Extent{u, 2 * u},
           "a returned extent between live ones stays separate");
    free.insert({*a, u});
    expect(free.extents()[0] == FreeExtents::Extent{0, 3 * u},
           "a returned extent coalesces with its free neighbour");
    free.insert({*c, u});
    expect(free.extents().size() == 1 && free.extents()[0] == FreeExtents::Extent{0, 8 * u},
           "returning the last live extent restores one complete extent");
}

void test_placement_follows_contiguous_extents() {
    FreeExtents free(u, u);
    free.reset(8 * u);
    (void)free.take(u);              // [0, 1u) live
    const auto b = free.take(2 * u); // [1u, 3u) live
    (void)free.take(u);              // [3u, 4u) live
    (void)free.take(4 * u);          // [4u, 8u) live
    expect(!free.can_place(runs(Run{u, 1})) && free.placeable_count(u) == 0,
           "a full space places nothing");
    free.insert({*b, 2 * u});
    expect(free.free_bytes() == 2 * u && free.can_place(runs(Run{2 * u, 1})) &&
               !free.can_place(runs(Run{3 * u, 1})),
           "one free extent places exactly what it holds");
    const std::array<ninfer::HostRange, 1> returning{{{0, u}}};
    expect(free.can_place(runs(Run{3 * u, 1}), returning) &&
               !free.can_place(runs(Run{3 * u + 1, 1}), returning),
           "a returning range coalesces with its free neighbour before placement");
    free.insert({4 * u, 4 * u});
    expect(free.free_bytes() == 6 * u && !free.can_place(runs(Run{6 * u, 1})),
           "free bytes split across extents do not place as one allocation");
    expect(free.can_place(runs(Run{2 * u, 3})) && !free.can_place(runs(Run{2 * u, 4})) &&
               free.placeable_count(2 * u) == 3,
           "a run fills each extent in order and places exactly the count the extents hold");
    expect(free.can_place(runs(Run{4 * u, 1}, Run{2 * u, 1})) &&
               free.can_place(runs(Run{2 * u, 1}, Run{4 * u, 1})) &&
               !free.can_place(runs(Run{2 * u, 2}, Run{4 * u, 1})),
           "runs are placed in order and an earlier run can consume the extent a later one needs");
    expect(free.can_place(runs(Run{u, 0})) && !free.can_place(runs(Run{0, 1})) &&
               free.placeable_count(0) == 0,
           "an empty run always places and a zero-byte allocation never does");
}

void test_runs_reserve_as_halving_groups() {
    // Free extents of 3u and 7u. A run of two 3u pages is reserved as one 6u group in the 7u
    // extent, leaving 1u there; two 2u pages then find room for only one. Placing the pages
    // one by one would have fit everything, so the simulation must follow the reservation.
    FreeExtents free(u, u);
    free.reset(16 * u);
    (void)free.take(3 * u);
    (void)free.take(6 * u);
    (void)free.take(7 * u);
    free.insert({0, 3 * u});
    free.insert({9 * u, 7 * u});
    expect(free.placeable_count(3 * u) == 3 && free.placeable_count(2 * u) == 4,
           "page counts are conserved under any grouping of equal pages");
    expect(!free.can_place(runs(Run{3 * u, 2}, Run{2 * u, 2})) &&
               free.can_place(runs(Run{2 * u, 2}, Run{3 * u, 2})),
           "a grouped reservation can leave later runs less room than page-wise placement");
    const auto refused = free.take_run(Run{8 * u, 1});
    expect(!refused && free.free_bytes() == 10 * u && free.extents().size() == 2,
           "a run that cannot be reserved takes nothing");
    const auto taken = free.take_run(Run{3 * u, 2});
    expect(taken && *taken == std::vector<FreeExtents::Extent>{{9 * u, 6 * u}} &&
               free.extents().size() == 2 && free.extents()[1] == FreeExtents::Extent{15 * u, u},
           "the whole group is reserved first, in the first extent that holds it");
    const auto halved = free.take_run(Run{2 * u, 2});
    expect(!halved, "a run halves down to single pages and still fails when one cannot fit");
    const auto one = free.take_run(Run{2 * u, 1});
    expect(one && *one == std::vector<FreeExtents::Extent>{{0, 2 * u}} &&
               free.free_bytes() == 2 * u,
           "a single page reserves in the first extent that holds it");
}
} // namespace

int main() {
    try {
        test_rounding_and_first_fit();
        test_placement_follows_contiguous_extents();
        test_runs_reserve_as_halving_groups();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
