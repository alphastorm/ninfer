#include "core/device.h"
#include "core/gpu_keep_warm.h"

#include <cuda_runtime.h>

#include <chrono>
#include <iostream>

namespace {

using Clock = std::chrono::steady_clock;

double ms(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    int count = 0;
    const auto error = cudaGetDeviceCount(&count);
    if (error == cudaErrorNoDevice || error == cudaErrorInsufficientDriver ||
        (error == cudaSuccess && count == 0)) {
        std::cout << "SKIP: no usable CUDA device\n";
        return 77;
    }
    if (error != cudaSuccess) { return fail("cudaGetDeviceCount failed"); }

    ninfer::DeviceContext device;
    ninfer::GpuKeepWarm keep_warm(device);
    const bool ada = device.compute_capability() == 89;
    if (keep_warm.period() != std::chrono::milliseconds(ada ? 100 : 10) ||
        keep_warm.busy() != std::chrono::microseconds(ada ? 50000 : 3500)) {
        return fail("selected-device pacing changed");
    }
    if (!keep_warm.launch() || cudaDeviceSynchronize() != cudaSuccess) {
        return fail("warm-up launch failed");
    }

    auto start = Clock::now();
    if (!keep_warm.launch() || cudaDeviceSynchronize() != cudaSuccess) {
        return fail("launch on an idle stream failed");
    }
    const double one_spin_ms = ms(Clock::now() - start);
    if (one_spin_ms < ms(keep_warm.busy()) / 2) {
        return fail("an idle launch did not spin for its bounded interval");
    }

    start = Clock::now();
    if (!keep_warm.launch() || !keep_warm.launch()) {
        return fail("overlapping launch failed");
    }
    if (const auto last = cudaGetLastError(); last != cudaSuccess) {
        std::cerr << "overlapping launch left " << cudaGetErrorName(last) << " behind\n";
        return 1;
    }
    if (cudaDeviceSynchronize() != cudaSuccess) { return fail("synchronize failed"); }
    const double overlapping_ms = ms(Clock::now() - start);
    if (overlapping_ms > 1.5 * one_spin_ms) {
        std::cerr << "queued a second spin: " << overlapping_ms << " ms versus "
                  << one_spin_ms << " ms\n";
        return 1;
    }
    std::cout << "ok: one spin " << one_spin_ms << " ms, two launches " << overlapping_ms
              << " ms\n";
    return 0;
}
