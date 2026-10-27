#include "ninfer/ops/rmsnorm_rope.h"
#include "ninfer/ops/rmsnorm.h"
#include "ninfer/ops/rope.h"
#include "ops/rope/rmsnorm/kernel.cuh"
#include "ops/rope/rmsnorm/offset_kernel.cuh"
#include "ops/common/host/operands.h"
#include "ops/normalization/rmsnorm/operands.h"
#include "core/device.h"

#include <cmath>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace ninfer::ops {
namespace {

template <bool Pair>
void launch_fixed(const Tensor& positions, const Tensor* q_norm_weight, const Tensor& k_norm_weight,
                  Tensor* q, Tensor& k, std::int32_t tokens, cudaStream_t stream) {
    const dim3 grid(tokens, Pair ? 5 : 1);
    detail::rmsnorm_rope_d128_kernel<Pair><<<grid, 256, 0, stream>>>(
        static_cast<const std::int32_t*>(positions.data),
        q_norm_weight == nullptr ? nullptr : static_cast<const __nv_bfloat16*>(q_norm_weight->data),
        static_cast<const __nv_bfloat16*>(k_norm_weight.data),
        q == nullptr ? nullptr : static_cast<__nv_bfloat16*>(q->data),
        static_cast<__nv_bfloat16*>(k.data));
}

constexpr std::int32_t kHeadDim       = 128;
constexpr std::int32_t kQueryHeads    = 32;
constexpr std::int32_t kKeyHeads      = 8;
constexpr std::int32_t kMaximumBatch  = 8;
constexpr std::int32_t kMaximumSingle = 2048;

void require_tensor(const Tensor& tensor, DType dtype, const std::array<std::int32_t, 4>& shape,
                    const char* label, Device device) {
    common::operand::device(tensor.device, device, "rmsnorm_rope", label);
    bool shape_matches = true;
    for (std::size_t dim = 0; dim < shape.size(); ++dim) {
        shape_matches = shape_matches && tensor.ne[dim] == shape[dim];
    }
    if (tensor.dtype != dtype || !shape_matches || !tensor.is_contiguous() ||
        !common::operand::aligned(tensor.data, 4)) {
        throw std::invalid_argument(std::string("rmsnorm_rope: invalid ") + label);
    }
}

bool overlaps(const Tensor& first, const Tensor& second) {
    return common::operand::overlaps(
        common::operand::range(first.data, first.bytes(), "rmsnorm_rope"),
        common::operand::range(second.data, second.bytes(), "rmsnorm_rope"));
}

void require_pair_nonoverlap(const Tensor& positions, const Tensor& q_norm_weight,
                             const Tensor& k_norm_weight, const Tensor& q, const Tensor& k) {
    if (overlaps(q, k) || overlaps(q, positions) || overlaps(q, q_norm_weight) ||
        overlaps(q, k_norm_weight) || overlaps(k, positions) || overlaps(k, q_norm_weight) ||
        overlaps(k, k_norm_weight)) {
        throw std::invalid_argument("rmsnorm_rope: pair mutable tensors overlap another operand");
    }
}

void require_single_nonoverlap(const Tensor& positions, const Tensor& norm_weight,
                               const Tensor& x) {
    if (overlaps(x, positions) || overlaps(x, norm_weight)) {
        throw std::invalid_argument(
            "rmsnorm_rope: single mutable tensor overlaps a read-only input");
    }
}

} // namespace

