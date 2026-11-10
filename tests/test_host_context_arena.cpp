#include "core/host_context_arena.h"
#include "cuda_interpose.h"

#include <cuda_runtime_api.h>

#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <vector>

// The real arena and pinned-buffer owner run unchanged; only CUDA's backing allocation
// boundary is interposed, as in test_export_state. No GPU or driver is needed.
#define NINFER_ARENA_CUDA(name) NINFER_TEST_CUDA_WRAP(name)
extern "C" {
cudaError_t CUDARTAPI NINFER_ARENA_CUDA(cudaMallocHost)(void** memory, std::size_t bytes) {
    *memory = ::operator new(bytes, std::align_val_t{256}, std::nothrow);
    return *memory ? cudaSuccess : cudaErrorMemoryAllocation;
}
cudaError_t CUDARTAPI NINFER_ARENA_CUDA(cudaFreeHost)(void* memory) {
    ::operator delete(memory, std::align_val_t{256});
    return cudaSuccess;
}
const char* CUDARTAPI NINFER_ARENA_CUDA(cudaGetErrorString)(cudaError_t) {
    return "host-test backing allocation failure";
}
}

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void test_reserve_publish_and_rollback() {
    ninfer::HostContextArena arena(1025, 1);
    expect(arena.capacity_bytes() == 1025 && arena.occupied_bytes() == 0,
           "idle fixed backing remains fully charged");
    auto source = arena.allocate(257);
    expect(source && source->bytes() == 512 && arena.reserved_bytes() == 512,
           "allocation includes alignment padding immediately");
    std::memset(source->data(), 0x51, source->bytes());
    source->publish();
    source->publish();
    expect(arena.live_bytes() == 512 && arena.reserved_bytes() == 0,
           "publication charges one unique live allocation");
    {
        auto target = arena.allocate(257);
        expect(target && target->data() != source->data() && arena.occupied_bytes() == 1024 &&
                   arena.live_bytes() == 512 && arena.reserved_bytes() == 512,
               "whole destination is reserved while the source remains resident");
        expect(!arena.allocate(1) && !arena.can_allocate(1),
               "unused trailing byte cannot satisfy aligned allocation");
    }
    expect(arena.occupied_bytes() == 512 && arena.reserved_bytes() == 0 &&
               arena.peak_occupied_bytes() == 1024,
           "cancelled destination rolls back occupancy without losing peak");
    expect(source->data()[0] == std::byte{0x51} && source->data()[511] == std::byte{0x51},
           "failed destination preserves the pinned source");
    auto moved = std::move(*source);
    expect(!source->valid() && arena.allocation_count() == 1,
           "moving ownership neither duplicates nor releases bytes");
    expect(moved.release() && !moved.release() && arena.occupied_bytes() == 0 &&
               arena.capacity_bytes() == 1025,
           "last owner release frees reusable bytes without hiding fixed backing");
    expect(!arena.allocate(0) && !arena.allocate(std::numeric_limits<std::size_t>::max()),
           "zero and overflowing requests fail without changing accounting");
}

