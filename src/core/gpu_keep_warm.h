#pragma once

#include "core/device.h"

#include <cuda_runtime.h>

#include <chrono>

namespace ninfer {

// One memory-free warp on an owned nonblocking stream. Construct, launch and destroy on the
// device-bound Engine worker. A busy stream skips a period rather than queueing another spin.
class GpuKeepWarm {
public:
    explicit GpuKeepWarm(const DeviceContext& device);
    ~GpuKeepWarm();

    GpuKeepWarm(const GpuKeepWarm&)            = delete;
    GpuKeepWarm& operator=(const GpuKeepWarm&) = delete;

    // The Windows sm_89 driver needs a longer utilization interval than the other shipped GPUs.
    [[nodiscard]] std::chrono::milliseconds period() const noexcept { return period_; }
    [[nodiscard]] std::chrono::microseconds busy() const noexcept { return busy_; }
    [[nodiscard]] bool launch() const noexcept;

private:
    cudaStream_t stream_ = nullptr;
    std::chrono::milliseconds period_;
    std::chrono::microseconds busy_;
};

} // namespace ninfer
