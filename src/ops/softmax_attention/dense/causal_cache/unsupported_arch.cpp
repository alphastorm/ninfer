#include "ops/softmax_attention/dense/causal_cache/fp8/launch.h"
#include "ops/softmax_attention/dense/causal_cache/nvfp4/launch.h"
#include "ops/softmax_attention/dense/causal_cache/k8v4/launch.h"
#include <stdexcept>

namespace ninfer::ops::detail {
void nvfp4_kv_append_attention(const Tensor&, const Tensor&, const Tensor&, const Tensor&,
                               const Tensor&, const Tensor&, float, PagedKVBatchLayerView,
                               CausalAttentionExecutionEnvelope, MemoryArena&, Tensor&,
                               CudaExecutionView) {
    throw std::runtime_error("NVFP4 KV attention requires an sm_120a GPU");
}
void nvfp4_kv_cached_attention(const Tensor&, const Tensor&, float, const PagedKVLayerView&,
                               CausalAttentionExecutionEnvelope, MemoryArena&, Tensor&,
                               CudaExecutionView) {
    throw std::runtime_error("NVFP4 KV attention requires an sm_120a GPU");
}
void k8v4_kv_append_attention(const Tensor&, const Tensor&, const Tensor&, const Tensor&,
                              const Tensor&, const Tensor&, float, PagedKVBatchLayerView,
                              CausalAttentionExecutionEnvelope, MemoryArena&, Tensor&,
                              CudaExecutionView) {
    throw std::runtime_error("K8V4 KV attention requires an sm_120a GPU");
}
void k8v4_kv_cached_attention(const Tensor&, const Tensor&, float, const PagedKVLayerView&,
                              CausalAttentionExecutionEnvelope, MemoryArena&, Tensor&,
                              CudaExecutionView) {
    throw std::runtime_error("K8V4 KV attention requires an sm_120a GPU");
}
#if defined(NINFER_SM86)
void fp8_kv_append_attention(const Tensor&, const Tensor&, const Tensor&, const Tensor&,
                             const Tensor&, const Tensor&, float, PagedKVBatchLayerView,
                             CausalAttentionExecutionEnvelope, MemoryArena&, Tensor&,
                             CudaExecutionView) {
    throw std::runtime_error("FP8 KV attention requires an sm_89 or newer GPU");
}
void fp8_kv_cached_attention(const Tensor&, const Tensor&, float, const PagedKVLayerView&,
                             CausalAttentionExecutionEnvelope, MemoryArena&, Tensor&,
                             CudaExecutionView) {
    throw std::runtime_error("FP8 KV attention requires an sm_89 or newer GPU");
}
#endif
} // namespace ninfer::ops::detail
