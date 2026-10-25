#pragma once

#include "ops/linear_attention/kimi_delta_attention/chunked/workspace.h"

namespace ninfer::ops::detail::kimi_delta_attention {
enum class RecurrenceRoute { Recurrent, Chunked };

struct RecurrencePlan {
    RecurrenceRoute route;
    int value_tile;
    std::size_t workspace_bytes;
};

RecurrenceRoute select_recurrence_route(int tokens);
int select_value_tile(int value_heads, int multiprocessor_count);
std::size_t recurrence_workspace_bytes(int value_heads, int tokens);
RecurrencePlan select_recurrence_plan(int value_heads, int tokens, int sm_count);
} // namespace ninfer::ops::detail::kimi_delta_attention
