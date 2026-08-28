#include "serve/http_contract.h"
#include "serve/request.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

namespace {

using Json = nlohmann::json;
using ninfer::serve::ServeOptions;

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    using HandlerResponse = httplib::Server::HandlerResponse;
    int failures          = 0;

    ServeOptions open;
    httplib::Request status;
    status.method = "GET";
    status.path   = "/v1/ninfer/status";
    httplib::Response status_response;
    const auto status_result = ninfer::serve::authorize_http_request(open, status, status_response);
    failures += check(status_result == HandlerResponse::Handled && status_response.status == 401 &&
                          Json::parse(status_response.body).at("error").at("code") ==
                              "authentication_required",
                      "status did not fail closed without configured authentication");

    httplib::Request health;
    health.method = "GET";
    health.path   = "/health";
    httplib::Response health_response;
    failures += check(ninfer::serve::authorize_http_request(open, health, health_response) ==
                              HandlerResponse::Unhandled &&
                          health_response.body.empty(),
                      "health unexpectedly required authentication");

    httplib::Request ordinary;
    ordinary.method = "POST";
    ordinary.path   = "/v1/responses";
    httplib::Response ordinary_response;
    failures += check(ninfer::serve::authorize_http_request(open, ordinary, ordinary_response) ==
                          HandlerResponse::Unhandled,
                      "ordinary endpoint changed when authentication is disabled");

    ServeOptions protected_options;
    protected_options.api_key = "local-secret";
    httplib::Response missing_response;
    failures +=
        check(ninfer::serve::authorize_http_request(protected_options, ordinary,
                                                    missing_response) == HandlerResponse::Handled &&
                  missing_response.status == 401 &&
                  Json::parse(missing_response.body).at("error").at("code") == "invalid_api_key",
              "missing bearer credential was accepted");

    httplib::Request bearer = ordinary;
    bearer.headers.emplace("Authorization", "Bearer local-secret");
    httplib::Response bearer_response;
    failures +=
        check(ninfer::serve::authorize_http_request(protected_options, bearer, bearer_response) ==
                      HandlerResponse::Unhandled &&
                  bearer_response.body.empty(),
              "valid bearer credential was rejected");

    httplib::Request anthropic;
    anthropic.method = "POST";
    anthropic.path   = "/v1/messages";
    anthropic.headers.emplace("x-api-key", "local-secret");
    httplib::Response anthropic_response;
    failures +=
        check(ninfer::serve::authorize_http_request(
                  protected_options, anthropic, anthropic_response) == HandlerResponse::Unhandled,
              "valid Anthropic x-api-key credential was rejected");

    httplib::Request wrong_anthropic = anthropic;
    wrong_anthropic.headers.clear();
    wrong_anthropic.headers.emplace("x-api-key", "wrong");
    httplib::Response wrong_anthropic_response;
    const auto wrong_anthropic_result = ninfer::serve::authorize_http_request(
        protected_options, wrong_anthropic, wrong_anthropic_response);
    const Json wrong_anthropic_body = Json::parse(wrong_anthropic_response.body);
    failures += check(wrong_anthropic_result == HandlerResponse::Handled &&
                          wrong_anthropic_response.status == 401 &&
                          wrong_anthropic_body.at("type") == "error" &&
                          wrong_anthropic_body.at("error").at("type") == "authentication_error",
                      "Anthropic authentication failure used the wrong wire envelope");

    httplib::Request preflight = ordinary;
    preflight.method           = "OPTIONS";
    httplib::Response preflight_response;
    failures +=
        check(ninfer::serve::authorize_http_request(
                  protected_options, preflight, preflight_response) == HandlerResponse::Unhandled,
              "CORS preflight unexpectedly required credentials");

    ServeOptions payload_options;
    payload_options.max_request_bytes = 1234;
    httplib::Response payload_response;
    payload_response.status = 413;
    const auto payload_result =
        ninfer::serve::handle_unrendered_http_error(payload_options, ordinary, payload_response);
    const Json payload_body = Json::parse(payload_response.body);
    failures += check(payload_result == HandlerResponse::Handled &&
                          payload_body.at("error").at("code") == "request_too_large" &&
                          payload_body.at("error").at("message").get<std::string>().find(
                              "1234 bytes") != std::string::npos,
                      "empty transport 413 did not become a payload-limit error");

    httplib::Response authored_response;
    authored_response.status = 413;
    authored_response.set_content(R"({"error":{"code":"application_error"}})", "application/json");
    const std::string authored_body = authored_response.body;
    failures +=
        check(ninfer::serve::handle_unrendered_http_error(
                  payload_options, ordinary, authored_response) == HandlerResponse::Unhandled &&
                  authored_response.body == authored_body,
              "application-authored 413 was overwritten by the transport error handler");

    const std::string session_digest(64, 'a');
    httplib::Request session_request;
    session_request.headers.emplace("X-NInfer-Session", session_digest);
    failures +=
        check(ninfer::serve::response_session_identity(session_request, true) == session_digest,
              "single stored-response session header was not accepted");
    httplib::Request ordinary_stored_route;
    failures += check(!ninfer::serve::response_session_identity(ordinary_stored_route, false),
                      "missing stored-response session header did not stay absent");

    httplib::Request duplicate_session = session_request;
    duplicate_session.headers.emplace("x-ninfer-session", std::string(64, 'b'));
    std::string duplicate_code;
    try {
        (void)ninfer::serve::response_session_identity(duplicate_session, true);
    } catch (const ninfer::serve::ApiException& exception) {
        duplicate_code = exception.error().code;
    }
    failures += check(duplicate_code == "invalid_ninfer_identity",
                      "duplicate stored-response session headers were accepted");

    std::string unauthenticated_code;
    try {
        (void)ninfer::serve::response_session_identity(session_request, false);
    } catch (const ninfer::serve::ApiException& exception) {
        unauthenticated_code = exception.error().code;
    }
    failures += check(unauthenticated_code == "authentication_required",
                      "stored-response session header bypassed API authentication");

    ninfer::serve::GenerationRequest header_only;
    ninfer::serve::apply_response_session_identity(session_request, true, header_only);
    failures += check(header_only.client_session_sha256 == session_digest,
                      "POST session header did not scope stored response state");
    ninfer::serve::GenerationRequest matching_body;
    matching_body.client_session_sha256 = session_digest;
    ninfer::serve::apply_response_session_identity(session_request, true, matching_body);
    failures += check(matching_body.client_session_sha256 == session_digest,
                      "matching POST body and header identity was rejected");
    ninfer::serve::GenerationRequest mismatched_body;
    mismatched_body.client_session_sha256 = std::string(64, 'b');
    std::string mismatch_code;
    try {
        ninfer::serve::apply_response_session_identity(session_request, true, mismatched_body);
    } catch (const ninfer::serve::ApiException& exception) {
        mismatch_code = exception.error().code;
    }
    failures += check(mismatch_code == "invalid_ninfer_identity",
                      "mismatched POST body/header session identity was accepted");

    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
