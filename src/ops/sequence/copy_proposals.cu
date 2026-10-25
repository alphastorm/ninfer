#include "ninfer/ops/copy_proposals.h"
#include "ops/common/host/operands.h"

#include <array>
#include <stdexcept>

namespace ninfer::ops {
namespace {

__global__ void overlay_kernel(const std::int32_t* copies, const std::int32_t* rows,
                                std::int32_t domain, std::int32_t k, std::int32_t batch,
                                std::int32_t* drafts, std::int32_t* candidates, float* q) {
    const auto index = static_cast<std::int32_t>(threadIdx.x);
    if (index >= k * batch || rows[index / k] == 0) return;
    const auto token = copies[index];
    drafts[index] = token;
    for (std::int32_t candidate = 0; candidate < 16; ++candidate) {
        // Widen before addition: token_domain is allowed up to INT32_MAX.
        candidates[index * 16 + candidate] = static_cast<std::int32_t>(
            (static_cast<std::int64_t>(token) + candidate) % domain);
        q[index * 16 + candidate] = candidate == 0 ? 1.0F : 0.0F;
    }
}

} // namespace

void overlay_copy_proposals(const Tensor& copies, const Tensor& copy_rows,
                             std::int32_t token_domain, Tensor& drafts,
                             Tensor& candidate_ids, Tensor& proposal_q,
                             CudaExecutionView execution) {
    constexpr const char* op = "overlay_copy_proposals";
    const auto k = copies.ne[0];
    const auto batch = copies.ne[1];
    if (k < 1 || k > 15 || batch < 1 || batch > 2 || token_domain < 16)
        throw std::invalid_argument("overlay_copy_proposals: invalid geometry or token domain");
    using namespace common::operand;
    tensor(copies, DType::I32, {k, batch}, 4, op, "copies", execution.device());
    tensor(copy_rows, DType::I32, {batch}, 4, op, "copy rows", execution.device());
    tensor(drafts, DType::I32, {k, batch}, 4, op, "drafts", execution.device());
    tensor(candidate_ids, DType::I32, {16, k, batch}, 4, op, "candidates", execution.device());
    tensor(proposal_q, DType::FP32, {16, k, batch}, 4, op, "q", execution.device());
    const std::array ranges{range(copies, op), range(copy_rows, op), range(drafts, op),
                            range(candidate_ids, op), range(proposal_q, op)};
    disjoint(ranges, op);
    overlay_kernel<<<1, 32, 0, execution.stream()>>>(
        static_cast<const std::int32_t*>(copies.data),
        static_cast<const std::int32_t*>(copy_rows.data), token_domain, k, batch,
        static_cast<std::int32_t*>(drafts.data), static_cast<std::int32_t*>(candidate_ids.data),
        static_cast<float*>(proposal_q.data));
    CUDA_CHECK(cudaGetLastError());
}

} // namespace ninfer::ops
