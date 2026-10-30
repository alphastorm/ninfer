#pragma once

#include "core/tensor.h"
#include "core/weight.h"
#include <cuda_runtime_api.h>

namespace ninfer::ops::detail {

// A16 contractions with an unrounded FP32 epilogue; no transient storage.
void linear_fp32(const Tensor& x, const Weight& weight, Tensor& out, cudaStream_t stream);

} // namespace ninfer::ops::detail
