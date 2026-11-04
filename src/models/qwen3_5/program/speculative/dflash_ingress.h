#pragma once

#include "models/qwen3_5/program/round_buffers.h"
#include "models/common/context/work.h"
#include <algorithm>

namespace ninfer::models::qwen3_5::speculative {
// Only the pre-existing live-prefix fields change; physical layout and proposal width do not.
inline void write_extent_ingress(DFlashDecodeIngress& ingress, std::size_t row, std::uint32_t k,
                                  std::uint32_t frontier, std::int32_t rope_delta,
                                  std::uint32_t extent) {
    ingress.proposal_valid_columns[row] = static_cast<std::int32_t>(k + 1);
    ingress.proposal_extents[row] = static_cast<std::int32_t>(extent);
    ingress.target_valid_columns[row] = static_cast<std::int32_t>(extent + 1);
    for (std::uint32_t j = 0; j <= k; ++j)
        ingress.target_rope_positions[row * (k + 1) + j] =
            context::checked_i32(frontier + std::min(j, extent), "DFlash target RoPE position") +
            rope_delta;
}
} // namespace ninfer::models::qwen3_5::speculative
