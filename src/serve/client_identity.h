#pragma once

#include "serve/request.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace ninfer::serve {

// Optional local-agent correlation fields. Values are caller-computed SHA-256 digests so routine
// logs and cache indexes never receive raw session or request identifiers.
std::string parse_client_identity_sha256(std::string_view value, std::string_view field);
void parse_client_identity(const nlohmann::json& body, GenerationRequest& request);
void require_authenticated_client_identity(const GenerationRequest& request,
                                           bool authentication_configured);
void apply_client_identity_cache_hints(const GenerationRequest& request,
                                       bool authentication_configured,
                                       ContextCacheHints& cache_hints);

} // namespace ninfer::serve
