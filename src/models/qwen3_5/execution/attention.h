#pragma once

#include "core/device.h"
#include "models/qwen3_5/execution/parameters.h"

namespace ninfer::models::qwen3_5::execution {

[[nodiscard]] std::size_t attention_projection_workspace_bytes(const AttentionProfile& parameters,
                                                               std::int32_t first,
                                                               std::int32_t last);
void attention_projection(const Tensor& hidden, const AttentionParameters& parameters,
                          Tensor& query, Tensor& gate, Tensor& key, Tensor& value,
                          MemoryArena& workspace, CudaExecutionView execution);

void text_rope(const Tensor& positions, const RopeConfig& config, Tensor& query,
               CudaExecutionView execution);
void text_rope(const Tensor& positions, const RopeConfig& config, Tensor& query, Tensor& key,
               CudaExecutionView execution);
// Unit-offset RMSNorm of query/key into normalized outputs, then text RoPE; bitwise the same as
// rmsnorm(query), rmsnorm(key), text_rope(normalized query, normalized key).
void text_norm_rope(const Tensor& positions, const RopeConfig& config, const Tensor& query,
                    const Tensor& key, const Tensor& query_norm, const Tensor& key_norm, float eps,
                    Tensor& normalized_query, Tensor& normalized_key, CudaExecutionView execution);

} // namespace ninfer::models::qwen3_5::execution
