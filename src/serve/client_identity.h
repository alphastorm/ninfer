#pragma once

#include "serve/request.h"

#include <nlohmann/json.hpp>

namespace ninfer::serve {

// Optional local-agent correlation fields. Values are caller-computed SHA-256 digests so routine
// logs and cache indexes never receive raw session or request identifiers.
void parse_client_identity(const nlohmann::json& body, GenerationRequest& request);
void require_authenticated_client_identity(const GenerationRequest& request,
                                           bool authentication_configured);

} // namespace ninfer::serve
