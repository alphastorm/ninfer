#pragma once

#include "serve/serve_options.h"

#include "ninfer/build_info.h"
#include "ninfer/types.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace ninfer::serve {

[[nodiscard]] nlohmann::json server_identity_json(const ServeOptions& options,
                                                  const ninfer::LoadSummary& load);
[[nodiscard]] std::string format_server_identity(const ServeOptions& options,
                                                 const ninfer::LoadSummary& load);
[[nodiscard]] std::string session_checkpoint_tenant_sha256(std::string_view api_key);
[[nodiscard]] nlohmann::json session_checkpoint_runtime_fingerprint(
    const ServeOptions& options, const ninfer::EngineOptions& engine,
    const ninfer::LoadSummary& load, const ninfer::MemorySummary& memory,
    const ninfer::BuildInfo& build);

} // namespace ninfer::serve