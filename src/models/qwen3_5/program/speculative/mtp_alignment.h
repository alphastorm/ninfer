#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace ninfer::models::qwen3_5 {

struct MtpAlignmentWindow {
    std::uint32_t hidden_begin             = 0;
    std::uint32_t position_begin           = 0;
    std::uint32_t shifted_embedding_begin  = 0;
    std::uint32_t columns                  = 0;
    bool final_column_uses_generated_token = false;
};

[[nodiscard]] inline MtpAlignmentWindow plan_mtp_alignment_window(std::uint32_t prompt_tokens,
                                                                  std::uint32_t chunk_begin,
                                                                  std::uint32_t columns) {
    if (columns == 0 || chunk_begin >= prompt_tokens || columns > prompt_tokens - chunk_begin) {
        throw std::invalid_argument("MTP alignment window is outside the prompt");
    }
    if (chunk_begin == std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("MTP shifted embedding position exceeds uint32");
    }
    return MtpAlignmentWindow{
        .hidden_begin                      = chunk_begin,
        .position_begin                    = chunk_begin,
        .shifted_embedding_begin           = chunk_begin + 1,
        .columns                           = columns,
        .final_column_uses_generated_token = columns == prompt_tokens - chunk_begin,
    };
}

} // namespace ninfer::models::qwen3_5
