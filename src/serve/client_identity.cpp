#include "serve/client_identity.h"

#include "core/sha256.h"

#include <algorithm>
#include <optional>
#include <span>
#include <utility>

namespace ninfer::serve {
namespace {

std::optional<std::string> parse_sha256_field(const RequestJson& body, const char* field) {
    if (!body.contains(field) || body.at(field).is_null()) { return std::nullopt; }
    if (!body.at(field).is_string()) {
        throw ApiException({.message = std::string(field) + " must be a string",
                            .param = field, .code = "invalid_ninfer_identity"});
    }
    return parse_client_identity_sha256(body.at(field).get<std::string>(), field);
}

[[noreturn]] void invalid_key(std::string message) {
    throw ApiException({.message = std::move(message), .param = "prompt_cache_key",
                        .code = "invalid_ninfer_identity"});
}

} // namespace

std::string parse_client_identity_sha256(std::string_view value, std::string_view field) {
    if (value.size() != 64 || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
        throw ApiException({.message = std::string(field) + " must be a 64-character lowercase SHA-256",
                            .param = std::string(field), .code = "invalid_ninfer_identity"});
    }
    return std::string(value);
}

void parse_client_identity(const RequestJson& body, GenerationRequest& request) {
    request.client_session_sha256 = parse_sha256_field(body, "ninfer_session");
    request.client_request_id = parse_sha256_field(body, "ninfer_request_id");
}

void require_authenticated_client_identity(const GenerationRequest& request,
                                           bool authentication_configured) {
    if (authentication_configured ||
        (!request.client_session_sha256 && !request.client_request_id)) { return; }
    throw ApiException({.status = 401,
                        .message = "ninfer_session and ninfer_request_id require API authentication",
                        .param = request.client_session_sha256 ? "ninfer_session" : "ninfer_request_id",
                        .code = "authentication_required"});
}

void apply_client_identity_cache_hints(const GenerationRequest& request,
                                       bool authentication_configured,
                                       ContextCacheHints& cache_hints) {
    require_authenticated_client_identity(request, authentication_configured);
    if (!request.client_session_sha256) { return; }
    cache_hints.session_key = "http:" + *request.client_session_sha256;
    cache_hints.reuse_scope = cache_hints.session_key;

    // Do not change update_session_index: store:false must not displace the retained lineage.
}

std::string prompt_cache_key_session_sha256(std::string_view key) {
    static constexpr char domain[] = "ninfer:prompt_cache_key:v1";
    crypto::Sha256 hasher;
    hasher.update(std::as_bytes(std::span(domain, sizeof(domain)))); // includes domain NUL
    hasher.update(std::as_bytes(std::span(key.data(), key.size())));
    return crypto::sha256_hex(hasher.finish());
}

std::optional<std::string> authenticated_principal_scope(std::string_view api_key) {
    if (api_key.empty()) { return std::nullopt; }
    static constexpr char domain[] = "ninfer:api_principal:v1";
    crypto::Sha256 hasher;
    hasher.update(std::as_bytes(std::span(domain, sizeof(domain))));
    hasher.update(std::as_bytes(std::span(api_key.data(), api_key.size())));
    return "principal:" + crypto::sha256_hex(hasher.finish());
}

void parse_prompt_cache_key(const RequestJson& body, GenerationRequest& request) {
    if (!body.contains("prompt_cache_key") || body.at("prompt_cache_key").is_null()) { return; }
    const auto& key = body.at("prompt_cache_key");
    if (!key.is_string() || key.get_ref<const std::string&>().empty()) {
        invalid_key("prompt_cache_key must be a non-empty string");
    }
    request.prompt_cache_session_sha256 =
        prompt_cache_key_session_sha256(key.get_ref<const std::string&>());
}

void resolve_client_session(GenerationRequest& request, bool authentication_configured,
                            bool session_header_present) {
    if (!request.prompt_cache_session_sha256) { return; }
    if (request.client_session_sha256 || session_header_present) {
        invalid_key("prompt_cache_key cannot be combined with ninfer_session or X-NInfer-Session");
    }
    if (authentication_configured) {
        request.client_session_sha256 = std::move(request.prompt_cache_session_sha256);
    }
    request.prompt_cache_session_sha256.reset();
}

} // namespace ninfer::serve
