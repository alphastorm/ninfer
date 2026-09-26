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
    constexpr auto busy_ns = std::chrono::nanoseconds(kBusy).count();
    gpu_keep_warm_spin_kernel<<<1, 32, 0, stream_>>>(static_cast<unsigned long long>(busy_ns));
    return cudaGetLastError() == cudaSuccess;
}

} // namespace ninfer
