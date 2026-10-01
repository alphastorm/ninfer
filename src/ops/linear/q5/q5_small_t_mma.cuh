#pragma once

// Q5 small-T tensor-core GEMM for the verify pass: out[N,T] = W[N,K] . x[K,T], T = 4, 8 or 16.
//
// A CTA owns 16 output rows and each of its KWarps warps one 64-value group of every
// KWarps*64-value K step, as in the Q4 small-T MMA kernel. Codes enter the MMA as exact bf16
// integers (bf16 128+m minus 144, m = lo + 16*(hi^1)), and each group's fp16 scale is applied in
// fp32 after its four k16 MMAs. The accumulation order differs from the SIMT row kernels, so a
// few outputs differ from theirs by one bf16 ulp (EXP-057).

#include "core/pdl.cuh"
#include "ops/common/memory.cuh"
#include "ops/common/mma.cuh"
#include "ops/linear/q4/q4_small_t_mma.cuh"

#include <cuda_bf16.h>
#include <cuda_fp16.h>

#include <cstdint>

namespace ninfer::ops::detail {

inline constexpr int kQ5SmallTMmaRows = 16;

// Token extents that run on the tensor cores: one request's MTP3 verify pass (T=4, EXP-057), two
// requests' MTP3 verify passes in one round at --max-concurrency 2 (T=8, EXP-077), and two
// requests' DFlash2 K=7 verify passes (T=16). An m16n8k16 MMA has eight columns, so T=4 computes
// and discards four of them, T=8 uses all eight, and T=16 issues a second MMA per k16 step for
// columns 8..15. Each output column depends only on its own activation column, so a token's
// result does not depend on the other tokens in the batch: column j of a T=16 pass is computed
// exactly as column j mod 8 of a T=8 pass over the same eight tokens.
__host__ __device__ constexpr bool q5_small_t_mma_tokens(int tokens) {
    return tokens == 4 || tokens == 8 || tokens == 16;
}

union Q5SmallTBf16PairBits {
    __nv_bfloat162 pair;
    unsigned bits;
};

// One packed code byte (low nibble = even k, high nibble = odd k) and its two high bits (bit 0 =
// even k, bit 1 = odd k) -> the exact signed codes lo - 16*hi as a bf16 pair.
__device__ __forceinline__ unsigned q5_small_t_bf16_pair(unsigned byte, unsigned high_bits) {
    unsigned m = (byte & 0x0fu) | ((byte & 0xf0u) << 12);
    m |= ((high_bits & 1u) << 4) | ((high_bits & 2u) << 19);
    m = (m ^ 0x00100010u) | 0x43004300u;
    Q5SmallTBf16PairBits value;
    value.bits = m;
    Q5SmallTBf16PairBits bias;
    bias.pair  = __floats2bfloat162_rn(144.0f, 144.0f);
    value.pair = __hsub2(value.pair, bias.pair);
    return value.bits;
}

// Rows below split go to main (leading dimension ld); the rest to tail (leading dimension n-split).
struct Q5SmallTMmaSplitStore {
    __nv_bfloat16* main;
    __nv_bfloat16* tail;
    int n;
    int split;
    int ld;

    __device__ __forceinline__ void store(int row, int col, float value) const {
        if (row < split) {
            main[static_cast<std::int64_t>(col) * ld + row] = __float2bfloat16(value);
        } else {
            tail[static_cast<std::int64_t>(col) * (n - split) + row - split] =
                __float2bfloat16(value);
        }
    }
};

// linear_add: out = bf16(sum + out), matching the SIMT split2 residual epilogue.
struct Q5SmallTMmaAddResidual {
    __nv_bfloat16* out;
    int n;

