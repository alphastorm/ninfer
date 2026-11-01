// Host-side transport/reference diagnostic, not a transformer or CUDA-kernel emulator.
// Real pool layout, allocation and pitched D2H/H2D copies; only CUDA transport is interposed.
#include "core/host_kv_arena.h"
#include "core/paged_kv_cache.h"
#include "cuda_interpose.h"
#include "ops/kv_cache/kernels/rk4v4.cuh"
#include "ops/rk4v4_reference.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

#define PAGE_CUDA(name) NINFER_TEST_CUDA_WRAP(name)
extern "C" {
cudaError_t CUDARTAPI PAGE_CUDA(cudaDeviceGetAttribute)(int* value, cudaDeviceAttr attr, int) {
    if (attr != cudaDevAttrMaxPitch) return cudaErrorInvalidValue;
    *value = std::numeric_limits<int>::max(); return cudaSuccess;
}
cudaError_t CUDARTAPI PAGE_CUDA(cudaMallocHost)(void** out, std::size_t bytes) {
    *out = ::operator new(bytes, std::align_val_t{256}, std::nothrow);
    return *out ? cudaSuccess : cudaErrorMemoryAllocation;
}
cudaError_t CUDARTAPI PAGE_CUDA(cudaFreeHost)(void* data) {
    ::operator delete(data, std::align_val_t{256}); return cudaSuccess;
}
cudaError_t CUDARTAPI PAGE_CUDA(cudaMemcpy2DAsync)(void* out, std::size_t opitch,
    const void* in, std::size_t ipitch, std::size_t width, std::size_t height,
    cudaMemcpyKind, cudaStream_t) {
    if (width > opitch || width > ipitch) return cudaErrorInvalidValue;
    for (std::size_t row = 0; row < height; ++row)
        std::memcpy(static_cast<std::byte*>(out) + row * opitch,
                    static_cast<const std::byte*>(in) + row * ipitch, width);
    return cudaSuccess;
}
}

