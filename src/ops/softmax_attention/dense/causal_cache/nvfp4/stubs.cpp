#include "ops/softmax_attention/dense/causal_cache/nvfp4/launch.h"

#include <stdexcept>

namespace ninfer::ops::detail {

void nvfp4_kv_append_attention(const Tensor&, const Tensor&, const Tensor&, const Tensor&,
                               const Tensor&, const Tensor&, float, PagedKVBatchLayerView,
                               CausalAttentionExecutionEnvelope, WorkspaceArena&, Tensor&,
                               cudaStream_t) {
    throw std::runtime_error("NVFP4 KV cache requires an sm_120a build and GPU");
}

void nvfp4_kv_cached_attention(const Tensor&, const Tensor&, float, const PagedKVLayerView&,
                               CausalAttentionExecutionEnvelope, WorkspaceArena&, Tensor&,
                               cudaStream_t) {
    throw std::runtime_error("NVFP4 KV cache requires an sm_120a build and GPU");
}

} // namespace ninfer::ops::detail
