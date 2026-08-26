#include "serve/client_identity.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <string>
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
    std::string value = body.at(field).get<std::string>();
    const bool valid =
        value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    if (!valid) {
        ApiError error;
        error.message = std::string(field) + " must be a 64-character lowercase SHA-256";
        error.param   = field;
        error.code    = "invalid_ninfer_identity";
        throw ApiException(std::move(error));
    }
    return value;
}

} // namespace

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

} // namespace ninfer::serve