namespace {
using namespace ninfer;
namespace codec = ninfer::ops::detail::kv;
constexpr unsigned layers = 16, heads = 4, prefix = 3066, frontier = 3093, pages = 49;
void require(bool value, const char* label) { if (!value) throw std::runtime_error(label); }

KVPageGeometry geometry() {
    KVPageGeometry result;
    const auto layout = paged_kv_storage_layout(KvCacheStorage::Rk4V4, 256);
    for (unsigned layer = 0; layer < layers; ++layer) {
        result.planes.push_back({layout.key.data_dtype, layout.key.data_leading_extent, heads});
        result.planes.push_back({layout.value.data_dtype, layout.value.data_leading_extent, heads});
        result.planes.push_back({layout.key.scale_dtype, layout.key.scale_leading_extent, heads});
        result.planes.push_back({layout.value.scale_dtype, layout.value.scale_leading_extent, heads});
    }
    return result;
}
struct AlignedDelete {
    void operator()(std::byte* p) const { ::operator delete(p, std::align_val_t{256}); }
};
struct Pool {
    LayoutBuilder builder;
    DeviceKVPagePoolLayout layout = plan_device_kv_page_pool(builder, {pages, geometry()});
    std::size_t bytes = builder.finish(256);
    std::unique_ptr<std::byte, AlignedDelete> memory{
        static_cast<std::byte*>(::operator new(bytes, std::align_val_t{256}))};
    DeviceKVPagePool physical{{memory.get(), bytes, Device::cuda(0)}, layout};
    std::vector<DeviceKVPageLease> leases;
    std::vector<DeviceKVPageHandle> handles;
    explicit Pool(bool reverse) {
        std::memset(memory.get(), 0xa5, bytes);
        auto reserved = physical.reserve(pages);
        require(reserved.has_value(), "page reservation");
        leases.reserve(pages);
        physical.materialize(*reserved, pages, leases);
        for (const auto& lease : leases) handles.push_back(lease.handle());
        if (reverse) std::reverse(handles.begin(), handles.end());
    }
    std::byte* row(unsigned layer, unsigned kind, unsigned head, unsigned token) {
        const auto p = physical.page_plane(handles.at(token / 64), layer * 4 + kind);
        return static_cast<std::byte*>(p.data) + head * p.nb[2] + (token % 64) * p.nb[1];
    }
};
using Oracle = std::array<std::array<rk4_reference::Row, 2>, 37>;
Oracle oracle() {
    Oracle result;
    for (unsigned i = 0; i < result.size(); ++i)
        for (unsigned kind = 0; kind < 2; ++kind) {
            std::array<double, 256> input{};
            for (unsigned d = 0; d < input.size(); ++d)
                input[d] = (int((d * 17 + i * 13 + kind * 7) % 61) - 30) / 16.0;
            result[i][kind] = rk4_reference::encode(input, kind == 0);
        }
    return result;
}
const rk4_reference::Row& expected(const Oracle& rows, unsigned layer, unsigned head,
                                  unsigned token, unsigned kind) {
    return rows[(token + 7 * layer + 11 * head) % rows.size()][kind];
}
void append(Pool& pool, const Oracle& rows, unsigned begin, unsigned end) {
    for (unsigned token = begin; token < end; ++token)
        for (unsigned layer = 0; layer < layers; ++layer)
            for (unsigned head = 0; head < heads; ++head)
                for (unsigned kind = 0; kind < 2; ++kind) {
                    const auto& row = expected(rows, layer, head, token, kind);
                    std::memcpy(pool.row(layer, kind, head, token), row.codes.data(), 128);
                    std::memcpy(pool.row(layer, kind + 2, head, token), row.scales.data(), kind ? 16 : 8);
                }
}
void canonicalize_tail(HostKVAllocationView view, unsigned tokens) {
    // checkpoint_api.cpp exports only committed rows into zeroed page records.
    // Match that canonical representation after the pool's full-page D2H copy.
    const auto& layout = view.layout();
    for (unsigned page = 0; page < view.page_count(); ++page) {
        const unsigned committed = std::min(64U, tokens > page * 64 ? tokens - page * 64 : 0U);
        for (unsigned p = 0; p < layout.planes.size(); ++p) {
            const auto& packed = layout.planes[p];
            const std::size_t row_bytes = packed.head_payload_bytes / 64;
            for (unsigned head = 0; head < heads; ++head)
                std::memset(view.data() + page * layout.page_stride + packed.offset +
                    head * packed.head_payload_bytes + committed * row_bytes, 0,
                    (64 - committed) * row_bytes);
        }
    }
}
void verify_reads(Pool& pool, const Oracle& rows) {
    for (unsigned token = 0; token < frontier; ++token)
        for (unsigned layer = 0; layer < layers; ++layer)
            for (unsigned head = 0; head < heads; ++head)
                for (unsigned kind = 0; kind < 2; ++kind) {
                    const auto& ref = expected(rows, layer, head, token, kind);
                    const auto* codes = reinterpret_cast<const std::uint8_t*>(pool.row(layer, kind, head, token));
                    for (unsigned d = 0; d < 256; ++d) {
                        const int index = (codes[d / 2] >> (4 * (d % 2))) & 15;
                        const int code = kind ? codec::rk4_value_code(index) : codec::rk4_key_code(index);
                        std::uint16_t scale;
                        std::memcpy(&scale, pool.row(layer, kind + 2, head, token) + 2 * (d / (kind ? 32 : 64)), 2);
                        require(code * rk4_reference::half(scale) == ref.decoded[d],
                                "production codebook read differs from independent FP64 row");
                    }
                }
}
void exercise() {
    const auto rows = oracle();
    Pool cold(false), restored(true);
    for (unsigned begin = 0; begin < frontier; begin += 1024)
        append(cold, rows, begin, std::min(frontier, begin + 1024));
    // A different final prefill width: 1018 before save, then 27 after restart.
    for (unsigned begin = 0; begin < prefix; begin += 1024)
        append(restored, rows, begin, std::min(prefix, begin + 1024));
    const auto layout = plan_host_kv_page_layout(geometry());
    require(layout.page_stride == 1146880, "16-layer / 4-head packed page stride");
    HostContextArena arena(2 * pages * layout.page_stride, layout.page_stride);
    HostKVArena host(arena, std::span(&layout, 1));
    auto saved = host.allocate(layout, pages), comparison = host.allocate(layout, pages);
    require(saved && comparison, "host records allocated");
    auto bytes = host.writable_view(*saved);
    constexpr unsigned saved_pages = (prefix + 63) / 64;
    auto saved_bytes = bytes.subview(0, saved_pages);
    (void)restored.physical.copy_to_host(std::span(restored.handles).first(saved_pages), saved_bytes);
    canonicalize_tail(saved_bytes, prefix);
    auto other = host.writable_view(*comparison);
    auto compared_bytes = other.subview(0, saved_pages);
    (void)cold.physical.copy_to_host(std::span(cold.handles).first(saved_pages), compared_bytes);
    canonicalize_tail(compared_bytes, prefix);
    const auto count = pages * layout.page_stride;
    const auto saved_count = saved_pages * layout.page_stride;
    require(saved_count == 55050240, "T1 checkpoint KV payload byte count");
    require(std::memcmp(bytes.data(), other.data(), saved_count) == 0,
            "cold 1024 chunks and 3066-token prefix have different committed page bytes");

    // The native checkpoint boundary stores this opaque representation; no codec is involved.
    struct TemporaryFile {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("ninfer-rk4-page-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~TemporaryFile() { std::error_code error; std::filesystem::remove(path, error); }
    } temporary;
    {
        std::ofstream file(temporary.path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), saved_count);
        file.close();
        require(file.good(), "checkpoint byte write");
    }
    std::memset(bytes.data(), 0x5a, saved_count);
    {
        std::ifstream file(temporary.path, std::ios::binary);
        file.read(reinterpret_cast<char*>(bytes.data()), saved_count);
        require(file.good() && file.gcount() == static_cast<std::streamsize>(saved_count),
                "checkpoint byte read");
    }
    require(std::memcmp(bytes.data(), other.data(), saved_count) == 0, "checkpoint byte roundtrip");
    std::memset(restored.memory.get(), 0x3c, restored.bytes);
    (void)restored.physical.copy_from_host(saved_bytes, std::span(restored.handles).first(saved_pages));
    append(restored, rows, prefix, frontier);
    (void)restored.physical.copy_to_host(restored.handles, bytes);
    (void)cold.physical.copy_to_host(cold.handles, other);
    canonicalize_tail(bytes, frontier); canonicalize_tail(other, frontier);
    require(std::memcmp(bytes.data(), other.data(), count) == 0,
            "restored prefix plus 27-token append differs from cold 3093-token cache");
    verify_reads(cold, rows); verify_reads(restored, rows);
    // A one-bit corruption is observable, rather than hidden by a tolerance or decoder equality.
    bytes.data()[layout.planes[2].offset] ^= std::byte{1};
    require(std::memcmp(bytes.data(), other.data(), count) != 0, "scale corruption sensitivity");
    std::cout << "PASS 3093-token / 3066+27 reference pages; 64 planes, 4 heads, reversed page IDs; "
                 "raw D2H/file/H2D and all decoded coordinates exact (identical K/V inputs only)\n";
}
}
int main() {
    try { exercise(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
