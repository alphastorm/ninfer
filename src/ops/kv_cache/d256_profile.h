#pragma once

#include "core/paged_kv_storage.h"

#include <cstdint>
#include <stdexcept>

namespace ninfer::ops {

using ninfer::kD256KVCacheHeadDim;

struct D256KVCacheProfile {
    DType code_dtype;
    std::int32_t quant_group;
    std::int32_t scale_leading_extent;
    std::int32_t code_leading_extent = 256;
    DType scale_dtype = DType::FP16;
};

inline D256KVCacheProfile d256_kv_cache_profile(
    DType dtype, KvCacheStorage storage = KvCacheStorage::BFloat16) {
    if (storage == KvCacheStorage::Nvfp4) {
        if (dtype != DType::U8) {
            throw std::invalid_argument("NVFP4 KV-cache codes must use U8 storage");
        }
        return {DType::U8, 16, 16, 128, DType::U8};
    }
    switch (dtype) {
    case DType::BF16:
        return {DType::BF16, 0, 0};
    case DType::I8:
        return {DType::I8, 64, 4};
    case DType::FP8_E4M3FN:
        return {DType::FP8_E4M3FN, 256, 1};
    default:
        throw std::invalid_argument("unsupported D256 KV-cache dtype");
    }
}

} // namespace ninfer::ops
