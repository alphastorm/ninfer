#include "serve/http_contract.h"

#include "serve/anthropic_schema.h"
#include "serve/client_identity.h"
#include "serve/openai_schema.h"
#include "serve/request.h"

#include <string>
#include <utility>

namespace ninfer::serve {
namespace {

void write_error(httplib::Response& response, const ApiError& error) {
    response.status = error.status;
    response.set_content(make_error_body(error), "application/json");
}

void write_messages_error(httplib::Response& response, const ApiError& error) {
    response.status = error.status;
    response.set_content(make_messages_error_body(error), "application/json");
}

} // namespace

httplib::Server::HandlerResponse handle_unrendered_http_error(const ServeOptions& options,
                                                              const httplib::Request& request,
                                                              httplib::Response& response) {
    if (response.status != 413 || !response.body.empty()) {
        return httplib::Server::HandlerResponse::Unhandled;
    }

    ApiError error;
    error.status  = 413;
    error.type    = "invalid_request_error";
    error.code    = "request_too_large";
    error.message = "request body exceeds the configured payload limit of " +
                    std::to_string(options.max_request_bytes) + " bytes";
    if (request.path.rfind("/v1/messages", 0) == 0) {
        write_messages_error(response, error);
    } else {
        write_error(response, error);
    }
    return httplib::Server::HandlerResponse::Handled;
}

httplib::Server::HandlerResponse authorize_http_request(const ServeOptions& options,
                                                        const httplib::Request& request,
                                                        httplib::Response& response) {
    if (request.path == "/health" || request.method == "OPTIONS") {
        return httplib::Server::HandlerResponse::Unhandled;
    }
    if (options.api_key.empty() && request.path != "/v1/ninfer/status") {
        return httplib::Server::HandlerResponse::Unhandled;
    }

    ApiError error;
    if (options.api_key.empty()) {
        error.status  = 401;
        error.code    = "authentication_required";
        error.message = "server status requires API authentication";
    } else {
        const bool bearer_ok =
            request.get_header_value("Authorization") == ("Bearer " + options.api_key);
        const bool x_api_key_ok = request.get_header_value("x-api-key") == options.api_key;
        if (bearer_ok || x_api_key_ok) {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        error.status  = 401;
        error.code    = "invalid_api_key";
        error.message = "missing or invalid API key";
    }

    if (request.path.rfind("/v1/messages", 0) == 0) {
        write_messages_error(response, error);
    } else {
        write_error(response, error);
    }
    return httplib::Server::HandlerResponse::Handled;
}

std::optional<std::string> response_session_identity(const httplib::Request& request,
                                                     bool authentication_configured) {
    constexpr const char* header = "X-NInfer-Session";
    const std::size_t count      = request.get_header_value_count(header);
    if (count == 0) { return std::nullopt; }
    if (count != 1) {
        ApiError error;
        error.message = "X-NInfer-Session must occur exactly once";
        error.param   = "ninfer_session";
        error.code    = "invalid_ninfer_identity";
        throw ApiException(std::move(error));
    }
    GenerationRequest identity;
    identity.client_session_sha256 =
        parse_client_identity_sha256(request.get_header_value(header), "ninfer_session");
    require_authenticated_client_identity(identity, authentication_configured);
    return identity.client_session_sha256;
}

void apply_response_session_identity(const httplib::Request& request,
                                     bool authentication_configured,
                                     GenerationRequest& generation) {
    const std::optional<std::string> header_session =
        response_session_identity(request, authentication_configured);
    if (!header_session) { return; }
    if (generation.client_session_sha256 &&
        *generation.client_session_sha256 != *header_session) {
        ApiError error;
        error.status  = 400;
        error.type    = "invalid_request_error";
        error.param   = "ninfer_session";
        error.code    = "invalid_ninfer_identity";
        error.message = "X-NInfer-Session conflicts with ninfer_session";
        throw ApiException(std::move(error));
    }
    generation.client_session_sha256 = header_session;
}

} // namespace ninfer::serve
