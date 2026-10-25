#pragma once

#include "core/device.h"
#include "ops/linear_attention/kimi_delta_attention/operands.h"

namespace ninfer::ops::detail::kimi_delta_attention {
struct Chunk;
void launch_recurrent(const Arguments& args, cudaStream_t stream);
void launch_batch_update(const Arguments& args, const std::int32_t* slots, int batch,
                         std::int64_t slot_stride, cudaStream_t stream);
void launch_prepare(const Arguments& args, Chunk* workspace, cudaStream_t stream);
void launch_chunk_recurrence(const Arguments& args, const Chunk* workspace, int value_tile,
                             cudaStream_t stream);

} // namespace ninfer::ops::detail::kimi_delta_attention
