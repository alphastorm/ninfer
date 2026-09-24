#include "serve/client_identity.h"

#include "core/sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ninfer::serve {
namespace {

std::optional<std::string> parse_sha256_field(const nlohmann::json& body, const char* field) {
    if (!body.contains(field) || body.at(field).is_null()) { return std::nullopt; }
    if (!body.at(field).is_string()) {
        ApiError error;
        error.message = std::string(field) + " must be a string";
        error.param   = field;
        error.code    = "invalid_ninfer_identity";
        throw ApiException(std::move(error));
    }
    return parse_client_identity_sha256(body.at(field).get<std::string>(), field);
}

[[noreturn]] void throw_prompt_cache_key_error(std::string message) {
    ApiError error;
    error.message = std::move(message);
    error.param   = "prompt_cache_key";
    error.code    = "invalid_ninfer_identity";
    throw ApiException(std::move(error));
}

} // namespace

std::string parse_client_identity_sha256(std::string_view value, std::string_view field) {
    const bool valid =
        value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    if (!valid) {
        ApiError error;
        error.message = std::string(field) + " must be a 64-character lowercase SHA-256";
        error.param   = std::string(field);
        error.code    = "invalid_ninfer_identity";
        throw ApiException(std::move(error));
    }
    return std::string(value);
}

void parse_client_identity(const nlohmann::json& body, GenerationRequest& request) {
    request.client_session_sha256 = parse_sha256_field(body, "ninfer_session");
    request.client_request_id     = parse_sha256_field(body, "ninfer_request_id");
}

void require_authenticated_client_identity(const GenerationRequest& request,
                                           bool authentication_configured) {
    if ((!request.client_session_sha256 && !request.client_request_id) ||
        authentication_configured) {
        return;
    }

    ApiError error;
    error.status  = 401;
    error.message = "ninfer_session and ninfer_request_id require API authentication";
    error.param   = request.client_session_sha256 ? "ninfer_session" : "ninfer_request_id";
    error.code    = "authentication_required";
    throw ApiException(std::move(error));
}

void apply_client_identity_cache_hints(const GenerationRequest& request,
                                       bool authentication_configured,
                                       ContextCacheHints& cache_hints) {
    require_authenticated_client_identity(request, authentication_configured);
    if (!request.client_session_sha256) {
        // Omission deliberately selects the single-principal anonymous cache pool. It provides no
        // ownership or retrieval boundary; callers that need isolation must send ninfer_session.
        return;
    }
    // NInfer currently has one configured API key and therefore one authenticated principal. The
    // client-supplied digest is a subordinate session capability inside that principal, not an
    // independent tenant credential; callers must keep it unguessable and private.

    cache_hints.session_key          = "http:" + *request.client_session_sha256;
    cache_hints.retention            = CacheRetentionHint::LiveSession;
    cache_hints.update_session_index = true;
}

std::string prompt_cache_key_session_sha256(std::string_view key) {
    // The terminating NUL of the domain separates it from the key.
    static constexpr char domain[] = "ninfer:prompt_cache_key:v1";
    crypto::Sha256 hasher;
    hasher.update(std::as_bytes(std::span(domain, sizeof(domain))));
    hasher.update(std::as_bytes(std::span(key.data(), key.size())));
    return crypto::sha256_hex(hasher.finish());
}

void parse_prompt_cache_key(const nlohmann::json& body, GenerationRequest& request) {
    if (!body.contains("prompt_cache_key") || body.at("prompt_cache_key").is_null()) { return; }
    const nlohmann::json& key = body.at("prompt_cache_key");
    if (!key.is_string() || key.get_ref<const std::string&>().empty()) {
        throw_prompt_cache_key_error("prompt_cache_key must be a non-empty string");
    }
    request.prompt_cache_session_sha256 =
        prompt_cache_key_session_sha256(key.get_ref<const std::string&>());
}

void resolve_client_session(GenerationRequest& request, bool authentication_configured,
                            bool session_header_present) {
    if (!request.prompt_cache_session_sha256) { return; }
    if (request.client_session_sha256 || session_header_present) {
        throw_prompt_cache_key_error(
            "prompt_cache_key cannot be combined with ninfer_session or X-NInfer-Session");
    }
    if (authentication_configured) {
        request.client_session_sha256 = std::move(request.prompt_cache_session_sha256);
    }
    request.prompt_cache_session_sha256.reset();
}

} // namespace ninfer::serve
