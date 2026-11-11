#pragma once

#include "ninfer/types.h"

#include <chrono>
#include <cstdint>

namespace ninfer::runtime {

// One synchronous restore phase at a time. Scope exit also accounts for refused/throwing work;
// no synchronization, allocation, or change to the operation being observed.
class CheckpointRestoreTimer {
public:
    using Clock = std::chrono::steady_clock;
    using Field = std::uint64_t RuntimeCheckpointRestoreStats::*;

    CheckpointRestoreTimer(RuntimeCheckpointRestoreStats* stats, Field field) noexcept
        : stats_(stats), field_(field), started_(stats ? Clock::now() : Clock::time_point{}) {}
    ~CheckpointRestoreTimer() { select(field_); }
    CheckpointRestoreTimer(const CheckpointRestoreTimer&) = delete;
    CheckpointRestoreTimer& operator=(const CheckpointRestoreTimer&) = delete;

    void select(Field field) noexcept {
        if (!stats_) { return; }
        const auto now = Clock::now();
        stats_->*field_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - started_).count());
        started_ = now;
        field_ = field;
    }

private:
    RuntimeCheckpointRestoreStats* stats_;
    Field field_;
    Clock::time_point started_;
};

} // namespace ninfer::runtime
