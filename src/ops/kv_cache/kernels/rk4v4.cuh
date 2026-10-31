#pragma once

// Adapted from ashalliants/ninfer-3090 (Apache-2.0), commit
// f5cec0a61adf23693144af4c0e8171ec95868141, src/ops/kv_cache/int8_g64_codec.cuh.
// See THIRD_PARTY_NOTICES.md. The format is not the E8 variant.
#include <cuda_fp16.h>
#include <cmath>
#include <cstdint>

namespace ninfer::ops::detail::kv {

__host__ __device__ constexpr int rk4_key_code(int index) {
    constexpr int codes[] = {6, 18, 31, 44, 58, 75, 96, 127};
    return (index & 8) ? -codes[index & 7] : codes[index & 7];
}

__host__ __device__ inline int rk4_key_index(float x, float inverse_rms) {
    const float a = fabsf(x * inverse_rms);
    const int magnitude = (a >= 0.25825f) + (a >= 0.52245f) + (a >= 0.79960f) +
                          (a >= 1.09930f) + (a >= 1.43715f) + (a >= 1.84355f) +
                          (a >= 2.40080f);
    return (x < 0.0f ? 8 : 0) | magnitude;
}

__host__ __device__ constexpr std::uint8_t rk4_pack(int low, int high) {
    return static_cast<std::uint8_t>((low & 15) | ((high & 15) << 4));
}

__host__ __device__ constexpr int rk4_value_code(unsigned nibble) {
    return static_cast<int>((nibble & 15) ^ 8u) - 8;
}

__host__ __device__ inline __half rk4_value_scale(float absmax) {
    return __float2half_rn(absmax / 7.0f);
}

__host__ __device__ inline int rk4_value_index(float value, __half scale) {
    const float s = __half2float(scale);
    if (s == 0.0f) return 0;
#if defined(__CUDA_ARCH__)
    const int q = __float2int_rn(value * (1.0f / s));
#else
    const int q = static_cast<int>(std::nearbyint(value * (1.0f / s)));
#endif
    return q < -7 ? -7 : (q > 7 ? 7 : q);
}

struct Rk4KeyGroup {
    int index0, index1;
    __half scale;
};

__host__ __device__ inline __half rk4_key_scale(float xc, float cc) {
#if defined(__CUDA_ARCH__)
    return __float2half_rn(__fdiv_rn(xc, cc));
#else
    return __float2half_rn(xc / cc);
#endif
}

#if defined(__CUDACC__)
// A full warp holds dimensions lane and lane+32. Explicit RN operations preserve
// the source codec's butterfly reduction and prevent contraction into FMAs.
__device__ __forceinline__ Rk4KeyGroup rk4_key_group(float x0, float x1) {
    float ss = __fadd_rn(__fmul_rn(x0, x0), __fmul_rn(x1, x1));
#pragma unroll
    for (int offset = 16; offset; offset >>= 1)
        ss = __fadd_rn(ss, __shfl_xor_sync(0xffffffffu, ss, offset));
    const float rms = __fsqrt_rn(__fmul_rn(ss, 1.0f / 64.0f));
    const float inv = rms > 0.0f ? __fdiv_rn(1.0f, rms) : 0.0f;
    const int i0 = rk4_key_index(x0, inv), i1 = rk4_key_index(x1, inv);
    const float c0 = static_cast<float>(rk4_key_code(i0));
    const float c1 = static_cast<float>(rk4_key_code(i1));
    float xc = __fadd_rn(__fmul_rn(x0, c0), __fmul_rn(x1, c1));
    float cc = __fadd_rn(__fmul_rn(c0, c0), __fmul_rn(c1, c1));
#pragma unroll
    for (int offset = 16; offset; offset >>= 1) {
        xc = __fadd_rn(xc, __shfl_xor_sync(0xffffffffu, xc, offset));
        cc = __fadd_rn(cc, __shfl_xor_sync(0xffffffffu, cc, offset));
    }
    return {i0, i1, rk4_key_scale(xc, cc)};
}

__device__ __forceinline__ unsigned rk4_expand_keys(unsigned nibbles) {
    const unsigned magnitude = nibbles & 0x7777u;
    const unsigned pos = __byte_perm(0x2C1F1206u, 0x7F604B3Au, magnitude);
    const unsigned neg = __byte_perm(0xD4E1EEFAu, 0x81A0B5C6u, magnitude);
    const unsigned mask = __byte_perm(0u, 0xFF00u, ((nibbles >> 3) & 0x1111u) | 0x4444u);
    return (neg & mask) | (pos & ~mask);
}

template <bool Key>
__device__ __forceinline__ int4 rk4_expand16(const std::int8_t* packed) {
    const uint2 raw = *reinterpret_cast<const uint2*>(packed);
    const unsigned words[] = {raw.x, raw.x >> 16, raw.y, raw.y >> 16};
    int4 result;
    auto* out = reinterpret_cast<unsigned*>(&result);
#pragma unroll
    for (int i = 0; i < 4; ++i) {
        if constexpr (Key) out[i] = rk4_expand_keys(words[i]);
        else {
            unsigned codes = 0;
#pragma unroll
            for (int j = 0; j < 4; ++j)
                codes |= (static_cast<unsigned>(rk4_value_code(words[i] >> (4 * j))) & 255u) << (8 * j);
            out[i] = codes;
        }
    }
    return result;
}
#endif
} // namespace ninfer::ops::detail::kv