void rmsnorm_rope(const Tensor& positions, const Tensor& q_norm_weight, const Tensor& k_norm_weight,
                  Tensor& q, Tensor& k, CudaExecutionView execution) {
    const auto stream        = execution.stream();
    const std::int32_t batch = q.ne[3];
    const std::int32_t width = q.ne[2];
    if (width < 2 || width > 16) throw std::invalid_argument("rmsnorm_rope: pair W must be 2..16");
    if (batch < 1 || batch > kMaximumBatch) {
        throw std::invalid_argument("rmsnorm_rope: pair B must be 1..8");
    }
    require_tensor(q, DType::BF16, {kHeadDim, kQueryHeads, width, batch}, "q", execution.device());
    require_tensor(k, DType::BF16, {kHeadDim, kKeyHeads, width, batch}, "k", execution.device());
    require_tensor(q_norm_weight, DType::BF16, {kHeadDim, 1, 1, 1}, "q norm weight",
                   execution.device());
    require_tensor(k_norm_weight, DType::BF16, {kHeadDim, 1, 1, 1}, "k norm weight",
                   execution.device());
    require_tensor(positions, DType::I32, {width, batch, 1, 1}, "positions", execution.device());
    require_pair_nonoverlap(positions, q_norm_weight, k_norm_weight, q, k);
    launch_fixed<true>(positions, &q_norm_weight, k_norm_weight, &q, k, width * batch, stream);
    CUDA_CHECK(cudaGetLastError());
}

void rmsnorm_rope(const Tensor& positions, const Tensor& norm_weight, Tensor& x,
                  CudaExecutionView execution) {
    const auto stream         = execution.stream();
    const std::int32_t tokens = x.ne[2];
    if (tokens < 1 || tokens > kMaximumSingle) {
        throw std::invalid_argument("rmsnorm_rope: single T must be 1..2048");
    }
    require_tensor(x, DType::BF16, {kHeadDim, kKeyHeads, tokens, 1}, "x", execution.device());
    require_tensor(norm_weight, DType::BF16, {kHeadDim, 1, 1, 1}, "norm weight",
                   execution.device());
    require_tensor(positions, DType::I32, {tokens, 1, 1, 1}, "positions", execution.device());
    require_single_nonoverlap(positions, norm_weight, x);
    launch_fixed<false>(positions, nullptr, norm_weight, nullptr, x, tokens, stream);
    CUDA_CHECK(cudaGetLastError());
}

namespace {

template <detail::RopeKernelMode Mode, int QHeads, int KHeads>
void launch_offset_rope(const Tensor& positions, const Tensor& q, const Tensor& k,
                        const Tensor& q_norm_weight, const Tensor& k_norm_weight, float eps,
                        Tensor& qn, Tensor& kn, cudaStream_t stream) {
    const std::int32_t tokens = positions.ne[0];
    detail::rmsnorm_offset_rope_d256_kernel<Mode, QHeads, KHeads>
        <<<tokens, (QHeads + KHeads) * 32, 0, stream>>>(
            static_cast<const std::int32_t*>(positions.data),
            static_cast<const __nv_bfloat162*>(q.data), static_cast<const __nv_bfloat162*>(k.data),
            static_cast<const __nv_bfloat162*>(q_norm_weight.data),
            static_cast<const __nv_bfloat162*>(k_norm_weight.data),
            static_cast<__nv_bfloat162*>(qn.data), static_cast<__nv_bfloat162*>(kn.data), tokens,
            QHeads * 128, KHeads * 128, eps);
}

bool fused_offset_rope_geometry(const Tensor& positions, int rotary_dim, float theta,
                                const Tensor& q, const Tensor& k, const Tensor& q_norm_weight,
                                const Tensor& k_norm_weight, const Tensor& qn, const Tensor& kn) {
#if defined(NINFER_SM86) || defined(NINFER_SM89)
    return false;
#else
    const std::int32_t tokens = positions.ne[0];
    const bool heads = (q.ne[1] == 24 && k.ne[1] == 4) || (q.ne[1] == 16 && k.ne[1] == 2);
    const bool axes  = positions.ne[1] == 1 || positions.ne[1] == 3;
    const auto row   = [&](const Tensor& t, std::int32_t h) {
        return t.dtype == DType::BF16 && t.ne[0] == 256 && t.ne[1] == h && t.ne[2] == tokens &&
               t.ne[3] == 1 && t.is_contiguous() && common::operand::aligned(t.data, 4);
    };
    return rotary_dim == 64 && theta == 1.0e7F && heads && axes && tokens >= 1 &&
           positions.dtype == DType::I32 && positions.ne[2] == 1 && positions.ne[3] == 1 &&
           positions.is_contiguous() && common::operand::aligned(positions.data, 4) &&
           row(q, q.ne[1]) && row(qn, q.ne[1]) &&
           row(k, k.ne[1]) && row(kn, k.ne[1]) && q_norm_weight.dtype == DType::BF16 &&
           k_norm_weight.dtype == DType::BF16 && q_norm_weight.numel() == 256 &&
           k_norm_weight.numel() == 256 && q_norm_weight.is_contiguous() &&
           k_norm_weight.is_contiguous() && common::operand::aligned(q_norm_weight.data, 4) &&
           common::operand::aligned(k_norm_weight.data, 4) && !overlaps(qn, kn) &&
           !overlaps(qn, q) && !overlaps(qn, k) && !overlaps(kn, q) && !overlaps(kn, k) &&
           !overlaps(qn, positions) && !overlaps(kn, positions) && !overlaps(qn, q_norm_weight) &&
           !overlaps(qn, k_norm_weight) && !overlaps(kn, q_norm_weight) &&
           !overlaps(kn, k_norm_weight);
#endif
}

} // namespace

