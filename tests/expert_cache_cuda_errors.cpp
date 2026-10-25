#include "core/expert_cache.h"
#include "cuda_interpose.h"

#include <cuda_runtime.h>

#include <array>
#include <stdexcept>
#include <utility>

namespace {
enum class Failure { CompletionEvent, IndexCopy, ConsumerWait };

struct Trace {
    bool active = false, failed = false, pending = false, premature_free = false;
    std::size_t copies = 0;
    Failure failure    = Failure::CompletionEvent;
};

Trace trace;
} // namespace

extern "C" {

cudaError_t CUDARTAPI NINFER_TEST_CUDA_WRAP(cudaEventRecord)(cudaEvent_t event, cudaStream_t stream) {
    if (trace.active && !trace.failed && trace.failure == Failure::CompletionEvent) {
        trace.failed = true;
        return cudaErrorInvalidResourceHandle;
    }
    return NINFER_TEST_CUDA_REAL(cudaEventRecord)(event, stream);
}

cudaError_t CUDARTAPI NINFER_TEST_CUDA_WRAP(cudaMemcpyAsync)(void* to, const void* from, std::size_t bytes,
                                             cudaMemcpyKind kind, cudaStream_t stream) {
    const auto result = NINFER_TEST_CUDA_REAL(cudaMemcpyAsync)(to, from, bytes, kind, stream);
    if (trace.active && result == cudaSuccess) {
        trace.pending = true;
        ++trace.copies;
    }
    return result;
}

cudaError_t CUDARTAPI NINFER_TEST_CUDA_WRAP(cudaMemcpyBatchAsync)(void* const* to, const void* const* from,
                                                  const std::size_t* bytes, std::size_t count,
                                                  cudaMemcpyAttributes* attrs, std::size_t* indices,
                                                  std::size_t attr_count, cudaStream_t stream) {
    if (trace.active && !trace.failed && trace.failure == Failure::IndexCopy && trace.copies == 1) {
        trace.failed = true;
        return cudaErrorInvalidValue;
    }
    const auto result =
        NINFER_TEST_CUDA_REAL(cudaMemcpyBatchAsync)(to, from, bytes, count, attrs, indices, attr_count, stream);
    if (trace.active && result == cudaSuccess) {
        trace.pending = true;
        ++trace.copies;
    }
    return result;
}

cudaError_t CUDARTAPI NINFER_TEST_CUDA_WRAP(cudaStreamSynchronize)(cudaStream_t stream) {
    const auto result = NINFER_TEST_CUDA_REAL(cudaStreamSynchronize)(stream);
    if (trace.active && result == cudaSuccess) { trace.pending = false; }
    return result;
}

cudaError_t CUDARTAPI NINFER_TEST_CUDA_WRAP(cudaStreamWaitEvent)(cudaStream_t stream, cudaEvent_t event,
                                                 unsigned flags) {
    if (trace.active && !trace.failed && trace.failure == Failure::ConsumerWait) {
        trace.failed = true;
        return cudaErrorInvalidResourceHandle;
    }
    return NINFER_TEST_CUDA_REAL(cudaStreamWaitEvent)(stream, event, flags);
}

cudaError_t CUDARTAPI NINFER_TEST_CUDA_WRAP(cudaFree)(void* p) {
    if (trace.active && trace.pending) { trace.premature_free = true; }
    return NINFER_TEST_CUDA_REAL(cudaFree)(p);
}

cudaError_t CUDARTAPI NINFER_TEST_CUDA_WRAP(cudaFreeHost)(void* p) {
    if (trace.active && trace.pending) { trace.premature_free = true; }
    return NINFER_TEST_CUDA_REAL(cudaFreeHost)(p);
}
}

namespace ninfer::test {
void expert_cache_cuda_errors(const ExpertBankSource& source, const DeviceContext& device) {
    const std::array sources{source};
    auto req = describe_expert_cache(sources, {.capacity_bytes = 65536, .max_batch_bytes = 65536});
    for (const auto failure :
         {Failure::CompletionEvent, Failure::IndexCopy, Failure::ConsumerWait}) {
        bool caught = false, unusable = false;
        {
            ExpertCache cache(sources, req, device, device.transfer_view());
            cache.replace({0, req.config.capacity_bytes, 256},
                          std::array{ExpertPlacement{{{0}, 0}, 0}});
            cache.prepare({std::array{ExpertKey{{0}, 0}}});
            trace.active  = true;
            trace.failure = failure;
            try {
                cache.progress();
                if (failure == Failure::ConsumerWait) {
                    (void)cache.acquire(std::array{ExpertKey{{0}, 0}}, device.execution_view());
                }
            } catch (const std::runtime_error&) { caught = true; }
            try {
                cache.progress();
            } catch (const std::runtime_error&) { unusable = true; }
        }
        const auto result = std::exchange(trace, {});
        if (!caught || !unusable || !result.failed || !result.copies || result.pending ||
            result.premature_free) {
            throw std::runtime_error(
                "expert upload failure did not safely drain before destruction");
        }
    }
}
} // namespace ninfer::test