void test_fragmentation_and_split() {
    ninfer::HostContextArena arena(2048, 256);
    auto full = arena.allocate(2048);
    full->publish();
    std::vector<ninfer::HostContextAllocation> pieces;
    pieces.reserve(8);
    while (full->bytes() > 256) {
        auto split = arena.split(std::move(*full), 256);
        pieces.push_back(std::move(split.first));
        *full = std::move(split.second);
    }
    pieces.push_back(std::move(*full));
    expect(arena.allocation_count() == 8 && arena.live_bytes() == 2048,
           "split reaches the smallest geometry without changing charged bytes");
    for (std::size_t index = 0; index < pieces.size(); index += 2) {
        (void)pieces[index].release();
    }
    expect(arena.free_bytes() == 1024 && !arena.can_allocate(512) && !arena.allocate(512),
           "actual contiguous extents, not total free bytes, govern availability");
    (void)pieces[1].release();
    auto merged = arena.allocate(768);
    expect(merged.has_value(), "adjacent released split pieces coalesce for another geometry");
    bool invalid_split_rejected = false;
    try {
        (void)arena.split(std::move(*merged), 1);
    } catch (const std::out_of_range&) { invalid_split_rejected = true; }
    expect(invalid_split_rejected && merged->valid() && merged->bytes() == 768,
           "invalid split leaves its source allocation owned");
    pieces.clear();
    merged.reset();
    expect(arena.occupied_bytes() == 0 && arena.allocation_count() == 0 && arena.can_allocate(2048),
           "all split pieces return to one complete reusable extent");
    ninfer::HostContextArena disabled(0, 256);
    expect(!disabled.allocate(256) && disabled.capacity_bytes() == 0,
           "Host zero has no backing or allocations");
}

void test_placement_follows_contiguous_extents() {
    using Run       = ninfer::HostAllocationRun;
    const auto runs = [](auto... run) { return std::array<Run, sizeof...(run)>{{run...}}; };
    ninfer::HostContextArena arena(2048, 256);
    auto a = arena.allocate(256);
    auto b = arena.allocate(512);
    auto c = arena.allocate(256);
    auto d = arena.allocate(1024);
    expect(a && b && c && d && !arena.can_place(runs(Run{256, 1})) &&
               arena.placeable_count(256) == 0,
           "a full backing places nothing");
    (void)b->release();
    expect(arena.free_bytes() == 512 && arena.can_place(runs(Run{512, 1})) &&
               !arena.can_place(runs(Run{768, 1})),
           "one free extent places exactly what it holds");
    const auto returning = arena.range_of(a->data(), a->bytes());
    const std::array<ninfer::HostRange, 1> freed{{returning}};
    expect(returning.offset == 0 && returning.bytes == 256 &&
               arena.can_place(runs(Run{768, 1}), freed) &&
               !arena.can_place(runs(Run{769, 1}), freed),
           "a returning range coalesces with its free neighbour before placement");
    (void)d->release();
    expect(arena.free_bytes() == 1536 && !arena.can_place(runs(Run{1536, 1})) &&
               !arena.can_allocate(1536),
           "free bytes split across extents do not place as one allocation");
    expect(arena.can_place(runs(Run{512, 3})) && !arena.can_place(runs(Run{512, 4})) &&
               arena.placeable_count(512) == 3,
           "equal-size allocations fill each extent in order, as allocate does");
    expect(arena.can_place(runs(Run{1024, 1}, Run{512, 1})) &&
               arena.can_place(runs(Run{512, 1}, Run{1024, 1})) &&
               !arena.can_place(runs(Run{512, 2}, Run{1024, 1})),
           "runs are placed in order and an earlier run can consume the extent a later one needs");
    expect(arena.can_place(runs(Run{256, 0})) && !arena.can_place(runs(Run{0, 1})) &&
               arena.placeable_count(0) == 0,
           "an empty run always places and a zero-byte allocation never does");
    bool outside_rejected = false;
    try {
        (void)arena.range_of(a->data() + 2048, 1);
    } catch (const std::out_of_range&) { outside_rejected = true; }
    expect(outside_rejected, "a range outside the backing is refused");
    (void)a->release();
    (void)c->release();
    expect(arena.can_place(runs(Run{2048, 1})) && arena.placeable_count(2048) == 1,
           "all releases return one complete placeable extent");
    ninfer::HostContextArena disabled(0, 256);
    bool empty_rejected = false;
    try {
        (void)disabled.range_of(nullptr, 0);
    } catch (const std::out_of_range&) { empty_rejected = true; }
    expect(empty_rejected, "a disabled arena has no range to return");
}
} // namespace

int main() {
    try {
        test_reserve_publish_and_rollback();
        test_fragmentation_and_split();
        test_placement_follows_contiguous_extents();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
