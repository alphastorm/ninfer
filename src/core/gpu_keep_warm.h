#pragma once

#include "core/device.h"

#include <cuda_runtime.h>

#include <chrono>

namespace ninfer {

// Keeps the GPU out of its low-power idle P-states between requests. After its last work an RTX
// 5090 steps P1 -> P3 -> P5 -> P8 (270 MHz SM, 405 MHz memory) within about 9 s, and the first
// prefill after that runs up to 2.3x slower while the clocks ramp (omp-ninfer EXP-060). Each
// launch enqueues one single-warp kernel that spins for kBusy of wall time on an owned
// non-blocking stream and returns at once; the caller paces launches every kPeriod, and a launch
// is skipped while the previous spin still runs, so at most one spin overlaps the work that
// follows. The kernel reads and writes no memory, so it cannot change an output. Construct and
// destroy on a thread bound to the device.
class GpuKeepWarm {
public:
    // The driver steps down on time-based GPU utilization, and each card reads it differently. On
    // an RTX 5090 a single-warp spin busy 30% of the time held P1 and 25% did not (omp-ninfer
    // EXP-061); 3.5 ms of every 10 ms leaves a margin at about 70 W above idle (EXP-062), and a
    // short period keeps any spin overlapping an admitted request brief. An RTX 4090 (sm_89) under
    // the Windows driver reached P8 with spins of 3.5-6 ms every 10 ms but held P2 with a spin of
    // 40 ms or more every 100 ms (EXP-064), so the sm_89 build spins 50 ms of every 100 ms. The
    // spin occupies one warp, and a pending request wakes the caller at once.
    static constexpr bool kLongSpin = kCompiledComputeCapability == 89;
    static constexpr std::chrono::milliseconds kPeriod{kLongSpin ? 100 : 10};
    static constexpr std::chrono::microseconds kBusy{kLongSpin ? 50000 : 3500};

    GpuKeepWarm();
    ~GpuKeepWarm();

    GpuKeepWarm(const GpuKeepWarm&)            = delete;
    GpuKeepWarm& operator=(const GpuKeepWarm&) = delete;

    // Enqueues one spin unless the previous one still runs. Returns false on a CUDA error; the
    // caller stops keeping warm.
    [[nodiscard]] bool launch() const noexcept;

private:
    cudaStream_t stream_ = nullptr;
};

} // namespace ninfer
