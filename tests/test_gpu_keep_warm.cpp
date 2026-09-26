#include "core/device.h"
#include "core/gpu_keep_warm.h"

#include <cuda_runtime.h>

#include <chrono>
#include <iostream>

namespace {

using Clock = std::chrono::steady_clock;

bool cuda_unavailable(cudaError_t err) {
    return err == cudaErrorNoDevice || err == cudaErrorInsufficientDriver;
}

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

} // namespace

int main() {
    int count                   = 0;
    const cudaError_t count_err = cudaGetDeviceCount(&count);
    if (cuda_unavailable(count_err)) {
        std::cout << "SKIP: no usable CUDA device\n";
        return 77;
    }
    if (count_err != cudaSuccess || count == 0) { return fail("cudaGetDeviceCount failed"); }

    ninfer::DeviceContext device(0);
    ninfer::GpuKeepWarm keep_warm;
    // Load the kernel before timing anything.
    if (!keep_warm.launch() || cudaDeviceSynchronize() != cudaSuccess) {
        return fail("warm-up launch failed");
    }

    Clock::time_point start = Clock::now();
    if (!keep_warm.launch() || cudaDeviceSynchronize() != cudaSuccess) {
        return fail("launch on an idle stream failed");
    }
    const double one_spin_ms = ms(Clock::now() - start);
    if (one_spin_ms < ms(ninfer::GpuKeepWarm::kBusy) / 2) {
        std::cerr << "a launch on an idle stream did not spin: " << one_spin_ms << " ms\n";
        return 1;
    }

    // A launch while the previous spin runs skips that period, so at most one spin is in flight. It
    // must not leave cudaErrorNotReady behind as the thread's last error: the Engine checks the
    // last error after each of its own launches and would fail the request.
    start = Clock::now();
    if (!keep_warm.launch()) { return fail("launch on an idle stream failed"); }
    if (!keep_warm.launch()) { return fail("launch while a spin runs failed"); }
    const cudaError_t last = cudaGetLastError();
    if (last != cudaSuccess) {
        std::cerr << "launch while a spin runs left " << cudaGetErrorName(last) << " behind\n";
        return 1;
    }
    if (cudaDeviceSynchronize() != cudaSuccess) { return fail("synchronize failed"); }
    const double overlapping_ms = ms(Clock::now() - start);
    if (overlapping_ms > 1.5 * one_spin_ms) {
        std::cerr << "a launch while a spin ran queued a second spin: " << overlapping_ms
                  << " ms against " << one_spin_ms << " ms for one\n";
        return 1;
    }

    std::cout << "ok: one spin " << one_spin_ms << " ms, two launches " << overlapping_ms
              << " ms\n";
    return 0;
}
