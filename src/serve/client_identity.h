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

// An OpenAI prompt_cache_key names a client session without a NInfer-specific field. The key is
// hashed under its own domain, so it cannot collide with a caller-chosen ninfer_session digest,
// and the raw key never leaves these calls.
std::string prompt_cache_key_session_sha256(std::string_view key);
void parse_prompt_cache_key(const nlohmann::json& body, GenerationRequest& request);
// Makes a parsed prompt_cache_key the session identity when API authentication is configured.
// A request names its session once: a key together with ninfer_session, or on a request that
// carries X-NInfer-Session, is refused on every route, including chat completions, which never
// binds the header. Without authentication the key names nothing and the request stays in the
// anonymous pool, exactly as omission does.
void resolve_client_session(GenerationRequest& request, bool authentication_configured,
                            bool session_header_present);

} // namespace ninfer::serve
