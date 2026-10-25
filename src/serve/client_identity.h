#pragma once

#include "serve/request.h"
#include "serve/request_json.h"

#include <optional>
#include <string>
#include <string_view>

namespace ninfer::serve {

// Digests are capabilities inside the server's single authenticated principal.
std::string parse_client_identity_sha256(std::string_view value, std::string_view field);
void parse_client_identity(const RequestJson& body, GenerationRequest& request);
void require_authenticated_client_identity(const GenerationRequest& request,
                                           bool authentication_configured);
void apply_client_identity_cache_hints(const GenerationRequest& request,
                                       bool authentication_configured,
                                       ContextCacheHints& cache_hints);
std::string prompt_cache_key_session_sha256(std::string_view key);
std::optional<std::string> authenticated_principal_scope(std::string_view api_key);
void parse_prompt_cache_key(const RequestJson& body, GenerationRequest& request);
// A prompt key and an explicit session/header are mutually exclusive on every route.
// An unauthenticated key is validated but does not select an ownership namespace.
void resolve_client_session(GenerationRequest& request, bool authentication_configured,
                            bool session_header_present);

} // namespace ninfer::serve
