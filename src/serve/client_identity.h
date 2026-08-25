#pragma once

#include "serve/request.h"

#include <nlohmann/json_fwd.hpp>

namespace ninfer::serve {

// Optional local-agent correlation fields. Values are caller-computed SHA-256 digests so routine
// logs and cache indexes never receive raw session or request identifiers.
void parse_client_identity(const nlohmann::json& body, GenerationRequest& request);

} // namespace ninfer::serve
