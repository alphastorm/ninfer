#pragma once

#include "core/device.h"

#include <optional>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ninfer {

// Experimental scheduling controls, read once at construction; all default off.
// No state, graph topology, arithmetic or durable identity depends on these controls.
[[nodiscard]] bool host_decode_flag(const char* name);

class DecodeHostPolicy {
public:
    DecodeHostPolicy();
    ~DecodeHostPolicy() noexcept;
    DecodeHostPolicy(const DecodeHostPolicy&) = delete;
    DecodeHostPolicy& operator=(const DecodeHostPolicy&) = delete;

private:
    void restore() noexcept;
#ifdef _WIN32
    bool timer_ = false;
    bool power_ = false;
    bool priority_ = false;
    bool affinity_ = false;
    int previous_priority_ = THREAD_PRIORITY_NORMAL;
    GROUP_AFFINITY previous_affinity_{};
#endif
};

// Program-owned event, distinct from graph-captured handoff/ready events. Only the
// completed decode submission uses it; exceptional cleanup retains a stream fence.
class DecodeRoundWait {
public:
    explicit DecodeRoundWait(const DeviceContext& device);
    void submitted(cudaStream_t stream);
    void wait(const DeviceContext& device) const;

private:
    const bool flush_;
    std::optional<CudaCompletionEvent> event_;
};

} // namespace ninfer
