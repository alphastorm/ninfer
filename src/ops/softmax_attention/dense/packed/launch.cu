#include "ops/softmax_attention/dense/packed/launch.h"

#include "ops/softmax_attention/kernels/noncausal/kernel.cuh"
#include "core/device.h"

#include <cstdint>
#include <stdexcept>

namespace ninfer::ops::detail {
namespace {

std::int32_t select_uniform_tile(std::int32_t segment_length) {
    // Measured issued-MMA rates are approximately 3:6:8 for the 16/32/64
    // tiles. Minimize padded tile area divided by that rate.
    constexpr std::int32_t tiles[] = {16, 32, 64};
    constexpr std::int32_t rates[] = {3, 6, 8};
    std::int32_t best_tile         = tiles[0];
    std::int64_t best_padded       = ((segment_length + best_tile - 1) / best_tile) * best_tile;
    std::int64_t best_square       = best_padded * best_padded;
    std::int32_t best_rate         = rates[0];
    for (int i = 1; i < 3; ++i) {
        const std::int64_t padded = ((segment_length + tiles[i] - 1) / tiles[i]) * tiles[i];
        const std::int64_t square = padded * padded;
        if (square * best_rate < best_square * rates[i]) {
            best_tile   = tiles[i];
            best_square = square;
            best_rate   = rates[i];
        }
    }
    return best_tile;
}

void launch_uniform_tile(const Tensor& q, const Tensor& k, const Tensor& v,
                         std::int32_t segment_length, std::int32_t tile_size, Tensor& out,
                         cudaStream_t stream) {
    const std::int32_t segments    = q.ne[2] / segment_length;
    const std::int32_t query_tiles = segments * ((segment_length + tile_size - 1) / tile_size);
    switch (tile_size) {
    case 16:
        launch_flash<16, 16>(q, k, v, nullptr, segment_length, query_tiles, out, stream);
        return;
    case 32:
        launch_flash<32, 32>(q, k, v, nullptr, segment_length, query_tiles, out, stream);
        return;
    case 64:
        launch_flash<64, 64>(q, k, v, nullptr, segment_length, query_tiles, out, stream);
        return;
    default:
        throw std::invalid_argument("packed_softmax_attention: invalid uniform tile size");
    }
}

} // namespace

void launch_packed_attention(const Tensor& q, const Tensor& k, const Tensor& v,
                             const Tensor& cu_seqlens, Tensor* tiles, Tensor& out,
                             cudaStream_t stream) {
    const bool packed_segments = tiles != nullptr;
    const int max_tiles        = packed_segments
                                     ? tiles->ne[1]
                                     : (q.ne[2] + kPackedDescriptorRows - 1) / kPackedDescriptorRows;
    if (packed_segments) {
        packed_attention_prepare_tiles_kernel<kPackedDescriptorRows><<<1, 256, 0, stream>>>(
            static_cast<const std::int32_t*>(cu_seqlens.data), cu_seqlens.ne[0] - 1,
            static_cast<NoncausalAttentionTile*>(tiles->data), max_tiles, q.ne[2]);
        CUDA_CHECK(cudaGetLastError());
    }

    launch_flash<kPackedDescriptorRows, kPackedDescriptorRows>(
        q, k, v,
        packed_segments ? static_cast<const NoncausalAttentionTile*>(tiles->data) : nullptr, 0,
        max_tiles, out, stream);
}

void launch_packed_attention_uniform(const Tensor& q, const Tensor& k, const Tensor& v,
                                     std::int32_t segment_length, Tensor& out,
                                     cudaStream_t stream) {
    launch_uniform_tile(q, k, v, segment_length, select_uniform_tile(segment_length), out, stream);
}

} // namespace ninfer::ops::detail
