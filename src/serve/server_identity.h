#pragma once

#include "serve/serve_options.h"

#include "ninfer/types.h"

#include <nlohmann/json.hpp>

#include <string>

namespace ninfer::serve {

[[nodiscard]] nlohmann::json server_identity_json(const ServeOptions& options,
                                                  const ninfer::LoadSummary& load);
[[nodiscard]] std::string format_server_identity(const ServeOptions& options,
                                                 const ninfer::LoadSummary& load);
[[nodiscard]] nlohmann::json session_checkpoint_runtime_fingerprint(
    const ServeOptions& options, const ninfer::EngineOptions& engine,
    const ninfer::LoadSummary& load);

} // namespace ninfer::serve
