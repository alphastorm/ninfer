#pragma once

#include "ops/moe/kernels/cuda/routing/launch.h"
#include "ops/common/cuda/pipeline.cuh"
#include "ops/common/cuda/mma.cuh"

namespace ninfer::ops::detail::moe_cuda::routing {
using common::cp_async_zfill;
using common::cp_commit;
using common::cp_wait;
using common::smem_addr;
using common::mma_bf16;
using common::ldmatrix_x2;
using common::ldmatrix_x4;

template <class Shape>
__global__ __launch_bounds__(256,
                             2) void router_mma_kernel(const __nv_bfloat16* __restrict__ x,
                                                       const __nv_bfloat16* __restrict__ weight,
                                                       float* __restrict__ scores, int tokens) {
    constexpr int BM = 16, BN = 64, BK = 64, Stages = 2, Threads = 256;
    __shared__ __align__(16) __nv_bfloat16 Ws[Stages][BM * BK];
    __shared__ __align__(16) __nv_bfloat16 Xs[Stages][BN * BK];

    const int tid    = static_cast<int>(threadIdx.x);
    const int warp   = tid >> 5;
    const int lane   = tid & 31;
    const int row0   = static_cast<int>(blockIdx.x) * BM;
    const int token0 = static_cast<int>(blockIdx.y) * BN;

    float acc[4] = {};

    auto stage_inputs = [&](int stage, int kt) {
        const int k0 = kt * BK;
        for (int item = tid; item < BM * (BK / 8); item += Threads) {
            const int row = item / (BK / 8);
            const int k8  = item - row * (BK / 8);
            const int rr  = row0 + row;
            auto* dst     = &Ws[stage][row * BK + common::swizzle_xor_64(row, k8 * 8)];
            const auto* src =
                weight +
                static_cast<std::int64_t>(rr < Shape::control_rows ? rr : 0) * Shape::hidden + k0 +
                k8 * 8;
            cp_async_zfill<16, Cache::cg>(dst, src, rr < Shape::control_rows ? 16 : 0);
        }
        for (int item = tid; item < BN * (BK / 8); item += Threads) {
            const int col = item / (BK / 8);
            const int k8  = item - col * (BK / 8);
            const int tt  = token0 + col;
            auto* dst     = &Xs[stage][col * BK + common::swizzle_xor_64(col, k8 * 8)];
            const auto* src =
                x + static_cast<std::int64_t>(tt < tokens ? tt : 0) * Shape::hidden + k0 + k8 * 8;
            cp_async_zfill<16, Cache::cg>(dst, src, tt < tokens ? 16 : 0);
        }
    };

#pragma unroll
    for (int stage = 0; stage < Stages; ++stage) {
        stage_inputs(stage, stage);
        cp_commit();
    }

    const int a_mat    = lane >> 3;
    const int a_rin    = lane & 7;
    const int a_rowoff = a_rin + ((a_mat & 1) << 3);
    const int a_coloff = (a_mat >> 1) << 3;
    const int b_rin    = lane & 7;
    const int b_koff   = ((lane >> 3) & 1) << 3;

#pragma unroll 1
    for (int kt = 0; kt < Shape::hidden / BK; ++kt) {
        const int stage = kt & 1;
        cp_wait<Stages - 1>();
        __syncthreads();

#pragma unroll
        for (int ki = 0; ki < BK / 16; ++ki) {
            unsigned af[4];
            unsigned bf[2];
            const int arow = a_rowoff;
            const int acol = ki * 16 + a_coloff;
            const int brow = warp * 8 + b_rin;
            const int bcol = ki * 16 + b_koff;
            ldmatrix_x4(af[0], af[1], af[2], af[3],
                        smem_addr(&Ws[stage][arow * BK + common::swizzle_xor_64(arow, acol)]));
            ldmatrix_x2(bf[0], bf[1],
                        smem_addr(&Xs[stage][brow * BK + common::swizzle_xor_64(brow, bcol)]));
            mma_bf16(acc[0], acc[1], acc[2], acc[3], af[0], af[1], af[2], af[3], bf[0], bf[1]);
        }

        __syncthreads();
        const int next = kt + Stages;
        if (next < Shape::hidden / BK) { stage_inputs(stage, next); }
        cp_commit();
    }
    cp_wait<0>();

    const int gid = lane >> 2;
    const int lid = lane & 3;
    const int r0  = row0 + gid;
    const int r1  = r0 + 8;
    const int c0  = token0 + warp * 8 + 2 * lid;
    const int c1  = c0 + 1;
    if (r0 < Shape::control_rows && c0 < tokens) {
        scores[static_cast<std::int64_t>(c0) * Shape::score_rows + r0] = acc[0];
    }
    if (r0 < Shape::control_rows && c1 < tokens) {
        scores[static_cast<std::int64_t>(c1) * Shape::score_rows + r0] = acc[1];
    }
    if (r1 < Shape::control_rows && c0 < tokens) {
        scores[static_cast<std::int64_t>(c0) * Shape::score_rows + r1] = acc[2];
    }
    if (r1 < Shape::control_rows && c1 < tokens) {
        scores[static_cast<std::int64_t>(c1) * Shape::score_rows + r1] = acc[3];
    }
}

} // namespace ninfer::ops::detail::moe_cuda::routing
