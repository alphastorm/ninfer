#pragma once

#include "ninfer/types.h"
#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>

namespace ninfer::models::qwen3_5::speculative {

struct Observation {
    std::uint32_t extent = 0, accepted = 0, licensed = 0, committed = 0;
    bool clipped = false, terminal = false, discarded = false;
};

// No clocks, allocations, global history, lane identity or device-side state.
class ExtentCap {
public:
    ExtentCap() noexcept = default;
    explicit ExtentCap(const DFlashOptions& options) noexcept
        : mode_(options.mode), rounds_(options.rounds), threshold_(options.threshold),
          cap_(options.cap), armed_(options.mode == DFlashMode::ForcedCap),
          capped_(options.mode == DFlashMode::ForcedCap) {}
    [[nodiscard]] bool armed() const noexcept { return armed_; }
    [[nodiscard]] std::uint32_t limit(std::uint32_t k) const noexcept {
        return armed_ && capped_ ? std::min(k, cap_) : k;
    }
    void commit(const Observation& o) noexcept {
        if (mode_ == DFlashMode::Off || mode_ == DFlashMode::ForcedCap || o.discarded ||
            o.clipped || o.extent == 0 || o.committed != o.licensed || o.licensed == 0)
            return;
        if (!armed_) {
            if (o.extent != 7) return;
            sum_ -= history_[cursor_];
            history_[cursor_] = o.accepted;
            sum_ += o.accepted;
            cursor_ = (cursor_ + 1) % rounds_;
            count_ = std::min(count_ + 1, rounds_);
            armed_ = count_ == rounds_ &&
                     (mode_ == DFlashMode::Calibrate || sum_ <= threshold_);
            if (armed_) capped_ = mode_ == DFlashMode::Calibrate || o.accepted <= cap_;
        } else if (mode_ == DFlashMode::Calibrate) {
            capped_ = !capped_;
        } else {
            capped_ = o.extent == 7 ? o.accepted <= cap_ : o.accepted < o.extent;
        }
    }
private:
    DFlashMode mode_ = DFlashMode::Off;
    std::uint32_t rounds_ = 16, threshold_ = 44, cap_ = 3;
    std::array<std::uint32_t, 32> history_{};
    std::uint32_t cursor_ = 0, count_ = 0, sum_ = 0;
    bool armed_ = false, capped_ = false;
};

struct ExtentPlan {
    std::uint32_t extent;
    bool budget_clipped, capacity_clipped;
};
[[nodiscard]] inline ExtentPlan plan_extent(const ExtentCap& governor, std::uint32_t k,
                                            std::uint32_t budget, std::uint32_t capacity,
                                            std::uint32_t frontier) noexcept {
    const auto limit = governor.limit(k);
    const auto by_budget = budget > 1 ? budget - 1 : 0U;
    const auto by_capacity = capacity > frontier ? capacity - frontier - 1 : 0U;
    return {std::min({limit, by_budget, by_capacity}), by_budget < limit, by_capacity < limit};
}

inline constexpr std::size_t kTraceRoundLimit = 131072;
inline constexpr std::size_t kTraceTokenLimit = 262144;
inline void require_trace_room(const DFlashTrace& trace, std::size_t records, std::size_t tokens) {
    if (trace.rounds.size() > kTraceRoundLimit ||
        records > kTraceRoundLimit - trace.rounds.size() ||
        trace.prompt_token_ids.size() > kTraceTokenLimit ||
        trace.output_token_ids.size() > kTraceTokenLimit - trace.prompt_token_ids.size() ||
        tokens > kTraceTokenLimit - trace.prompt_token_ids.size() - trace.output_token_ids.size())
        throw std::length_error("DFlash trace overflow: incomplete evidence refused");
}
inline void append_trace(DFlashTrace& trace, const DFlashRoundRecord& record,
                         std::span<const TokenId> committed) {
    require_trace_room(trace, 1, committed.size());
    auto entry = record;
    entry.output_offset = static_cast<std::uint32_t>(trace.output_token_ids.size());
    trace.rounds.push_back(entry);
    trace.output_token_ids.insert(trace.output_token_ids.end(), committed.begin(), committed.end());
}
static_assert(sizeof(DFlashRoundRecord) * kTraceRoundLimit +
                  sizeof(TokenId) * kTraceTokenLimit < 64ULL * 1024 * 1024);
} // namespace ninfer::models::qwen3_5::speculative