void rmsnorm_offset_rope(const Tensor& positions, int rotary_dim, float theta, const Tensor& q,
                         const Tensor& k, const Tensor& q_norm_weight, const Tensor& k_norm_weight,
                         float eps, Tensor& qn, Tensor& kn, CudaExecutionView execution) {
    (void)detail::rmsnorm::validate(q, q_norm_weight, nullptr, eps, qn,
                                  detail::rmsnorm::Epilogue::Offset, execution,
                                  "rmsnorm_offset_rope");
    (void)detail::rmsnorm::validate(k, k_norm_weight, nullptr, eps, kn,
                                  detail::rmsnorm::Epilogue::Offset, execution,
                                  "rmsnorm_offset_rope");
    common::operand::device(positions.device, execution.device(), "rmsnorm_offset_rope",
                            "positions");
    if (overlaps(qn, kn) || overlaps(qn, q) || overlaps(qn, k) || overlaps(kn, q) ||
        overlaps(kn, k) || overlaps(qn, positions) || overlaps(kn, positions) ||
        overlaps(qn, q_norm_weight) || overlaps(qn, k_norm_weight) ||
        overlaps(kn, q_norm_weight) || overlaps(kn, k_norm_weight)) {
        throw std::invalid_argument("rmsnorm_offset_rope: outputs overlap another operand");
    }
    if (!fused_offset_rope_geometry(positions, rotary_dim, theta, q, k, q_norm_weight,
                                    k_norm_weight, qn, kn) ||
        !(eps > 0.0f) || !std::isfinite(eps)) {
        rmsnorm(q, q_norm_weight, eps, true, qn, execution);
        rmsnorm(k, k_norm_weight, eps, true, kn, execution);
        rope(positions, rotary_dim, theta, qn, kn, execution);
        return;
    }
    const auto stream = execution.stream();
    const bool mrope  = positions.ne[1] == 3;
    if (q.ne[1] == 24) {
        if (mrope)
            launch_offset_rope<detail::RopeKernelMode::MropeD256R64, 24, 4>(
                positions, q, k, q_norm_weight, k_norm_weight, eps, qn, kn, stream);
        else
            launch_offset_rope<detail::RopeKernelMode::D256R64, 24, 4>(
                positions, q, k, q_norm_weight, k_norm_weight, eps, qn, kn, stream);
    } else {
        if (mrope)
            launch_offset_rope<detail::RopeKernelMode::MropeD256R64, 16, 2>(
                positions, q, k, q_norm_weight, k_norm_weight, eps, qn, kn, stream);
        else
            launch_offset_rope<detail::RopeKernelMode::D256R64, 16, 2>(
                positions, q, k, q_norm_weight, k_norm_weight, eps, qn, kn, stream);
    }
    CUDA_CHECK(cudaGetLastError());
}

} // namespace ninfer::ops
