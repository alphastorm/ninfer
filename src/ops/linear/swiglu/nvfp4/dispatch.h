#pragma once

#include "core/weight.h"
#include "core/arena.h"
#include "core/tensor.h"
#include "ninfer/ops/linear.h"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace ninfer::ops::detail {

[[nodiscard]] std::size_t nvfp4_linear_swiglu_workspace_capacity_bytes(LinearPolicy policy,
                                                                       std::int32_t min_tokens,
                                                                       std::int32_t max_tokens);

void launch_nvfp4_swiglu_decode(const Tensor& x, const Weight& weight, Tensor& out,
                                cudaStream_t stream);
void launch_nvfp4_swiglu_small_t(const Tensor& x, const Weight& weight, Tensor& out,
                                 cudaStream_t stream);
void launch_nvfp4_swiglu_a4(const Tensor& x, const Weight& weight, Tensor& out,
                            MemoryArena& workspace, cudaStream_t stream);

void nvfp4_linear_swiglu_dispatch(const Tensor& x, const Weight& weight, Tensor& out,
                                  LinearPolicy policy, MemoryArena& workspace, cudaStream_t stream);

} // namespace ninfer::ops::detail