    __device__ __forceinline__ void store(int row, int col, float value) const {
        const std::int64_t index = static_cast<std::int64_t>(col) * n + row;
        out[index]               = __float2bfloat16(value + __bfloat162float(out[index]));
    }
};

template <int K, int Tokens, int KWarps, int Stages, class Epilogue, bool TriggerPdl = false>
__global__ __launch_bounds__(KWarps *
                             32) void q5_small_t_mma_kernel(const __nv_bfloat16* __restrict__ x,
                                                            const std::uint8_t* __restrict__ codes,
                                                            const std::uint8_t* __restrict__ high,
                                                            const std::uint8_t* __restrict__ scales,
                                                            Epilogue epilogue) {
    constexpr int kT          = Tokens;
    constexpr int kTiles      = kT > 8 ? kT / 8 : 1; // eight-column MMA tiles
    constexpr int kRows       = kQ5SmallTMmaRows;
    constexpr int kTileK      = 64;
    constexpr int kGroupK     = KWarps * kTileK;
    constexpr int kIters      = K / kGroupK;
    constexpr int kGroups     = K / 64;
    constexpr int kCodeStride = kGroupK / 2 + 16; // 16-byte pad: 8 fragment rows, 8 banks
    constexpr int kHighStride = kGroupK / 8 + 16;
    static_assert(K % kGroupK == 0 && (KWarps == 8 || KWarps == 16) && Stages >= 2);
    static_assert(q5_small_t_mma_tokens(Tokens));

    if constexpr (TriggerPdl) {
        if (threadIdx.x == 0) { pdl::trigger_dependents(); }
    }

    struct Stage {
        std::uint8_t codes[kRows][kCodeStride];
        std::uint8_t high[kRows][kHighStride];
        std::uint16_t scales[kRows][KWarps];
        __nv_bfloat16 act[KWarps][kT * kTileK];
    };

    union Shared {
        Stage stage[Stages];
        float partial[KWarps][32][4 * kTiles];
    };

    __shared__ __align__(16) Shared shared;

    const int tid  = static_cast<int>(threadIdx.x);
    const int warp = tid >> 5;
    const int lane = tid & 31;
    const int gid  = lane >> 2;
    const int lid  = lane & 3;
    const int row0 = static_cast<int>(blockIdx.x) * kRows;

    const auto issue = [&](int it, Stage& st) {
        const int k0 = it * kGroupK;
        {
            constexpr int kChunks = kGroupK / 32; // 16-byte code chunks per row: one per thread
            const int row         = tid / kChunks;
            const int chunk       = tid - row * kChunks;
            cp_async<16, Cache::cg>(&st.codes[row][chunk * 16],
                                    codes + static_cast<std::int64_t>(row0 + row) * (K / 2) +
                                        k0 / 2 + chunk * 16);
        }
        {
            constexpr int kChunks = kGroupK / 128;
            if (tid < kRows * kChunks) {
                const int row   = tid / kChunks;
                const int chunk = tid - row * kChunks;
                cp_async<16, Cache::cg>(&st.high[row][chunk * 16],
                                        high + static_cast<std::int64_t>(row0 + row) * (K / 8) +
                                            k0 / 8 + chunk * 16);
            }
        }
        {
            constexpr int kChunks = KWarps / 8;
            if (tid < kRows * kChunks) {
                const int row   = tid / kChunks;
                const int chunk = tid - row * kChunks;
                cp_async<16, Cache::cg>(
                    &st.scales[row][chunk * 8],
                    scales + (static_cast<std::int64_t>(row0 + row) * kGroups + k0 / 64) * 2 +
                        chunk * 16);
            }
        }
        {
            // One 16-byte chunk per lane covers four tokens' 64 values; T=8 takes two passes and
            // T=16 four.
            const int k8 = lane & 7;
#pragma unroll
            for (int pass = 0; pass < kT / 4; ++pass) {
                const int col = (lane >> 3) + 4 * pass;
                cp_async<16>(&st.act[warp][col * kTileK + q4_small_t_swizzle_64(col, k8 * 8)],
                             x + static_cast<std::int64_t>(col) * K + k0 + warp * kTileK + k8 * 8);
            }
        }
    };

    // At T=4, B rows 4..7 alias tokens 0..3: their MMA columns are computed and discarded. At
    // T=16, tile t reads B rows 8t..8t+7.
    const int b_row  = lane & (kT > 8 ? 7 : kT - 1);
    const int b_koff = ((lane >> 3) & 1) << 3;
    float acc[kTiles][4] = {};

#pragma unroll
    for (int s = 0; s < Stages - 1; ++s) {
        if (s < kIters) { issue(s, shared.stage[s]); }
        cp_commit();
    }

#pragma unroll 1
    for (int it = 0; it < kIters; ++it) {
        const int fetch = it + Stages - 1;
        if (fetch < kIters) { issue(fetch, shared.stage[fetch % Stages]); }
        cp_commit();
        cp_wait<Stages - 1>();
        __syncthreads();

        const Stage& st         = shared.stage[it % Stages];
        float group[kTiles][4] = {};
#pragma unroll
        for (int ks = 0; ks < 4; ++ks) {
            const int byte_col = warp * 32 + ks * 8 + lid;
            const unsigned h0 =
                *reinterpret_cast<const std::uint16_t*>(&st.high[gid][warp * 8 + ks * 2]);
            const unsigned h1 =
                *reinterpret_cast<const std::uint16_t*>(&st.high[gid + 8][warp * 8 + ks * 2]);
            const unsigned a0 = q5_small_t_bf16_pair(st.codes[gid][byte_col], h0 >> (2 * lid));
            const unsigned a1 = q5_small_t_bf16_pair(st.codes[gid + 8][byte_col], h1 >> (2 * lid));
            const unsigned a2 =
                q5_small_t_bf16_pair(st.codes[gid][byte_col + 4], h0 >> (8 + 2 * lid));
            const unsigned a3 =
                q5_small_t_bf16_pair(st.codes[gid + 8][byte_col + 4], h1 >> (8 + 2 * lid));
#pragma unroll
            for (int tile = 0; tile < kTiles; ++tile) {
                const int row = b_row + 8 * tile;
                unsigned b0, b1;
                ldmatrix_x2(b0, b1,
                            smem_addr(&st.act[warp][row * kTileK +
                                                    q4_small_t_swizzle_64(row, ks * 16 + b_koff)]));
                mma_bf16(group[tile][0], group[tile][1], group[tile][2], group[tile][3], a0, a1,
                         a2, a3, b0, b1);
            }
        }
        const float top_scale    = __half2float(__ushort_as_half(st.scales[gid][warp]));
        const float bottom_scale = __half2float(__ushort_as_half(st.scales[gid + 8][warp]));
#pragma unroll
        for (int tile = 0; tile < kTiles; ++tile) {
            acc[tile][0] = fmaf(group[tile][0], top_scale, acc[tile][0]);
            acc[tile][1] = fmaf(group[tile][1], top_scale, acc[tile][1]);
            acc[tile][2] = fmaf(group[tile][2], bottom_scale, acc[tile][2]);
            acc[tile][3] = fmaf(group[tile][3], bottom_scale, acc[tile][3]);
        }
        __syncthreads();
    }

#pragma unroll
    for (int tile = 0; tile < kTiles; ++tile) {
        store_vec(&shared.partial[warp][lane][4 * tile],
                  make_float4(acc[tile][0], acc[tile][1], acc[tile][2], acc[tile][3]));
    }
    __syncthreads();
    if (warp == 0 && lid < kT / 2) {
#pragma unroll
        for (int tile = 0; tile < kTiles; ++tile) {
            float4 sum = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
#pragma unroll
            for (int w = 0; w < KWarps; ++w) {
                const float4 value = load_vec<float4>(&shared.partial[w][lane][4 * tile]);
                sum.x += value.x;
                sum.y += value.y;
                sum.z += value.z;
                sum.w += value.w;
            }
            const int col0 = 8 * tile + 2 * lid;
            epilogue.store(row0 + gid, col0, sum.x);
            epilogue.store(row0 + gid, col0 + 1, sum.y);
            epilogue.store(row0 + gid + 8, col0, sum.z);
            epilogue.store(row0 + gid + 8, col0 + 1, sum.w);
        }
    }
}

// Launches the route over n rows (a multiple of 16) of a Q5 weight whose padded K is K; x holds
// Tokens columns of K values.
template <int K, int Tokens, bool TriggerPdl = false, class Epilogue>
void q5_small_t_mma_launch(const __nv_bfloat16* x, const std::uint8_t* codes,
                           const std::uint8_t* high, const std::uint8_t* scales, int n,
                           Epilogue epilogue, cudaStream_t stream) {
    // T=16 doubles each stage's activation tile, so it keeps two stages inside 48 KiB of static
    // shared memory (44,544 bytes with eight warps).
    constexpr int kWarps  = 8;
    constexpr int kStages = Tokens > 8 ? 2 : 3;
    q5_small_t_mma_kernel<K, Tokens, kWarps, kStages, Epilogue, TriggerPdl>
        <<<n / kQ5SmallTMmaRows, kWarps * 32, 0, stream>>>(x, codes, high, scales, epilogue);
}

} // namespace ninfer::ops::detail
