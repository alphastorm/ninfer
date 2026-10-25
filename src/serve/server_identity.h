#pragma once

#include "runtime/contract/runtime_fingerprint.h"
#include "serve/serve_options.h"

#include <nlohmann/json.hpp>

#include <string>

namespace ninfer::serve {

nlohmann::json server_identity_json(const ServeOptions& options, const ninfer::LoadSummary& load);
std::string format_server_identity(const ServeOptions& options, const ninfer::LoadSummary& load);

// K2 requires loaded frontend/native-layout identities supplied by their authorities. There is
// deliberately no options-only builder that could invent a layout or hash a resource filename.
std::string session_checkpoint_effective_config_sha256(const runtime::RuntimeFingerprintV2& value);
nlohmann::json session_checkpoint_runtime_fingerprint(const runtime::RuntimeFingerprintV2& value);
[[nodiscard]] bool validate_session_checkpoint_runtime_fingerprint(const nlohmann::json& value) noexcept;

} // namespace ninfer::serve
