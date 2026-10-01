#include "ops/kv_cache/append/launch.h"

#include <stdexcept>

namespace ninfer::ops::detail {

void kv_cache_append_nvfp4_launch(const Tensor&, const Tensor&, const Tensor&,
                                  PagedKVLayerView, cudaStream_t) {
    throw std::runtime_error("NVFP4 KV cache requires an sm_120a build and GPU");
}

void kv_cache_append_nvfp4_batch_launch(const Tensor&, const Tensor&, const Tensor&,
                                        const Tensor&, const Tensor&,
                                        PagedKVBatchLayerView, cudaStream_t) {
    throw std::runtime_error("NVFP4 KV cache requires an sm_120a build and GPU");
}

} // namespace ninfer::ops::detail
