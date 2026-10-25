#pragma once

#include "ninfer/ops/softmax_attention.h"
#include "ops/common/host/operands.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace ninfer::ops::detail::noncausal {

inline constexpr std::int32_t kHeadDim = 72;
inline constexpr std::int32_t kHeads   = 16;
inline constexpr float kExpectedScale  = 0.11785113019775792073f;

inline void require_profile(AttentionHeadGeometry geometry, float scale, const char* op) {
    if (!valid_attention_head_geometry(geometry) || geometry.head_dim != kHeadDim ||
        geometry.query_heads != kHeads || geometry.kv_heads != kHeads) {
        throw std::invalid_argument(std::string(op) + ": unsupported head geometry");
    }
    if (!std::isfinite(scale) || std::abs(scale - kExpectedScale) > 1.0e-7f) {
        throw std::invalid_argument(std::string(op) + ": scale must be 1/sqrt(72)");
    }
}

inline void require_qkv(const Tensor& tensor, std::int32_t tokens, const char* op, const char* name,
                        Device device) {
    common::operand::device(tensor.device, device, op, name);
    if (tensor.dtype != DType::BF16 || tensor.ne[0] != kHeadDim || tensor.ne[1] != kHeads ||
        tensor.ne[2] != tokens || tensor.ne[3] != 1) {
        throw std::invalid_argument(std::string(op) + ": invalid " + name + " shape");
    }
    constexpr std::int64_t elem = 2;
    if (tensor.nb[0] != elem || tensor.nb[1] != elem * kHeadDim ||
        tensor.nb[2] < elem * kHeadDim * kHeads || (tensor.nb[2] % elem) != 0) {
        throw std::invalid_argument(std::string(op) + ": invalid " + name + " strides");
    }
    if (tensor.data == nullptr) {
        throw std::invalid_argument(std::string(op) + ": " + name + " data must be non-null");
    }
}

inline std::int32_t validate_qkv(const Tensor& q, const Tensor& k, const Tensor& v,
                                 const Tensor& out, AttentionHeadGeometry geometry, float scale,
                                 const char* op, Device device) {
    require_profile(geometry, scale, op);
    const std::int32_t tokens = q.ne[2];
    if (tokens <= 0) { throw std::invalid_argument(std::string(op) + ": T must be positive"); }
    require_qkv(q, tokens, op, "q", device);
    require_qkv(k, tokens, op, "k", device);
    require_qkv(v, tokens, op, "v", device);
    require_qkv(out, tokens, op, "out", device);
    if (!out.is_contiguous()) {
        throw std::invalid_argument(std::string(op) + ": out must be contiguous");
    }
    return tokens;
}

} // namespace ninfer::ops::detail::noncausal
