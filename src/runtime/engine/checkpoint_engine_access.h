#pragma once

#include "runtime/contract/continuation_checkpoint.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ninfer {
class Engine;
class PreparedPrompt;
} // namespace ninfer

namespace ninfer::runtime {

class CheckpointEngineAccess {
public:
    static void set_checkpoint_tag(PreparedPrompt& prompt, std::string tag);

    [[nodiscard]] static std::shared_ptr<ContinuationCheckpointReadQueue>
    make_read_queue(Engine& engine, const std::filesystem::path& root);

    [[nodiscard]] static std::optional<ContinuationCheckpointStats>
    checkpoint_session(Engine& engine, std::string_view session_sha256,
                       std::string_view checkpoint_tag, ContinuationCheckpointWriter& writer,
                       std::size_t staging_bytes, SessionCheckpointSkipDetail* skip = nullptr);

    [[nodiscard]] static std::optional<ContinuationCheckpointStats>
    restore_session(Engine& engine, std::string_view session_sha256, std::string checkpoint_tag,
                    const ContinuationCheckpointReader& reader,
                    ContinuationCheckpointStats expected, std::size_t staging_bytes);
};

} // namespace ninfer::runtime
