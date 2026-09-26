#include "core/gpu_keep_warm.h"

#include "core/device.h"

namespace ninfer {
namespace {

__device__ __forceinline__ unsigned long long global_timer_ns() {
    unsigned long long now;
    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(now));
    return now;
}

__global__ void gpu_keep_warm_spin_kernel(unsigned long long busy_ns) {
    const unsigned long long start = global_timer_ns();
    while (global_timer_ns() - start < busy_ns) {}
}

} // namespace

GpuKeepWarm::GpuKeepWarm() {
    CUDA_CHECK(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking));
}

GpuKeepWarm::~GpuKeepWarm() {
    // A spin still queued does not delay destruction; the driver releases the stream after it.
    (void)cudaStreamDestroy(stream_);
}

bool GpuKeepWarm::launch() const noexcept {
    // Another context holding the GPU can delay a spin past the next period; skip rather than
    // queue behind it. The runtime can record cudaErrorNotReady as this thread's last error, which
    // the Engine's next launch check would report, so clear it (as PyTorch's CUDAStream::query
    // does).
    const cudaError_t previous = cudaStreamQuery(stream_);
    if (previous == cudaErrorNotReady) {
        (void)cudaGetLastError();
        return true;
    }
    if (previous != cudaSuccess) { return false; }
    constexpr auto busy_ns = std::chrono::nanoseconds(kBusy).count();
    gpu_keep_warm_spin_kernel<<<1, 32, 0, stream_>>>(static_cast<unsigned long long>(busy_ns));
    return cudaGetLastError() == cudaSuccess;
}

} // namespace ninfer
