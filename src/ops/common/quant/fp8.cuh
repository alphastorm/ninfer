#pragma once

#include "ops/common/cuda/memory.cuh"

#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_fp8.h>

#include <cstdint>

namespace ninfer::ops::common {

inline constexpr float kFp8E4M3MaxFinite = 448.0F;

// Dynamic E4M3 activation groups retain their scale in FP32.
__device__ __forceinline__ float fp8_fp32_scale(float absmax) {
    return __fdiv_rn(absmax, kFp8E4M3MaxFinite);
}

__device__ __forceinline__ std::uint16_t fp8_quant_fp32_pair(float2 value, float scale) {
    if (scale == 0.0F) { return 0; }
    return __nv_cvt_float2_to_fp8x2(
        make_float2(__fdiv_rn(value.x, scale), __fdiv_rn(value.y, scale)), __NV_SATFINITE,
        __NV_E4M3);
}

__device__ __forceinline__ unsigned fp8_e4m3x2_to_bf16x2_bits(unsigned packed) {
#if __CUDACC_VER_MAJOR__ > 13 || (__CUDACC_VER_MAJOR__ == 13 && __CUDACC_VER_MINOR__ >= 2)
    // PTX 9.2 exposes the native pair widening on the project's sm_120a target.
    unsigned result;
    asm("cvt.rn.bf16x2.e4m3x2 %0, %1;" : "=r"(result) : "h"(static_cast<std::uint16_t>(packed)));
    return result;
#else
    // CUDA 13.1 retains the exact FP16 bridge.
    __nv_fp8x2_e4m3 fp8;
    fp8.__x                 = static_cast<std::uint16_t>(packed);
    const __half2 half_pair = static_cast<__half2>(fp8);

    union {
        __nv_bfloat162 pair;
        unsigned bits;
    } result;

    result.pair = __halves2bfloat162(__nv_bfloat16(__low2half(half_pair)),
                                     __nv_bfloat16(__high2half(half_pair)));
    return result.bits;
#endif
}

__device__ __forceinline__ float decode_e4m3(std::uint8_t storage) {
    __nv_fp8x2_e4m3 value;
    value.__x = static_cast<std::uint16_t>(storage) | (static_cast<std::uint16_t>(storage) << 8);
    return static_cast<float2>(value).x;
}

__device__ __forceinline__ float2 fp8_code2_to_float2(std::uint16_t storage) {
    __nv_fp8x2_e4m3 value;
    value.__x = storage;
    return static_cast<float2>(value);
}

// Codes use the reciprocal of the represented FP16 scale; nonzero scales clamp to
// [2^-24, 65504] before FP16-RNE. A zero absmax produces a zero scale and codes.
struct Fp8Fp16Scale {
    __half scale;
    float inverse_scale;
};

__device__ __forceinline__ Fp8Fp16Scale fp8_fp16_scale(float absmax) {
    if (absmax == 0.0F) { return {.scale = __float2half_rn(0.0F), .inverse_scale = 0.0F}; }
    const float raw_scale         = absmax / kFp8E4M3MaxFinite;
    const float bounded           = fminf(65504.0F, fmaxf(0x1p-24F, raw_scale));
    const __half scale            = __float2half_rn(bounded);
    const float represented_scale = __half2float(scale);
    return {.scale = scale, .inverse_scale = 1.0F / represented_scale};
}

__device__ __forceinline__ std::uint8_t fp8_quant_code(float x, float inverse_scale) {
    if (inverse_scale == 0.0F) { return 0; }
    return __nv_cvt_float_to_fp8(x * inverse_scale, __NV_SATFINITE, __NV_E4M3);
}

__device__ __forceinline__ std::uint16_t fp8_quant_code2(float x0, float x1, float inverse_scale) {
    if (inverse_scale == 0.0F) { return 0; }
    return __nv_cvt_float2_to_fp8x2(make_float2(x0 * inverse_scale, x1 * inverse_scale),
                                    __NV_SATFINITE, __NV_E4M3);
}

__device__ __forceinline__ __half2 fp8_code2_to_half2(std::uint16_t storage) {
    __nv_fp8x2_e4m3 value;
    value.__x = storage;
    return static_cast<__half2>(value);
}

__device__ __forceinline__ __half2 fp8_dequant_code2_to_half2(std::uint16_t storage, __half scale) {
    return __hmul2(fp8_code2_to_half2(storage), __halves2half2(scale, scale));
}

// Eight code bytes share one represented FP16 scale; expansion multiplies in FP16.
__device__ __forceinline__ int4 fp8_dequant_f16x8(const std::uint8_t* codes, __half scale) {
    const int2 raw         = load_vec<int2>(codes);
    const std::uint16_t* c = reinterpret_cast<const std::uint16_t*>(&raw);
    unsigned packed[4];
#pragma unroll
    for (int i = 0; i < 4; ++i) {
        const __half2 value2 = fp8_dequant_code2_to_half2(c[i], scale);
        packed[i]            = *reinterpret_cast<const unsigned*>(&value2);
    }
    return make_int4(static_cast<int>(packed[0]), static_cast<int>(packed[1]),
                     static_cast<int>(packed[2]), static_cast<int>(packed[3]));
}

} // namespace ninfer::ops::common
