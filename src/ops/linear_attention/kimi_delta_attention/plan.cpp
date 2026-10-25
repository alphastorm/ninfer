#include "ops/linear_attention/kimi_delta_attention/plan.h"
#include <climits>
#include <stdexcept>

namespace ninfer::ops::detail::kimi_delta_attention {
int select_value_tile(int value_heads, int multiprocessor_count) {
    // Small grids need the narrowest stripe. Otherwise minimize SM waves weighted
    // by the measured per-wave cost of this mixed BF16/TF32 kernel on SM120a.
    // Wider stripes reduce replicated packet reads but retain more state per CTA.
    if (static_cast<std::int64_t>(value_heads) * 16 <= multiprocessor_count) return 8;
    int best               = 64;
    std::int64_t best_work = INT64_MAX;

    struct Choice {
        int tile, cost;
    };

    for (const auto [tile, cost] : {Choice{64, 34}, Choice{32, 20}, Choice{16, 13}}) {
        const auto blocks = static_cast<std::int64_t>(value_heads) * (kStateDim / tile);
        const auto waves  = (blocks + multiprocessor_count - 1) / multiprocessor_count;
        const auto work   = waves * cost;
        if (work < best_work) {
            best      = tile;
            best_work = work;
        }
    }
    return best;
}

RecurrenceRoute select_recurrence_route(int tokens) {
    return tokens < 12 ? RecurrenceRoute::Recurrent : RecurrenceRoute::Chunked;
}

std::size_t recurrence_workspace_bytes(int value_heads, int tokens) {
    return select_recurrence_route(tokens) == RecurrenceRoute::Recurrent
               ? 0
               : chunked_workspace_bytes(value_heads, tokens);
}

RecurrencePlan select_recurrence_plan(int value_heads, int tokens, int sm_count) {
    const auto route = select_recurrence_route(tokens);
    if (route == RecurrenceRoute::Chunked && sm_count <= 0)
        throw std::invalid_argument("kimi_delta_attention: positive multiprocessor count required");
    return {route, route == RecurrenceRoute::Chunked ? select_value_tile(value_heads, sm_count) : 0,
            recurrence_workspace_bytes(value_heads, tokens)};
}
} // namespace ninfer::ops::detail::kimi_delta_attention
