#pragma once

#include "core/device.h"
#include "ninfer/ops/projection_weights.h"
#include "core/tensor.h"

namespace ninfer::ops {

// Capacity for every extent in the inclusive interval; invalid profiles or intervals throw.
[[nodiscard]] std::size_t linear_pair_workspace_capacity_bytes(const Weight& first_weight,
                                                               const Weight& second_weight,
                                                               std::int32_t min_tokens,
                                                               std::int32_t max_tokens);

[[nodiscard]] std::size_t linear_pair_workspace_capacity_bytes(LinearProfile first,
                                                               LinearProfile second,
                                                               std::int32_t min_tokens,
                                                               std::int32_t max_tokens);

/**
 * first_out[:,t] = W_first * x[:,t]; second_out[:,t] = W_second * x[:,t].
 *
 * x [K,T] and both outputs [N,T] are contiguous, 16-byte-aligned BF16 matrices, T > 0.
 * Both weights use the same registered [N,K] geometry and Q8_G32_FP16 RowSplit encoding;
 * code and scale planes require 16-byte alignment.
 * The K=2048 geometry requires adjacent row views [4096,5120) and [5120,6144) of
 * the same [6144,2048] parent, including its code and scale planes.
 * Inputs, outputs and weight planes must be mutually non-overlapping.
 * Overwrites both outputs; requires no scratch and has no persistent state.
 */
void linear_pair(const Tensor& x, const Weight& first_weight, const Weight& second_weight,
                 Tensor& first_out, Tensor& second_out, CudaExecutionView execution);

} // namespace ninfer::ops
