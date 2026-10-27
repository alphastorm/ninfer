#pragma once

#include "core/pdl_scope.h"

#include <cuda_runtime.h>

#include <cstddef>
#include <utility>

namespace ninfer::pdl {

struct LaunchConfig {
    dim3 grid;
    dim3 block;
    std::size_t dynamic_smem_bytes = 0;
    cudaStream_t stream            = nullptr;
};

// Launches a consumer kernel as a programmatic dependent of the immediately preceding producer
// kernel in the same stream. Every consumer control path that reads producer output must first call
// wait_for_dependencies().
template <class... KernelArgs, class... CallArgs>
[[nodiscard]] inline cudaError_t
launch_dependent(const LaunchConfig& launch, void (*kernel)(KernelArgs...), CallArgs&&... args) {
#if !defined(NINFER_SM86) && !defined(NINFER_SM89)
    cudaLaunchAttribute attribute{};
    attribute.id = cudaLaunchAttributeProgrammaticStreamSerialization;
    attribute.val.programmaticStreamSerializationAllowed = 1;
#endif

    cudaLaunchConfig_t config{};
    config.gridDim          = launch.grid;
    config.blockDim         = launch.block;
    config.dynamicSmemBytes = launch.dynamic_smem_bytes;
    config.stream           = launch.stream;
#if !defined(NINFER_SM86) && !defined(NINFER_SM89)
    config.attrs            = &attribute;
    config.numAttrs         = 1;
#endif

    return cudaLaunchKernelEx(&config, kernel, std::forward<CallArgs>(args)...);
}

// Launches a kernel that implements the streaming-consumer protocol: it may stage immutable
// weights before wait_for_dependencies(), touches nothing else before it, and triggers its own
// dependents only after its K loop. Inside a ConsumerCaptureScope on a capturing stream the launch
// becomes a programmatic dependent of the preceding kernel; otherwise it is stream-serialized.
template <class... KernelArgs, class... CallArgs>
[[nodiscard]] inline cudaError_t
launch_consumer(const LaunchConfig& launch, void (*kernel)(KernelArgs...), CallArgs&&... args) {
#if !defined(NINFER_SM86) && !defined(NINFER_SM89)
    if (ConsumerCaptureScope::active()) {
        cudaStreamCaptureStatus status = cudaStreamCaptureStatusNone;
        const cudaError_t query        = cudaStreamIsCapturing(launch.stream, &status);
        if (query != cudaSuccess) return query;
        if (status == cudaStreamCaptureStatusActive)
            return launch_dependent(launch, kernel, std::forward<CallArgs>(args)...);
    }
#endif
    cudaLaunchConfig_t config{};
    config.gridDim          = launch.grid;
    config.blockDim         = launch.block;
    config.dynamicSmemBytes = launch.dynamic_smem_bytes;
    config.stream           = launch.stream;
    return cudaLaunchKernelEx(&config, kernel, std::forward<CallArgs>(args)...);
}

// Every producer CTA must call this at least once or exit. This enables dependent scheduling but
// does not make producer writes visible to the consumer.
__device__ __forceinline__ void trigger_dependents() {
#if defined(__CUDA_ARCH__) && __CUDA_ARCH__ >= 900
    cudaTriggerProgrammaticLaunchCompletion();
#endif
}

// Call on every consumer control path before its first access to producer-dependent data.
__device__ __forceinline__ void wait_for_dependencies() {
#if defined(__CUDA_ARCH__) && __CUDA_ARCH__ >= 900
    cudaGridDependencySynchronize();
#endif
}

} // namespace ninfer::pdl
