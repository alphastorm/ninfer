#pragma once

#include "ops/common/cuda/reduce.cuh"
#include "ops/normalization/rmsnorm/kernel/epilogue.cuh"
#include "ops/rope/kernels/rotate.cuh"

#include <cuda_bf16.h>
#include <cstdint>

namespace ninfer::ops::detail {

// One CTA owns one token: warp w normalizes head w of q (w < QHeads) or head w-QHeads of k, keeping
// the row in registers exactly as rmsnorm_warp_bf16x2_kernel<Offset, ..., 256> does, rounds the
// normalized row to BF16, and rotates its first 64 elements exactly as rope_fixed_kernel<Mode>
// does with the token's shared sine/cosine table. Lane L holds BF16x2 pairs L, L+32, L+64, L+96;
// the rotary half-pairs L and L+16 meet through one xor-16 shuffle.
template <RopeKernelMode Mode, int QHeads, int KHeads>
__global__ __launch_bounds__((QHeads + KHeads) * 32) void rmsnorm_offset_rope_d256_kernel(
    const std::int32_t* __restrict__ positions, const __nv_bfloat162* __restrict__ q,
    const __nv_bfloat162* __restrict__ k, const __nv_bfloat162* __restrict__ q_weight,
    const __nv_bfloat162* __restrict__ k_weight, __nv_bfloat162* __restrict__ qn,
    __nv_bfloat162* __restrict__ kn, std::int32_t tokens, std::int64_t q_token_pairs,
    std::int64_t k_token_pairs, float eps) {
    static_assert(Mode == RopeKernelMode::D256R64 || Mode == RopeKernelMode::MropeD256R64);
    constexpr int kD             = 256;
    constexpr int kPairsPerLane  = 4;
    constexpr int kHalf          = 32;
    constexpr int kHalfPair      = kHalf / 2;
    const int token              = static_cast<int>(blockIdx.x);
    const int lane               = static_cast<int>(threadIdx.x) & (common::kWarpSize - 1);
    const int warp               = static_cast<int>(threadIdx.x) / common::kWarpSize;

    __shared__ float cos_cache[kHalf];
    __shared__ float sin_cache[kHalf];
    if (threadIdx.x < kHalf) {
        const int pair = static_cast<int>(threadIdx.x);
        fixed_sincos<Mode>(positions, tokens, token, pair, &sin_cache[pair], &cos_cache[pair]);
    }

    const bool query            = warp < QHeads;
    const int head              = query ? warp : warp - QHeads;
    const __nv_bfloat162* x     = query ? q : k;
    const __nv_bfloat162* weight = query ? q_weight : k_weight;
    __nv_bfloat162* out         = query ? qn : kn;
    const std::int64_t row_base = static_cast<std::int64_t>(token) * (query ? q_token_pairs : k_token_pairs) +
                                  static_cast<std::int64_t>(head) * (kD / 2);

    __nv_bfloat162 values[kPairsPerLane];
    __nv_bfloat162 weights[kPairsPerLane];
    float sum = 0.0f;
#pragma unroll
    for (int i = 0; i < kPairsPerLane; ++i) {
        const int pair  = lane + i * common::kWarpSize;
        values[i]       = x[row_base + pair];
        weights[i]      = weight[pair];
        const float2 xf = __bfloat1622float2(values[i]);
        sum += xf.x * xf.x + xf.y * xf.y;
    }
    sum       = common::warp_reduce_sum(sum);
    float inv = lane == 0 ? rsqrtf(sum / static_cast<float>(kD) + eps) : 0.0f;
    inv       = __shfl_sync(common::kFullWarpMask, inv, 0);

    __nv_bfloat162 normalized[kPairsPerLane];
#pragma unroll
    for (int i = 0; i < kPairsPerLane; ++i) {
        const float2 xf = __bfloat1622float2(values[i]);
        const float2 wf = __bfloat1622float2(weights[i]);
        normalized[i]   = __floats2bfloat162_rn(
            rmsnorm::apply_epilogue<rmsnorm::Epilogue::Offset>(xf.x, inv, wf.x, 0.0f),
            rmsnorm::apply_epilogue<rmsnorm::Epilogue::Offset>(xf.y, inv, wf.y, 0.0f));
    }
#pragma unroll
    for (int i = 1; i < kPairsPerLane; ++i) out[row_base + lane + i * common::kWarpSize] = normalized[i];

    const __nv_bfloat162 partner =
        __shfl_xor_sync(common::kFullWarpMask, normalized[0], kHalfPair);
    __syncthreads();

    const int rotary_pair = lane & (kHalfPair - 1);
    const float c0        = cos_cache[rotary_pair * 2];
    const float c1        = cos_cache[rotary_pair * 2 + 1];
    const float s0        = sin_cache[rotary_pair * 2];
    const float s1        = sin_cache[rotary_pair * 2 + 1];
    if (lane < kHalfPair) {
        const float2 first  = __bfloat1622float2(normalized[0]);
        const float2 second = __bfloat1622float2(partner);
        out[row_base + lane] =
            __floats2bfloat162_rn(first.x * c0 - second.x * s0, first.y * c1 - second.y * s1);
    } else {
        const float2 first  = __bfloat1622float2(partner);
        const float2 second = __bfloat1622float2(normalized[0]);
        out[row_base + lane] =
            __floats2bfloat162_rn(second.x * c0 + first.x * s0, second.y * c1 + first.y * s1);
    }
}

} // namespace ninfer::ops::detail
