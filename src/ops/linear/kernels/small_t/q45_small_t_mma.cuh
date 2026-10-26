#pragma once

// Tensor-core small-T Q4/Q5 projections for 1..32 activation columns: MTP verify (T = K + 1),
// DFlash verify (T = 8) and small cohorts on sm_86 and sm_89, where the SIMT split kernels pay
// one FMA stream per column and the generic sliced-K MMA stalls on its operand decode.
//
// Adapted from ashalliants/ninfer-3090 (Apache License 2.0) at 4a178c4b:
// src/ops/linear/q4/q4_ksplit_mma.cuh, src/ops/linear/q5/q5_small_t_mma.cuh and
// src/ops/common/small_t_layout.cuh. This version folds both formats into one kernel and
// publishes through this tree's Output/Epilogue contracts and row policies.
//
// Layout. A CTA is eight warps. KWarps of them split each K slab, one 64-k quantization group per
// warp, and the CTA covers 8 / KWarps * TilesPerWarp sixteen-row MMA tiles, all reading the same
// staged activation slab. Weight codes (and the Q5 high-bit plane) and FP16 group scales are staged
// with cp.async into padded rows: a half-warp of 64-bit code loads spans four rows, so code rows
// are staggered by 32 bytes and every row a warp touches sits in its own banks.
//
// K order. The MMA's k slots may be any permutation of a group's 64 k provided A and B agree.
// Lane lid owns k 16*lid .. 16*lid + 15 of its warp's group: in step ks, slots (2*lid, 2*lid+1)
// carry k 16*lid + 4*ks + {0, 1} and slots (2*lid + 8, 2*lid + 9) carry 16*lid + 4*ks + {2, 3}.
// The A operand is then eight contiguous code bytes per row (one 64-bit load, decoded to exact
// BF16 integers by a magic-bias byte permute) and the B operand sixteen contiguous activations of
// one column (two 128-bit loads, no ldmatrix).
//
// Numerics. Each warp accumulates one group's integer products in FP32 on the tensor cores, folds
// the group's FP16 scale with one FMA, and the K partials are reduced through shared memory in a
// fixed order (odd K warps publish, even ones fold their neighbour, K warp 0 sums the even ones).
// Outputs are deterministic but not bitwise equal to the SIMT routes.

#include "ops/common/cuda/memory.cuh"
#include "ops/common/cuda/mma.cuh"
#include "ops/common/cuda/pipeline.cuh"
#include "ops/linear/kernels/q4/q4_operands.h"
#include "ops/linear/kernels/q5/q5_operands.h"
#include "core/device.h"

#include <cuda_bf16.h>
#include <cuda_fp16.h>

#include <cstdint>
#include <stdexcept>

namespace ninfer::ops::detail {

enum class SmallTFormat { Q4, Q5 };

template <SmallTFormat Format, int K, int XCols, int KWarps = 8, int Stages = 1,
          int TilesPerWarp = 1, int MinBlocks = 0>
struct SmallTMmaSchedule {
    static_assert(KWarps == 2 || KWarps == 4 || KWarps == 8);
    static_assert(TilesPerWarp == 1 || TilesPerWarp == 2);
    static_assert(XCols == 4 || XCols == 8 || XCols == 16 || XCols == 24 || XCols == 32);
    static_assert(Stages >= 1 && Stages <= 3);
    static constexpr SmallTFormat kFormat = Format;
    static constexpr bool kQ5             = Format == SmallTFormat::Q5;
    static constexpr int kK               = K;
    static constexpr int kXCols           = XCols;
    static constexpr int kKWarps          = KWarps;
    static constexpr int kStages          = Stages;
    static constexpr int kTilesPerWarp    = TilesPerWarp;
    static constexpr int kWarps           = 8;
    static constexpr int kThreads         = kWarps * 32;
    static constexpr int kRowTiles        = kWarps / KWarps * TilesPerWarp;
    static constexpr int kRowsPerCta      = 16 * kRowTiles;
    static constexpr int kSlabK           = 64 * KWarps;
    static constexpr int kSlabs           = K / kSlabK;
    static_assert(K % kSlabK == 0, "K must be a whole number of slabs");
    static constexpr int kNt            = XCols < 8 ? 1 : XCols / 8;
    static constexpr int kCodeRowBytes  = KWarps * 32;
    static constexpr int kCodeRowStride = kCodeRowBytes + 32;
    static constexpr int kHighRowBytes  = KWarps * 8;
    static constexpr int kHighRowStride = kHighRowBytes + (KWarps == 2 ? 32 : 16);
    static constexpr int kXColStride    = 64 + 8;
    static constexpr int kMinBlocksPerSm =
        MinBlocks > 0 ? MinBlocks
                      : (KWarps < 8 || XCols >= 32 ? 2 : (XCols >= 16 || Stages >= 2 ? 4 : 6));

