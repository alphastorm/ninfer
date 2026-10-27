#pragma once

// Host-only half of core/pdl.cuh, includable from plain C++ translation units.

namespace ninfer::pdl {

// Programmatic launches of streaming consumers are opt-in per CUDA Graph capture. An owner whose
// captured weights stay immutable for the graph's lifetime (the Qwen3.5 dense decode graphs)
// holds this scope on the capturing thread; every other launch stays fully stream-serialized.
class ConsumerCaptureScope {
public:
    ConsumerCaptureScope() noexcept : previous_(active_ref()) { active_ref() = true; }
    ~ConsumerCaptureScope() { active_ref() = previous_; }
    ConsumerCaptureScope(const ConsumerCaptureScope&)            = delete;
    ConsumerCaptureScope& operator=(const ConsumerCaptureScope&) = delete;

    [[nodiscard]] static bool active() noexcept { return active_ref(); }

private:
    static bool& active_ref() noexcept {
        thread_local bool active = false;
        return active;
    }
    bool previous_;
};

} // namespace ninfer::pdl
