#pragma once

#include "runtime/engine/context_cache/context_cost.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ninfer::runtime {

// A prediction, not GPU preemption: one minimum-sized step can exceed a small budget.
// Full-chunk execution permits remain owned by Native until the shortened step settles.
class PrefillPacing {
public:
    PrefillPacing(std::uint32_t budget_ms, const ContextMachineCostModel& costs) noexcept
        : budget_ns_(std::uint64_t{budget_ms} * 1'000'000), costs_(costs) {}

    [[nodiscard]] bool enabled() const noexcept { return budget_ns_ != 0; }

    // Zero means execute the planned unit, including its existing capture boundaries.
    [[nodiscard]] std::uint32_t cap(std::uint64_t prefix, std::uint32_t planned) const noexcept {
        constexpr std::uint32_t alignment = 128;
        if (!enabled() || planned <= alignment) return 0;
        const auto fits = [&](std::uint32_t tokens) {
            return static_cast<double>(predict(prefix, tokens)) * correction_ <= budget_ns_;
        };
        if (fits(planned)) return 0;
        std::uint32_t low = 1, high = (planned - 1) / alignment;
        while (low < high) {
            const auto middle = low + (high - low + 1) / 2;
            if (fits(middle * alignment)) low = middle;
            else high = middle - 1;
        }
        return low * alignment;
    }

    // Text steps only; solo prefill warms the estimate before a neighbour starts decoding.
    void observe(std::uint64_t prefix, std::uint32_t tokens, std::uint64_t elapsed_ns) noexcept {
        if (!enabled() || tokens == 0 || elapsed_ns == 0) return;
        const auto predicted = predict(prefix, tokens);
        if (predicted == 0 || predicted == std::numeric_limits<std::uint64_t>::max()) return;
        const double ratio = std::clamp(static_cast<double>(elapsed_ns) / predicted, 1.0 / 64, 64.0);
        // Geometric moving average, weight 1/4, without logarithms or allocation.
        correction_ *= std::sqrt(std::sqrt(ratio / correction_));
    }

    [[nodiscard]] bool over_budget(std::uint64_t elapsed_ns) const noexcept {
        return enabled() && elapsed_ns > budget_ns_;
    }

private:
    [[nodiscard]] std::uint64_t predict(std::uint64_t prefix, std::uint32_t tokens) const noexcept {
        return costs_.prefill_ns(make_prefill_work(prefix, tokens, 0, 0, tokens));
    }

    std::uint64_t budget_ns_;
    ContextMachineCostModel costs_;
    double correction_ = 1.0;
};

} // namespace ninfer::runtime