    struct Stage {
        alignas(16) std::uint8_t codes[kRowsPerCta][kCodeRowStride];
        alignas(16) std::uint8_t high[kQ5 ? kRowsPerCta : 1][kQ5 ? kHighRowStride : 16];
        alignas(16) __nv_bfloat16 activations[KWarps][XCols][kXColStride];
        alignas(16) std::uint16_t scales[kRowsPerCta][KWarps];
    };
    union Shared {
        Stage stages[Stages];
        float partial[kWarps * TilesPerWarp * kNt * 32 * 4];
    };
    static constexpr int kSharedBytes = sizeof(Shared);
    static_assert(kSharedBytes <= 48 * 1024, "small-T stages must fit static shared memory");
};

// Weight rows are the output rows.
struct SmallTIdentityRows {
    static constexpr int kOutputRowsPerTile = 16;
    __device__ __forceinline__ int weight_row(int tile, int local_row) const {
        return tile * 16 + local_row;
    }
};

// Eight packed four-bit codes -> four exact BF16 pairs. Byte j holds weight 2j in its low nibble
// and 2j + 1 in its high nibble; a nibble n is the integer (n ^ 8) - 8. Placing n ^ 8 in the
// mantissa of BF16 128.0 reads 128 + (n ^ 8); subtracting 136 leaves the code. out[i] holds
// weights (2i, 2i + 1).
__device__ __forceinline__ void small_t_decode_q4(unsigned word, unsigned (&out)[4]) {
    const unsigned kMagic = 0x43004300u; // BF16 128.0 in both halves
    const unsigned kBias  = 0x43084308u; // BF16 136.0 in both halves
    word ^= 0x88888888u;
    const unsigned lo   = word & 0x0f0f0f0fu;
    const unsigned hi   = (word >> 4) & 0x0f0f0f0fu;
    const unsigned even = __byte_perm(lo, hi, 0x5140); // weights 0..3, one per byte
    const unsigned odd  = __byte_perm(lo, hi, 0x7362); // weights 4..7
    const unsigned biased[4] = {__byte_perm(even, kMagic, 0x7150), __byte_perm(even, kMagic, 0x7372),
                                __byte_perm(odd, kMagic, 0x7150), __byte_perm(odd, kMagic, 0x7372)};
#pragma unroll
    for (int i = 0; i < 4; ++i) {
        const __nv_bfloat162 value = __hsub2(*reinterpret_cast<const __nv_bfloat162*>(&biased[i]),
                                             *reinterpret_cast<const __nv_bfloat162*>(&kBias));
        out[i]                     = *reinterpret_cast<const unsigned*>(&value);
    }
}

// Q5: the same nibbles plus one high bit per weight (bit i of `high` for weight i). A code is the
// five-bit two's-complement value n - 16 * h; with h inverted into bit 4 it is v = n + 16 * (1 - h)
// in [0, 31], which in the mantissa of BF16 128.0 reads 128 + v; subtracting 144 leaves the code.
__device__ __forceinline__ void small_t_decode_q5(unsigned word, unsigned high,
                                                  unsigned (&out)[4]) {
    const unsigned kMagic = 0x43004300u; // BF16 128.0 in both halves
    const unsigned kBias  = 0x43104310u; // BF16 144.0 in both halves
    const unsigned lo     = word & 0x0f0f0f0fu;
    const unsigned hi     = (word >> 4) & 0x0f0f0f0fu;
    unsigned even         = __byte_perm(lo, hi, 0x5140);
    unsigned odd          = __byte_perm(lo, hi, 0x7362);
    const unsigned inv    = ~high;
    // (b & 0xf) * 0x00204081 lays bits 0..3 at 0, 8, 16 and 24 without carries between copies.
    even |= (((inv & 0xfu) * 0x00204081u) & 0x01010101u) << 4;
    odd |= ((((inv >> 4) & 0xfu) * 0x00204081u) & 0x01010101u) << 4;
    const unsigned biased[4] = {__byte_perm(even, kMagic, 0x7150), __byte_perm(even, kMagic, 0x7372),
                                __byte_perm(odd, kMagic, 0x7150), __byte_perm(odd, kMagic, 0x7372)};
#pragma unroll
    for (int i = 0; i < 4; ++i) {
        const __nv_bfloat162 value = __hsub2(*reinterpret_cast<const __nv_bfloat162*>(&biased[i]),
                                             *reinterpret_cast<const __nv_bfloat162*>(&kBias));
        out[i]                     = *reinterpret_cast<const unsigned*>(&value);
    }
}

// Publishes one lane's fragment: rows (r, r + 8) of `tile` x columns (col0, col0 + 1), with
// v = {(r, col0), (r, col0 + 1), (r + 8, col0), (r + 8, col0 + 1)}. Epilogues that own a fused
// fragment (SwiGLU pairs gate row r with up row r + 8) provide store_fragment.
template <class RowPolicy, class Output, class Epilogue>
__device__ __forceinline__ void small_t_store(const RowPolicy& row_policy, const Output& output,
                                              const Epilogue& epilogue, int tile, int gid,
                                              int col0, float4 v, int rows, int columns) {
    if constexpr (requires {
                      epilogue.store_fragment(output, 0, 0, v, rows, columns);
                  }) {
        epilogue.store_fragment(output, tile * RowPolicy::kOutputRowsPerTile + gid, col0, v, rows,
                                columns);
    } else {
        const int top = row_policy.weight_row(tile, gid);
        const int bot = row_policy.weight_row(tile, gid + 8);
        if (col0 < columns) {
            output.store(top, col0, epilogue.apply(top, col0, v.x));
            output.store(bot, col0, epilogue.apply(bot, col0, v.z));
        }
        if (col0 + 1 < columns) {
            output.store(top, col0 + 1, epilogue.apply(top, col0 + 1, v.y));
            output.store(bot, col0 + 1, epilogue.apply(bot, col0 + 1, v.w));
        }
    }
}

// x: [columns, K] BF16, token-major. codes/high/scales: RowSplit Q4/Q5 planes of a weight whose
// physical K equals Schedule::kK. The grid covers output tiles; `rows` is the weight row count.
template <class Schedule, class RowPolicy, class Output, class Epilogue>
__global__ __launch_bounds__(Schedule::kThreads, Schedule::kMinBlocksPerSm) void q45_small_t_mma_kernel(
    const __nv_bfloat16* __restrict__ x, const std::uint8_t* __restrict__ codes,
    const std::uint8_t* __restrict__ high_bits, const std::uint8_t* __restrict__ scales,
    Output output, Epilogue epilogue, RowPolicy row_policy, int rows, int columns) {
    using Stage                 = typename Schedule::Stage;
    constexpr int kK            = Schedule::kK;
    constexpr int kKWarps       = Schedule::kKWarps;
    constexpr int kTpw          = Schedule::kTilesPerWarp;
    constexpr int kRowsPerCta   = Schedule::kRowsPerCta;
    constexpr int kSlabK        = Schedule::kSlabK;
    constexpr int kSlabs        = Schedule::kSlabs;
    constexpr int kStages       = Schedule::kStages;
    constexpr int kNt           = Schedule::kNt;
    constexpr int kGroupsPerRow = kK / 64;
    constexpr bool kQ5          = Schedule::kQ5;

    __shared__ __align__(16) typename Schedule::Shared shared;

    const int tid        = static_cast<int>(threadIdx.x);
    const int warp       = tid >> 5;
    const int lane       = tid & 31;
    const int gid        = lane >> 2;
    const int lid        = lane & 3;
    const int k_split    = warp % kKWarps;
    const int first_tile = warp / kKWarps * kTpw;
    const int tile0      = static_cast<int>(blockIdx.x) * Schedule::kRowTiles;

    const auto stage_x = [&](int slab_k0, Stage& stage) {
        constexpr int kChunksPerCol = kSlabK / 8;
        const int items             = columns * kChunksPerCol;
        for (int item = tid; item < items; item += Schedule::kThreads) {
            const int col   = item / kChunksPerCol;
            const int chunk = item - col * kChunksPerCol;
            common::cp_async<16>(&stage.activations[chunk / 8][col][(chunk % 8) * 8],
                                 x + static_cast<std::int64_t>(col) * kK + slab_k0 + chunk * 8);
        }
    };

    // Per row: the code chunks, then (Q5) the high-bit chunks, then one scale copy.
    const auto stage_weight = [&](int slab_k0, Stage& stage) {
        constexpr int kCodeChunks  = Schedule::kCodeRowBytes / 16;
        constexpr int kHighChunks  = kQ5 ? Schedule::kHighRowBytes / 16 : 0;
        constexpr int kItemsPerRow = kCodeChunks + kHighChunks + 1;
        for (int item = tid; item < kRowsPerCta * kItemsPerRow; item += Schedule::kThreads) {
            const int row        = item / kItemsPerRow;
            const int j          = item - row * kItemsPerRow;
            const std::int64_t r = row_policy.weight_row(tile0 + row / 16, row % 16);
            if (j < kCodeChunks) {
                common::cp_async<16, Cache::cg>(&stage.codes[row][j * 16],
                                                codes + r * (kK / 2) + slab_k0 / 2 + j * 16);
            } else if (kQ5 && j < kCodeChunks + kHighChunks) {
                const int chunk = j - kCodeChunks;
                common::cp_async<16, Cache::cg>(&stage.high[row][chunk * 16],
                                                high_bits + r * (kK / 8) + slab_k0 / 8 + chunk * 16);
            } else {
                common::cp_async<2 * kKWarps>(&stage.scales[row][0],
                                              scales + (r * kGroupsPerRow + slab_k0 / 64) * 2);
            }
        }
    };

    const auto issue_slab = [&](int slab) {
        if (slab < kSlabs) {
            Stage& stage = shared.stages[slab % kStages];
            stage_weight(slab * kSlabK, stage);
            stage_x(slab * kSlabK, stage);
        }
        common::cp_commit(); // empty commits keep cp_wait<kStages - 1> exact through the tail
    };

    const int code_byte     = k_split * 32 + 8 * lid;
    const int high_byte     = k_split * 8 + 2 * lid;
    float acc[kTpw][kNt][4] = {};

#pragma unroll
    for (int prefetch = 0; prefetch < kStages - 1; ++prefetch) issue_slab(prefetch);

#pragma unroll 1
    for (int slab = 0; slab < kSlabs; ++slab) {
        issue_slab(slab + kStages - 1);
        common::cp_wait<kStages - 1>();
        __syncthreads();

        const Stage& stage = shared.stages[slab % kStages];
        uint2 top[kTpw], bot[kTpw];
        unsigned top_high[kTpw], bot_high[kTpw];
#pragma unroll
        for (int t = 0; t < kTpw; ++t) {
            const int tile_row = (first_tile + t) * 16 + gid;
            top[t]             = common::load_vec<uint2>(&stage.codes[tile_row][code_byte]);
            bot[t]             = common::load_vec<uint2>(&stage.codes[tile_row + 8][code_byte]);
            if constexpr (kQ5) {
                top_high[t] = *reinterpret_cast<const std::uint16_t*>(&stage.high[tile_row][high_byte]);
                bot_high[t] =
                    *reinterpret_cast<const std::uint16_t*>(&stage.high[tile_row + 8][high_byte]);
            } else {
                top_high[t] = bot_high[t] = 0;
            }
        }

        float group_acc[kTpw][kNt][4] = {};
#pragma unroll
        for (int half = 0; half < 2; ++half) {
            unsigned a_top[kTpw][4], a_bot[kTpw][4];
#pragma unroll
            for (int t = 0; t < kTpw; ++t) {
                const unsigned top_word = half == 0 ? top[t].x : top[t].y;
                const unsigned bot_word = half == 0 ? bot[t].x : bot[t].y;
                if constexpr (kQ5) {
                    small_t_decode_q5(top_word, (top_high[t] >> (8 * half)) & 0xffu, a_top[t]);
                    small_t_decode_q5(bot_word, (bot_high[t] >> (8 * half)) & 0xffu, a_bot[t]);
                } else {
                    small_t_decode_q4(top_word, a_top[t]);
                    small_t_decode_q4(bot_word, a_bot[t]);
                }
            }
#pragma unroll
            for (int nt = 0; nt < kNt; ++nt) {
                const int col = nt * 8 + gid;
                uint4 b       = make_uint4(0u, 0u, 0u, 0u);
                if (col < columns)
                    b = common::load_vec<uint4>(&stage.activations[k_split][col][16 * lid + 8 * half]);
#pragma unroll
                for (int t = 0; t < kTpw; ++t) {
                    float(&g)[4] = group_acc[t][nt];
                    common::mma_bf16(g[0], g[1], g[2], g[3], a_top[t][0], a_bot[t][0],
                                     a_top[t][1], a_bot[t][1], b.x, b.y);
                    common::mma_bf16(g[0], g[1], g[2], g[3], a_top[t][2], a_bot[t][2],
                                     a_top[t][3], a_bot[t][3], b.z, b.w);
                }
            }
        }

#pragma unroll
        for (int t = 0; t < kTpw; ++t) {
            const int tile_row    = (first_tile + t) * 16 + gid;
            const float top_scale = __half2float(__ushort_as_half(stage.scales[tile_row][k_split]));
            const float bot_scale =
                __half2float(__ushort_as_half(stage.scales[tile_row + 8][k_split]));
#pragma unroll
            for (int nt = 0; nt < kNt; ++nt) {
                acc[t][nt][0] = fmaf(group_acc[t][nt][0], top_scale, acc[t][nt][0]);
                acc[t][nt][1] = fmaf(group_acc[t][nt][1], top_scale, acc[t][nt][1]);
                acc[t][nt][2] = fmaf(group_acc[t][nt][2], bot_scale, acc[t][nt][2]);
                acc[t][nt][3] = fmaf(group_acc[t][nt][3], bot_scale, acc[t][nt][3]);
            }
        }
        // The next iteration's issue_slab overwrites the stage this one just read.
        __syncthreads();
    }
    common::cp_wait<0>();
    __syncthreads();

    float* partial  = shared.partial;
    const auto slot = [&](int w, int t, int nt) {
        return partial + (((w * kTpw + t) * kNt + nt) * 32 + lane) * 4;
    };
    if ((k_split & 1) != 0) {
#pragma unroll
        for (int t = 0; t < kTpw; ++t)
#pragma unroll
            for (int nt = 0; nt < kNt; ++nt)
                common::store_vec(slot(warp, t, nt), make_float4(acc[t][nt][0], acc[t][nt][1],
                                                                 acc[t][nt][2], acc[t][nt][3]));
    }
    __syncthreads();
    if ((k_split & 1) == 0) {
#pragma unroll
        for (int t = 0; t < kTpw; ++t) {
#pragma unroll
            for (int nt = 0; nt < kNt; ++nt) {
                float(&a)[4]         = acc[t][nt];
                const float4 partner = common::load_vec<float4>(slot(warp + 1, t, nt));
                a[0] += partner.x;
                a[1] += partner.y;
                a[2] += partner.z;
                a[3] += partner.w;
                if (k_split != 0)
                    common::store_vec(slot(warp, t, nt), make_float4(a[0], a[1], a[2], a[3]));
            }
        }
    }
    if constexpr (kKWarps > 2) __syncthreads();
    if (k_split == 0) {
#pragma unroll
        for (int t = 0; t < kTpw; ++t) {
#pragma unroll
            for (int nt = 0; nt < kNt; ++nt) {
                float4 sum = make_float4(acc[t][nt][0], acc[t][nt][1], acc[t][nt][2], acc[t][nt][3]);
#pragma unroll
                for (int split = 2; split < kKWarps; split += 2) {
                    const float4 value = common::load_vec<float4>(slot(warp + split, t, nt));
                    sum.x += value.x;
                    sum.y += value.y;
                    sum.z += value.z;
                    sum.w += value.w;
                }
                small_t_store(row_policy, output, epilogue, tile0 + first_tile + t, gid,
                              nt * 8 + 2 * lid, sum, rows, columns);
            }
        }
    }
}

// Grid: output_tiles / kRowTiles CTAs, where output_tiles = output rows / kOutputRowsPerTile.
template <class Schedule, class RowPolicy, class Output, class Epilogue>
void launch_q45_small_t_mma(const __nv_bfloat16* x, const std::uint8_t* codes,
                            const std::uint8_t* high, const std::uint8_t* scales, int output_tiles,
                            int rows, int columns, Output output, Epilogue epilogue,
                            RowPolicy row_policy, cudaStream_t stream) {
    if (columns < 1 || columns > Schedule::kXCols || rows % 16 != 0 ||
        output_tiles < 1 || output_tiles % Schedule::kRowTiles != 0) {
        throw std::invalid_argument("Q4/Q5 small-T requires complete row tiles and fitting columns");
    }
    const dim3 grid(static_cast<unsigned>(output_tiles / Schedule::kRowTiles));
    q45_small_t_mma_kernel<Schedule, RowPolicy, Output, Epilogue>
        <<<grid, Schedule::kThreads, 0, stream>>>(x, codes, high, scales, output, epilogue,
                                                 row_policy, rows, columns);
    CUDA_CHECK(cudaGetLastError());
}

template <class Schedule, class Operands, class Output, class Epilogue,
          class RowPolicy = SmallTIdentityRows>
void launch_q45_small_t_mma(const Operands& operands, Output output, Epilogue epilogue,
                            cudaStream_t stream, RowPolicy row_policy = {}) {
    const std::uint8_t* high = nullptr;
    if constexpr (Schedule::kQ5) {
        validate_q5_operands(operands);
        high = operands.high;
    } else {
        validate_q4_operands(operands);
    }
    if (operands.k != Schedule::kK || operands.padded_k != Schedule::kK) {
        throw std::invalid_argument("Q4/Q5 small-T requires its exact physical K");
    }
    // Each tile consumes sixteen weight rows, including fused gate/up row policies.
    launch_q45_small_t_mma<Schedule>(
        operands.x, operands.codes, high, operands.scales, operands.rows / 16, operands.rows,
        operands.tokens, output, epilogue, row_policy, stream);
}

} // namespace ninfer::ops::detail
