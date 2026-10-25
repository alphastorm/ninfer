#include "core/gpu_keep_warm.h"

#include <stdexcept>
#include <string>

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

GpuKeepWarm::GpuKeepWarm(const DeviceContext& device)
    : period_(device.compute_capability() == 89 ? 100 : 10),
      busy_(device.compute_capability() == 89 ? 50000 : 3500) {
    const auto error = cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking);
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string("GPU keep-warm stream creation failed: ") +
                                 cudaGetErrorString(error));
    }
}

GpuKeepWarm::~GpuKeepWarm() {
    // The driver retires an outstanding bounded spin before releasing the stream.
    (void)cudaStreamDestroy(stream_);
}

bool GpuKeepWarm::launch() const noexcept {
    const cudaError_t previous = cudaStreamQuery(stream_);
    if (previous == cudaErrorNotReady) {
        // cudaStreamQuery may leave NotReady in the worker's last-error slot. Do not let the
        // Engine's next launch check mistake an outstanding warm spin for a model error.
        (void)cudaGetLastError();
        return true;
    }
    if (previous != cudaSuccess) { return false; }
    const auto busy_ns = std::chrono::nanoseconds(busy_).count();
    gpu_keep_warm_spin_kernel<<<1, 32, 0, stream_>>>(static_cast<unsigned long long>(busy_ns));
    return cudaGetLastError() == cudaSuccess;
}

} // namespace ninfer
