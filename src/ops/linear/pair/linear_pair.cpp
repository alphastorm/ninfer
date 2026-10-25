#include "ninfer/ops/linear_pair.h"
#include "ops/linear/pair/dispatch.h"
#include "ops/common/host/operands.h"

#include <array>
#include <stdexcept>

namespace ninfer::ops {
namespace {
namespace operand = common::operand;

void require_matrix(const Tensor& t, int rows, int tokens, CudaExecutionView execution) {
    if (t.device != execution.device()) {
        throw std::invalid_argument("linear_pair: operand belongs to another device");
    }
    if (t.dtype != DType::BF16 || t.ne[0] != rows || t.ne[1] != tokens || t.ne[2] != 1 ||
        t.ne[3] != 1 || !t.is_contiguous() || !operand::aligned(t.data, 16))
        throw std::invalid_argument("linear_pair: expected aligned contiguous BF16 matrix");
}

void require_nonoverlap(const Tensor& x, const Weight& w0, const Weight& w1, const Tensor& y0,
                        const Tensor& y1) {
    const auto range = [](const void* ptr, std::size_t bytes) {
        return operand::range(ptr, bytes, "linear_pair");
    };
    const std::size_t codes  = std::size_t(1024) * x.ne[0];
    const std::size_t scales = codes / 32 * 2;
    const std::array ranges{range(x.data, x.bytes()),   range(y0.data, y0.bytes()),
                            range(y1.data, y1.bytes()), range(w0.qdata, codes),
                            range(w0.scales, scales),   range(w1.qdata, codes),
                            range(w1.scales, scales)};
    operand::disjoint(ranges, "linear_pair");
}
} // namespace

std::size_t linear_pair_workspace_capacity_bytes(const Weight& first, const Weight& second,
                                                 std::int32_t min_tokens, std::int32_t max_tokens) {
    if (min_tokens <= 0 || max_tokens < min_tokens)
        throw std::invalid_argument("linear_pair: invalid token interval");
    detail::validate_pair_weights(first, second, max_tokens);
    return 0;
}

std::size_t linear_pair_workspace_capacity_bytes(LinearProfile first, LinearProfile second,
                                                 std::int32_t min_tokens, std::int32_t max_tokens) {
    if (min_tokens <= 0 || max_tokens < min_tokens || first.qtype != QType::Q8_G32_FP16 ||
        second.qtype != QType::Q8_G32_FP16 || first.n != 1024 || second.n != 1024 ||
        first.k != second.k) {
        throw std::invalid_argument("linear_pair: invalid matrix profile or token interval");
    }
    (void)detail::select_linear_pair(first.k, max_tokens);
    return 0;
}

void linear_pair(const Tensor& x, const Weight& first, const Weight& second, Tensor& first_out,
                 Tensor& second_out, CudaExecutionView execution) {
    const auto launch = detail::select_linear_pair(x.ne[0], x.ne[1]);
    require_matrix(x, first.k, x.ne[1], execution);
    require_matrix(first_out, 1024, x.ne[1], execution);
    require_matrix(second_out, 1024, x.ne[1], execution);
    detail::validate_pair_weights(first, second, x.ne[1]);
    if (first.device != execution.device() || second.device != execution.device()) {
        throw std::invalid_argument("linear_pair: weights belong to another device");
    }
    require_nonoverlap(x, first, second, first_out, second_out);
    launch(x, first, second, first_out, second_out, execution.stream());
}
} // namespace ninfer::ops
