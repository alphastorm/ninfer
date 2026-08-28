#pragma once

#include "runtime/contract/continuation_checkpoint.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ninfer {
class Engine;
class PreparedPrompt;
}

namespace ninfer::runtime {

class CheckpointEngineAccess {
public:
    static void bind_checkpoint_session(PreparedPrompt& prompt,
                                        AuthenticatedCheckpointNamespace checkpoint_namespace,
                                        std::string tag);

    [[nodiscard]] static std::optional<ContinuationCheckpointStats>
    checkpoint_session(Engine& engine,
                       const AuthenticatedCheckpointNamespace& checkpoint_namespace,
                       std::string_view checkpoint_tag, ContinuationCheckpointWriter& writer,
                       std::size_t staging_bytes);

    [[nodiscard]] static std::optional<ContinuationCheckpointStats>
    restore_session(Engine& engine,
                    const AuthenticatedCheckpointNamespace& checkpoint_namespace,
                    std::string checkpoint_tag, const ContinuationCheckpointReader& reader,
                    ContinuationCheckpointStats expected, std::size_t staging_bytes);
};

} // namespace ninfer::runtime
