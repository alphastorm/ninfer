#pragma once

#include "core/arena.h"
#include "core/tensor.h"
#include "core/weight.h"
#include "ninfer/ops/linear.h"

#include <cstddef>
#include <span>

namespace ninfer::ops::detail::group64_a8 {

inline bool selected(LinearPolicy policy, int tokens) {
#if defined(NINFER_NATIVE_PREFILL_GROUP64_A8)
    return policy != LinearPolicy::A16Only && tokens > 32;
#else
    return false;
#endif
}

struct Destination {
    void* data;
    int rows;
    int offset;
    int weight_row;
    int count;
};

// Includes zero-initialized padded activation columns; no weight repacking or persistent allocation.
std::size_t workspace_bytes(int input_rows, int max_tokens);
void add(const Tensor& x, const Weight& weight, Tensor& residual, MemoryArena& workspace,
         cudaStream_t stream);
void swiglu(const Tensor& x, const Weight& weight, Tensor& out, MemoryArena& workspace,
            cudaStream_t stream);
void split(const Tensor& x, const Weight& first, const Weight& second,
           std::span<const Destination> first_outputs, std::span<const Destination> second_outputs,
           MemoryArena& workspace, cudaStream_t stream);

} // namespace ninfer::ops::detail::group64_a8
