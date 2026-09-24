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

    // reclaim is consulted only when a shared capacity gate blocks the import: it decides which
    // other live sessions may be dropped because their checkpoints are already current on disk.
    [[nodiscard]] static std::optional<ContinuationCheckpointStats>
    restore_session(Engine& engine, std::string_view session_sha256, std::string checkpoint_tag,
                    const ContinuationCheckpointReader& reader,
                    ContinuationCheckpointStats expected, std::size_t staging_bytes,
                    SessionRestoreSkipDetail* skip                = nullptr,
                    const ReclaimableSessionOracle* reclaim       = nullptr);

    // Installs, or with null removes, the handler admission consults before it drops a live
    // session's newest turn. Removal waits out a call in progress.
    static void set_pressure_checkpoint_handler(Engine& engine,
                                                std::shared_ptr<PressureCheckpointHandler> handler);
};

} // namespace ninfer::runtime
