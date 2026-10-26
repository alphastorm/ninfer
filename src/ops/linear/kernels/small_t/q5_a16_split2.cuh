#pragma once
#include "core/device.h"
#include "ops/linear/kernels/q5/q5_operands.h"
#include "ops/common/cuda/memory.cuh"
#include "ops/common/cuda/reduce.cuh"
#include "ops/common/numeric/convert.cuh"
namespace ninfer::ops::detail {
// Exact-K split-2 schedule from cad19767, re-hosted with Output/Epilogue contracts.
// Keep the two 256-value phases per warp and the original FP32 FMA/reduction order.
template <int kTt, int kFullSlabs, int kStride, class Output, class Epilogue>
__launch_bounds__(64, 16) __global__
    void q5_a16_split2_kernel(const __nv_bfloat16* __restrict__ x,
                                             const std::uint8_t* __restrict__ codes,
                                             const std::uint8_t* __restrict__ high,
                                             const std::uint8_t* __restrict__ scales,
                                             Output output, Epilogue epilogue, std::int32_t n,
                                             std::int32_t k, std::int32_t t, std::int32_t padded_k,
                                             std::int32_t full_slabs) {
    static_assert(kTt > 0, "direct split2 requires a positive column tile");
    static_assert(kFullSlabs > 0 && kStride > 0, "direct split2 requires exact positive shape");
    (void)full_slabs;
    (void)k;
    (void)t;

    __shared__ float s_part[2][kTt];

    const int lane = static_cast<int>(threadIdx.x) & 31;
    const int part = static_cast<int>(threadIdx.x) >> 5;
    const int row  = static_cast<int>(blockIdx.x);
    if (row >= n) { return; }

    const int kg_padded          = padded_k / 64;
    const std::uint8_t* code_row = codes + static_cast<std::int64_t>(row) * kg_padded * 32;
    const std::uint8_t* high_row = high + static_cast<std::int64_t>(row) * kg_padded *
                                              8;
    const std::uint8_t* scale_row = scales + static_cast<std::int64_t>(row) * kg_padded * 2;

    float acc[kTt];
#pragma unroll
    for (int i = 0; i < kTt; ++i) { acc[i] = 0.0f; }

#pragma unroll
    for (int s = 0; s < kFullSlabs; ++s) {
#pragma unroll
        for (int local = 0; local < 2; ++local) {
            const int chunk = part * 2 + local;
            const std::uint8_t* code_phase =
                code_row + static_cast<std::int64_t>(s) * 512 + chunk * 128 + lane * 4;
            const std::uint8_t* high_phase =
                high_row + static_cast<std::int64_t>(s) * 128 + chunk * 32 + lane;
            const int group_in_slab  = chunk * 4 + (lane >> 3);
            std::uint32_t scale_bits = 0;
            if ((lane & 7) == 0) {
                scale_bits = *reinterpret_cast<const std::uint16_t*>(
                    scale_row + (static_cast<std::int64_t>(s) * 16 + group_in_slab) * 2);
            }
            scale_bits = __shfl_sync(0xffffffffu, scale_bits, lane & ~7);

            const std::uint32_t word = *reinterpret_cast<const std::uint32_t*>(code_phase);
            const std::uint32_t hc   = static_cast<std::uint32_t>(*high_phase) ^ 0xffu;
            const float scale        = __half2float(__ushort_as_half(scale_bits));
            const __half2 bias       = __half2half2(__ushort_as_half(0x6410)); // 1040.0
            float w[8];
#pragma unroll
            for (int p = 0; p < 4; ++p) {
                std::uint32_t bits = ((word >> (4 * p)) & 0x000f000fu) | 0x64006400u;
                bits |= (((hc >> p) & 1u) << 4) | (((hc >> (p + 4)) & 1u) << 20);
                const __half2 h = __hsub2(common::half2_from_bits(bits), bias);
                const float2 f  = __half22float2(h);
                w[p]            = f.x * scale;
                w[p + 4]        = f.y * scale;
            }

            const std::int64_t xoff = static_cast<std::int64_t>(s) * 1024 + chunk * 256 + lane * 8;
#pragma unroll
            for (int tt = 0; tt < kTt; ++tt) {
                const uint4 xv =
                    common::load_vec<uint4>(x + static_cast<std::int64_t>(tt) * kStride + xoff);
                const float2 f0 = common::bf16x2_bits_to_float2(xv.x);
                const float2 f1 = common::bf16x2_bits_to_float2(xv.y);
                const float2 f2 = common::bf16x2_bits_to_float2(xv.z);
                const float2 f3 = common::bf16x2_bits_to_float2(xv.w);
                acc[tt]         = fmaf(w[0], f0.x, acc[tt]);
                acc[tt]         = fmaf(w[1], f0.y, acc[tt]);
                acc[tt]         = fmaf(w[2], f1.x, acc[tt]);
                acc[tt]         = fmaf(w[3], f1.y, acc[tt]);
                acc[tt]         = fmaf(w[4], f2.x, acc[tt]);
                acc[tt]         = fmaf(w[5], f2.y, acc[tt]);
                acc[tt]         = fmaf(w[6], f3.x, acc[tt]);
                acc[tt]         = fmaf(w[7], f3.y, acc[tt]);
            }
        }
    }

#pragma unroll
    for (int tt = 0; tt < kTt; ++tt) {
        float a = acc[tt];
        a       = common::warp_reduce_sum(a);
        if (lane == 0) { s_part[part][tt] = a; }
    }

    __syncthreads();

    if (part == 0 && lane < kTt) {
        float sum                = s_part[0][lane] + s_part[1][lane];
        output.store(row, lane, epilogue.apply(row, lane, sum));
    }
}


template <int K, int Tokens, class Output, class Epilogue>
void launch_q5_a16_split2(const Q5LinearOperands& operands, Output output, Epilogue epilogue,
                          cudaStream_t stream) {
    static_assert(K % 1024 == 0 && Tokens >= 1 && Tokens <= 32);
    validate_q5_operands(operands);
    if (operands.k != K || operands.padded_k != K || operands.tokens != Tokens)
        throw std::invalid_argument("Q5 split-2 requires its exact K and token extent");
    q5_a16_split2_kernel<Tokens, K / 1024, K><<<operands.rows, 64, 0, stream>>>(
        operands.x, operands.codes, operands.high, operands.scales, output, epilogue,
        operands.rows, K, Tokens, K, K / 1024);
    CUDA_CHECK(cudaGetLastError());
}
} // namespace ninfer::ops::detail
