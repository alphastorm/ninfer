#pragma once

#include <cuda_runtime.h>
#include <cstddef>

// Test executables only: real CUDA work is retained beneath their fault/call tracing wrappers.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdexcept>
#include <string>

namespace ninfer::test::cuda_interpose {
template <class Function>
Function runtime_symbol(const char* name) {
    const auto module = GetModuleHandleW(L"cudart64_13.dll");
    const auto symbol = module ? GetProcAddress(module, name) : nullptr;
    if (!symbol) throw std::runtime_error(std::string("missing CUDA runtime symbol: ") + name);
    return reinterpret_cast<Function>(symbol);
}
#define NINFER_CUDA_REAL_FUNCTION(name, signature, arguments) \
    inline cudaError_t CUDARTAPI real_##name signature { \
        static const auto call = runtime_symbol<decltype(&real_##name)>(#name); \
        return call arguments; \
    }
NINFER_CUDA_REAL_FUNCTION(cudaMalloc, (void** p, std::size_t bytes), (p, bytes))
NINFER_CUDA_REAL_FUNCTION(cudaMallocHost, (void** p, std::size_t bytes), (p, bytes))
NINFER_CUDA_REAL_FUNCTION(cudaFree, (void* p), (p))
NINFER_CUDA_REAL_FUNCTION(cudaFreeHost, (void* p), (p))
NINFER_CUDA_REAL_FUNCTION(cudaEventCreateWithFlags, (cudaEvent_t* event, unsigned flags), (event, flags))
NINFER_CUDA_REAL_FUNCTION(cudaEventRecord, (cudaEvent_t event, cudaStream_t stream), (event, stream))
NINFER_CUDA_REAL_FUNCTION(cudaMemcpyAsync,
    (void* to, const void* from, std::size_t bytes, cudaMemcpyKind kind, cudaStream_t stream),
    (to, from, bytes, kind, stream))
NINFER_CUDA_REAL_FUNCTION(cudaMemcpyBatchAsync,
    (void* const* to, const void* const* from, const std::size_t* bytes, std::size_t count,
     cudaMemcpyAttributes* attrs, std::size_t* indices, std::size_t attr_count, cudaStream_t stream),
    (to, from, bytes, count, attrs, indices, attr_count, stream))
NINFER_CUDA_REAL_FUNCTION(cudaStreamSynchronize, (cudaStream_t stream), (stream))
NINFER_CUDA_REAL_FUNCTION(cudaStreamWaitEvent,
    (cudaStream_t stream, cudaEvent_t event, unsigned flags), (stream, event, flags))
#undef NINFER_CUDA_REAL_FUNCTION
} // namespace ninfer::test::cuda_interpose
#define NINFER_TEST_CUDA_WRAP(name) name
#define NINFER_TEST_CUDA_REAL(name) ::ninfer::test::cuda_interpose::real_##name
#else
extern "C" {
cudaError_t CUDARTAPI __real_cudaMalloc(void**, std::size_t);
cudaError_t CUDARTAPI __real_cudaMallocHost(void**, std::size_t);
cudaError_t CUDARTAPI __real_cudaFree(void*);
cudaError_t CUDARTAPI __real_cudaFreeHost(void*);
cudaError_t CUDARTAPI __real_cudaEventCreateWithFlags(cudaEvent_t*, unsigned);
cudaError_t CUDARTAPI __real_cudaEventRecord(cudaEvent_t, cudaStream_t);
cudaError_t CUDARTAPI __real_cudaMemcpyAsync(void*, const void*, std::size_t, cudaMemcpyKind, cudaStream_t);
cudaError_t CUDARTAPI __real_cudaMemcpyBatchAsync(void* const*, const void* const*, const std::size_t*,
    std::size_t, cudaMemcpyAttributes*, std::size_t*, std::size_t, cudaStream_t);
cudaError_t CUDARTAPI __real_cudaStreamSynchronize(cudaStream_t);
cudaError_t CUDARTAPI __real_cudaStreamWaitEvent(cudaStream_t, cudaEvent_t, unsigned);
}
#define NINFER_TEST_CUDA_WRAP(name) __wrap_##name
#define NINFER_TEST_CUDA_REAL(name) __real_##name
#endif
