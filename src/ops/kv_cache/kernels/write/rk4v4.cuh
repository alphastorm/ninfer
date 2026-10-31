#pragma once

// Adapted from ashalliants/ninfer-3090 (Apache-2.0), commit
// f5cec0a61adf23693144af4c0e8171ec95868141, src/ops/kv_cache/append/kernel.cuh.
// See THIRD_PARTY_NOTICES.md.
#include "ops/common/cuda/reduce.cuh"
#include "ops/kv_cache/kernels/layout.cuh"
#include "ops/kv_cache/kernels/hadamard.cuh"
#include "ops/kv_cache/kernels/rk4v4.cuh"
#include <cuda_bf16.h>

namespace ninfer::ops::detail::kv {

template <int Heads>
__device__ __forceinline__ void rk4_store_group(
    std::int8_t* keys, std::int8_t* values, __half* key_scales, __half* value_scales,
    int page, int head, int offset, int group, int lane,
    float k0, float k1, float v0, float v1) {
    const auto key = rk4_key_group(k0, k1);
    const __half s0 = rk4_value_scale(common::warp_max(fabsf(v0), 0xffffffffu));
    const __half s1 = rk4_value_scale(common::warp_max(fabsf(v1), 0xffffffffu));
    const int q0 = rk4_value_index(v0, s0), q1 = rk4_value_index(v1, s1);
    const int kp0 = __shfl_xor_sync(0xffffffffu, key.index0, 1);
    const int kp1 = __shfl_xor_sync(0xffffffffu, key.index1, 1);
    const int vp0 = __shfl_xor_sync(0xffffffffu, q0, 1);
    const int vp1 = __shfl_xor_sync(0xffffffffu, q1, 1);
    if ((lane & 1) == 0) {
        const auto base = page_major_offset<128, Heads>(page, head, offset, group * 32 + lane / 2);
        keys[base] = static_cast<std::int8_t>(rk4_pack(key.index0, kp0));
        keys[base + 16] = static_cast<std::int8_t>(rk4_pack(key.index1, kp1));
        values[base] = static_cast<std::int8_t>(rk4_pack(q0, vp0));
        values[base + 16] = static_cast<std::int8_t>(rk4_pack(q1, vp1));
    }
    if (lane == 0) {
        key_scales[page_major_offset<4, Heads>(page, head, offset, group)] = key.scale;
        const auto base = page_major_offset<8, Heads>(page, head, offset, group * 2);
        value_scales[base] = s0;
        value_scales[base + 1] = s1;
    }
}

template <class Geometry, class Metadata, bool MultiBatch = false, bool Page = false>
__launch_bounds__(256) __global__ void write_rk4_kernel(
    const __nv_bfloat16* k, const __nv_bfloat16* v, const std::int32_t* positions,
    Metadata metadata, std::int8_t* keys, std::int8_t* values, __half* key_scales,
    __half* value_scales, int width) {
    if constexpr (MultiBatch) {
        metadata.table_rows += blockIdx.z;
        if (metadata.valid_columns) metadata.valid_columns += blockIdx.z;
        const auto base = static_cast<std::int64_t>(blockIdx.z) * width * 256 * Geometry::KVHeads;
        k += base; v += base; positions += blockIdx.z * width;
    }
    const int lane = threadIdx.x & 31, warp = threadIdx.x >> 5;
    const int unit = blockIdx.x * 8 + warp;
    int token, head;
    if constexpr (Page) {
        const int first = (positions[0] / 8 + blockIdx.x) * 8;
        token = max(0, first - positions[0]) + warp;
        head = blockIdx.y;
        if (token >= min(metadata.valid_tokens(width), first + 8 - positions[0])) return;
    } else {
        token = unit / Geometry::KVHeads; head = unit % Geometry::KVHeads;
        if (token >= metadata.valid_tokens(width)) return;
    }
    const int position = positions[0] + token;
    int page = lane == 0 ? lookup_page(metadata.block_table(), position) : 0;
    page = __shfl_sync(0xffffffffu, page, 0);
    float key[8];
#pragma unroll
    for (int r = 0; r < 8; ++r)
        key[r] = __bfloat162float(k[source_offset<Geometry>(head, lane + 32 * r, token)]);
    normalized_hadamard_d256_inplace(key, lane);
#pragma unroll
    for (int g = 0; g < 4; ++g)
        rk4_store_group<Geometry::KVHeads>(keys, values, key_scales, value_scales,
            page, head, position & kPageMask, g, lane, key[2*g], key[2*g+1],
            __bfloat162float(v[source_offset<Geometry>(head, 64*g + lane, token)]),
            __bfloat162float(v[source_offset<Geometry>(head, 64*g + lane + 32, token)]));
}
} // namespace ninfer::ops::detail::kv
