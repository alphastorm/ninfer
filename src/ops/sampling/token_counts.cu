#include "ninfer/ops/sampling.h"
#include <algorithm>
#include <stdexcept>
#include "ops/common/numeric/math.h"
#include "core/device.h"

namespace ninfer::ops {
using namespace common;

namespace {

__global__ void increment_token_counts_kernel(const std::int32_t* token_ids, std::int32_t count,
                                              std::int32_t* token_counts) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < count) { atomicAdd(&token_counts[token_ids[index]], 1); }
}

void increment_token_counts_launch(const Tensor& token_ids, Tensor& token_counts,
                                   cudaStream_t stream) {
    constexpr int kBlock = 256;
    const int count      = token_ids.ne[0];
    increment_token_counts_kernel<<<div_up(count, kBlock), kBlock, 0, stream>>>(
        static_cast<const std::int32_t*>(token_ids.data), count,
        static_cast<std::int32_t*>(token_counts.data));
    CUDA_CHECK(cudaGetLastError());
}

} // namespace

void increment_token_counts(const Tensor& token_ids, Tensor& token_counts,
                            CudaExecutionView execution) {
    if (token_ids.device != execution.device() || token_counts.device != execution.device()) {
        throw std::invalid_argument(
            "increment_token_counts: operands must belong to the execution device");
    }
    const auto stream = execution.stream();
    if (token_ids.dtype != DType::I32 || token_ids.ne[0] <= 0 || token_ids.ne[1] != 1 ||
        token_ids.ne[2] != 1 || token_ids.ne[3] != 1 || !token_ids.is_contiguous() ||
        token_ids.data == nullptr) {
        throw std::invalid_argument(
            "increment_token_counts: token_ids must be a contiguous non-empty I32 vector");
    }
    if (token_counts.dtype != DType::I32 || token_counts.ne[0] <= 0 || token_counts.ne[1] != 1 ||
        token_counts.ne[2] != 1 || token_counts.ne[3] != 1 || !token_counts.is_contiguous() ||
        token_counts.data == nullptr) {
        throw std::invalid_argument(
            "increment_token_counts: token_counts must be a contiguous non-empty I32 vector");
    }
    increment_token_counts_launch(token_ids, token_counts, stream);
}

} // namespace ninfer::ops
