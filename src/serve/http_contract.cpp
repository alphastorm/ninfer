#include "serve/http_contract.h"

#include "serve/anthropic_schema.h"
#include "serve/openai_schema.h"
#include "serve/request.h"

#include <string>

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

} // namespace ninfer::serve
