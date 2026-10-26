#pragma once

// Groupwise integer prefill follows the row-split A8 schedule in ashalliants/ninfer-3090
// (Apache-2.0), commit 4a178c4b1fd78555b5efd9b07d10d5b1946ca3ad.
// Adapted for NInfer's explicit workspace, group64 Use policy and partial-column contract.
// License: https://www.apache.org/licenses/LICENSE-2.0 (see repository LICENSE).
// Stored Q4/Q5 scales remain per group; only transient activations are quantized.
#include "ops/common/cuda/mma.cuh"
#include "ops/common/cuda/pipeline.cuh"
#include "ops/common/host/launch.h"
#include "ops/linear/kernels/group64_a8/launch.h"
#include <cuda_bf16.h>
#include <cuda_fp16.h>

namespace ninfer::ops::detail::group64_a8 {

constexpr int kThreads = 512;
constexpr int kGroup = 64;
constexpr int kPackedStride = 48;

__device__ __forceinline__ unsigned shared_u16(const void* p) {
    unsigned v;
    asm volatile("ld.shared.u16 %0, [%1];" : "=r"(v) : "r"(common::smem_addr(p)));
    return v;
}
__device__ __forceinline__ unsigned shared_u8(const void* p) {
    unsigned v;
    asm volatile("ld.shared.u8 %0, [%1];" : "=r"(v) : "r"(common::smem_addr(p)));
    return v;
}
__device__ __forceinline__ uint4 shared_u128(const void* p) {
    uint4 v;
    asm volatile("ld.shared.v4.u32 {%0,%1,%2,%3}, [%4];"
                 : "=r"(v.x), "=r"(v.y), "=r"(v.z), "=r"(v.w)
                 : "r"(common::smem_addr(p)));
    return v;
}

template <bool High>
__device__ __forceinline__ unsigned unpack(unsigned pair, unsigned high) {
    const unsigned lo = pair & 255u, hi = (pair >> 8) & 255u;
    unsigned value = (lo & 15u) | ((lo & 240u) << 4) | ((hi & 15u) << 16) | ((hi & 240u) << 20);
    if constexpr (High) {
        const unsigned bits = high & 15u;
        value |= (((bits | (bits << 7) | (bits << 14) | (bits << 21)) & 0x01010101u) << 4);
        return __vsub4(value ^ 0x10101010u, 0x10101010u);
    }
    return __vsub4(value ^ 0x08080808u, 0x08080808u);
}

// Each token/group writes an MMA-ready, contiguous lane fragment. Padding is written,
// not read from uninitialized workspace. FP32 scales avoid FP16 scale under/overflow.
template <int K, int BN>
__global__ void quantize(const __nv_bfloat16* x, int tokens, std::int8_t* codes, float* scales) {
    constexpr int Groups = K / 64;
    const int token = blockIdx.x;
    const int cb = token / BN, local = token % BN;
    for (int g = threadIdx.x; g < Groups; g += blockDim.x) {
        float peak = 0;
        if (token < tokens) {
            for (int j = 0; j < 64; ++j)
                peak = fmaxf(peak, fabsf(__bfloat162float(x[static_cast<std::size_t>(token) * K + g * 64 + j])));
        }
        const float scale = peak > 0 ? peak / 127.0F : 0.0F;
        const float inv = peak > 0 ? 127.0F / peak : 0.0F;
        scales[(static_cast<std::size_t>(cb) * Groups + g) * BN + local] = scale;
        auto* tile = codes + ((static_cast<std::size_t>(cb) * Groups + g) * (BN / 8) + local / 8) * 512;
        for (int ks = 0; ks < 2; ++ks) {
            for (int hi = 0; hi < 2; ++hi) {
                for (int tig = 0; tig < 4; ++tig) {
                    unsigned packed = 0;
                    for (int j = 0; j < 4; ++j) {
                        int code = 0;
                        if (token < tokens) {
                            const float value = __bfloat162float(x[static_cast<std::size_t>(token) * K +
                                                                  g * 64 + ks * 32 + hi * 16 + tig * 4 + j]);
                            code = max(-127, min(127, __float2int_rn(value * inv)));
                        }
                        packed |= (static_cast<unsigned>(code) & 255u) << (8 * j);
                    }
                    *reinterpret_cast<unsigned*>(tile + ((local % 8) * 4 + tig) * 16 + ks * 8 + hi * 4) = packed;
                }
            }
        }
    }
}

struct Store {
    Destination output;
    __device__ void operator()(int row, int token, float value) const {
        static_cast<__nv_bfloat16*>(output.data)[static_cast<std::size_t>(token) * output.rows + output.offset + row] =
            __float2bfloat16(value);
    }
};
struct Add {
    __nv_bfloat16* output;
    int rows;
    __device__ void operator()(int row, int token, float value) const {
        const auto index = static_cast<std::size_t>(token) * rows + row;
        output[index] = __float2bfloat16(__bfloat162float(output[index]) + value);
    }
};
struct SwiGlu {
    __nv_bfloat16* output;
    int rows;
    __device__ void operator()(int row, int token, float gate, float up) const {
        output[static_cast<std::size_t>(token) * rows + row] =
            __float2bfloat16(gate / (1.0F + __expf(-gate)) * up);
    }
};

template <bool High, int K, int NT, bool Paired>
constexpr int shared_bytes() {
    constexpr int BM = Paired ? 128 : 64, BN = 32 * NT;
    return 2 * (BM * kPackedStride + (High ? BM * 8 : 0) + BN * 64 + BN * 4) + 2 * BM * 16;
}

template <bool High, int K, int NT, bool Paired, class Epilogue>
__global__ __launch_bounds__(kThreads) void gemm(
    const std::uint8_t* w, const std::uint8_t* high, const __half* scale,
    const std::int8_t* x, const float* xs, int tokens, int row_begin, int output_rows,
    Epilogue epilogue) {
    constexpr int Groups = K / 64, MT = Paired ? 2 : 1;
    constexpr int BM = 64 * MT, BN = 32 * NT;
    constexpr int WB = BM * kPackedStride, HB = High ? BM * 8 : 0;
    constexpr int XB = BN * 64, XSB = BN * 4;
    constexpr int Stage = WB + HB + XB + XSB, Ring = BM * 16;
    extern __shared__ __align__(16) char memory[];
    const int tid = threadIdx.x, lane = tid & 31, warp = tid >> 5;
    const int gid = lane >> 2, tig = lane & 3, wm = warp >> 2, wn = warp & 3;
    const int row_block = blockIdx.x * 64, col_block = blockIdx.y;
    const auto weight_row = [&](int staged) {
        if constexpr (Paired) return row_begin + row_block + staged % 64 + (staged / 64) * output_rows;
        else return row_begin + row_block + staged;
    };
    const auto staged_row = [&](int m) {
        if constexpr (Paired) return wm * 16 + m * 64 + gid;
        else return wm * 16 + gid;
    };
    const char* xb = reinterpret_cast<const char*>(x) + static_cast<std::size_t>(col_block) * Groups * XB;
    const char* xsb = reinterpret_cast<const char*>(xs) + static_cast<std::size_t>(col_block) * Groups * XSB;
    const auto issue = [&](int group) {
        char* dst = memory + (group % 2) * Stage;
        for (int i = tid; i < BM * 2; i += kThreads) {
            const int row = weight_row(i / 2);
            common::cp_async<16, Cache::cg>(dst + (i / 2) * kPackedStride + (i % 2) * 16,
                w + static_cast<std::size_t>(row) * (K / 2) + group * 32 + (i % 2) * 16);
        }
        if constexpr (High) {
            for (int i = tid; i < BM; i += kThreads)
                common::cp_async<8>(dst + WB + i * 8,
                    high + static_cast<std::size_t>(weight_row(i)) * (K / 8) + group * 8);
        }
        for (int i = tid; i < XB / 16; i += kThreads)
            common::cp_async<16, Cache::cg>(dst + WB + HB + i * 16,
                xb + static_cast<std::size_t>(group) * XB + i * 16);
        for (int i = tid; i < XSB / 16; i += kThreads)
            common::cp_async<16, Cache::cg>(dst + WB + HB + XB + i * 16,
                xsb + static_cast<std::size_t>(group) * XSB + i * 16);
        if (group % 8 == 0) {
            for (int i = tid; i < BM; i += kThreads)
                common::cp_async<16, Cache::cg>(memory + 2 * Stage + ((group / 8) % 2) * Ring + i * 16,
                    scale + static_cast<std::size_t>(weight_row(i)) * Groups + group);
        }
        common::cp_commit();
    };
    float acc[MT][NT][4]{};
    issue(0);
    for (int g = 0; g < Groups; ++g) {
        if (g + 1 < Groups) { issue(g + 1); common::cp_wait<1>(); }
        else common::cp_wait<0>();
        __syncthreads();
        const char* sa = memory + (g % 2) * Stage;
        const char* sh = sa + WB;
        const char* sb = sh + HB;
        const auto* sx = reinterpret_cast<const float*>(sb + XB);
        const auto* ws = reinterpret_cast<const __half*>(memory + 2 * Stage + ((g / 8) % 2) * Ring);
        unsigned af[MT][2][4], bf[NT][2][2];
#pragma unroll
        for (int m = 0; m < MT; ++m) {
            const int r0 = staged_row(m), r1 = r0 + 8;
#pragma unroll
            for (int ks = 0; ks < 2; ++ks) {
                const int off = ks * 16 + tig * 2;
                unsigned h0 = 0, h1 = 0, h2 = 0, h3 = 0;
                if constexpr (High) {
                    const int byte = ks * 4 + (tig >> 1), shift = (tig & 1) * 4;
                    h0 = shared_u8(sh + r0 * 8 + byte) >> shift;
                    h1 = shared_u8(sh + r1 * 8 + byte) >> shift;
                    h2 = shared_u8(sh + r0 * 8 + byte + 2) >> shift;
                    h3 = shared_u8(sh + r1 * 8 + byte + 2) >> shift;
                }
                af[m][ks][0] = unpack<High>(shared_u16(sa + r0 * kPackedStride + off), h0);
                af[m][ks][1] = unpack<High>(shared_u16(sa + r1 * kPackedStride + off), h1);
                af[m][ks][2] = unpack<High>(shared_u16(sa + r0 * kPackedStride + off + 8), h2);
                af[m][ks][3] = unpack<High>(shared_u16(sa + r1 * kPackedStride + off + 8), h3);
            }
        }
#pragma unroll
        for (int n = 0; n < NT; ++n) {
            const uint4 b = shared_u128(sb + ((wn * NT + n) * 32 + lane) * 16);
            bf[n][0][0] = b.x; bf[n][0][1] = b.y; bf[n][1][0] = b.z; bf[n][1][1] = b.w;
        }
#pragma unroll
        for (int m = 0; m < MT; ++m) {
            const int row = staged_row(m);
            const float w0 = __half2float(ws[row * 8 + g % 8]);
            const float w1 = __half2float(ws[(row + 8) * 8 + g % 8]);
#pragma unroll
            for (int n = 0; n < NT; ++n) {
                int c0 = 0, c1 = 0, c2 = 0, c3 = 0;
#pragma unroll
                for (int ks = 0; ks < 2; ++ks)
                    common::mma_s8(c0, c1, c2, c3, af[m][ks][0], af[m][ks][1], af[m][ks][2], af[m][ks][3], bf[n][ks][0], bf[n][ks][1]);
                const int col = (wn * NT + n) * 8 + tig * 2;
                acc[m][n][0] = fmaf(static_cast<float>(c0), w0 * sx[col], acc[m][n][0]);
                acc[m][n][1] = fmaf(static_cast<float>(c1), w0 * sx[col + 1], acc[m][n][1]);
                acc[m][n][2] = fmaf(static_cast<float>(c2), w1 * sx[col], acc[m][n][2]);
                acc[m][n][3] = fmaf(static_cast<float>(c3), w1 * sx[col + 1], acc[m][n][3]);
            }
        }
        __syncthreads();
    }
#pragma unroll
    for (int n = 0; n < NT; ++n) {
        const int col = col_block * BN + (wn * NT + n) * 8 + tig * 2;
#pragma unroll
        for (int half = 0; half < 2; ++half) {
            const int row = row_block + wm * 16 + gid + half * 8;
            if constexpr (Paired) {
                if (col < tokens) epilogue(row, col, acc[0][n][half * 2], acc[1][n][half * 2]);
                if (col + 1 < tokens) epilogue(row, col + 1, acc[0][n][half * 2 + 1], acc[1][n][half * 2 + 1]);
            } else {
                if (col < tokens) epilogue(row, col, acc[0][n][half * 2]);
                if (col + 1 < tokens) epilogue(row, col + 1, acc[0][n][half * 2 + 1]);
            }
        }
    }
}

} // namespace ninfer::ops::detail::group64_a8
