#pragma once

#include <cuda_runtime.h>

#include <chrono>

namespace ninfer {

// Keeps the GPU out of its low-power idle P-states between requests. After its last work an RTX
// 5090 steps P1 -> P3 -> P5 -> P8 (270 MHz SM, 405 MHz memory) within about 9 s, and the first
// prefill after that runs up to 2.3x slower while the clocks ramp (omp-ninfer EXP-060). Each
// launch enqueues one single-warp kernel that spins for kBusy of wall time on an owned
// non-blocking stream and returns at once; the caller paces launches every kPeriod. The kernel
// reads and writes no memory, so it cannot change an output; at most one spin overlaps the work
// that follows it. Construct and destroy on a thread bound to the device.
class GpuKeepWarm {
public:
    // The driver steps down on time-based GPU utilization: a single-warp spin busy 30% of the
    // time held P1 and 25% did not (omp-ninfer EXP-061); 35% leaves a margin at about 60-65 W
    // above idle. A short period keeps any spin overlapping an admitted request brief.
    static constexpr std::chrono::milliseconds kPeriod{10};
    static constexpr std::chrono::microseconds kBusy{3500};

    GpuKeepWarm();
    ~GpuKeepWarm();

    GpuKeepWarm(const GpuKeepWarm&)            = delete;
    GpuKeepWarm& operator=(const GpuKeepWarm&) = delete;

    // Enqueues one spin. Returns false if the launch failed; the caller stops keeping warm.
    [[nodiscard]] bool launch() const noexcept;

private:
    cudaStream_t stream_ = nullptr;
};

} // namespace ninfer
