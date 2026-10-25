#include "serve/client_identity.h"
#include "serve/credential_compare.h"
#include "serve/http_server.h"
#include "serve/opaque_id.h"
#include "serve/openai_chat.h"
#include "serve/openai_responses.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

namespace {
using namespace ninfer::serve;
using Json = nlohmann::json;
int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << message << '\n';
    return 1;
}
template <typename Action> ApiError error(Action action) {
    try { action(); }
    catch (const ApiException& exception) { return exception.error(); }
    return {.status = 0};
}
int no_entropy(unsigned char*, int) { return 0; }
int fixed_entropy(unsigned char* out, int length) {
    for (int i = 0; i < length; ++i) { out[i] = static_cast<unsigned char>(i); }
    return 1;
}
} // namespace

int main() {
    int failures = 0;
    const std::string session(64, 'a'), other(64, 'b');
    httplib::Request header;
    header.headers.emplace("X-NInfer-Session", session);
    failures += check(parse_client_session_header(header, true) == session,
                      "authenticated session header was not parsed");
    failures += check(error([&] { (void)parse_client_session_header(header, false); }).status == 401,
                      "session header did not require authentication");
    GenerationRequest identity;
    apply_client_session_header(header, true, identity);
    failures += check(identity.client_session_sha256 == session, "header did not bind Responses");
    identity.client_session_sha256 = other;
    failures += check(error([&] { apply_client_session_header(header, true, identity); }).code ==
                          "invalid_ninfer_identity", "conflicting body/header identities were accepted");
    header.headers.emplace("X-NInfer-Session", session);
    failures += check(error([&] { (void)parse_client_session_header(header, true); }).status == 400,
                      "duplicate session headers accepted");
    failures += check(error([&] { (void)parse_client_identity_sha256(std::string(64, 'A'), "ninfer_session"); }).status == 400,
                      "uppercase session digest accepted");
    failures += check(credential_equal("test-key", "test-key") &&
                          !credential_equal("test-key", "test-keY") &&
                          !credential_equal("test-key", "test-key-longer"),
                      "credential equality did not distinguish values and lengths");
    failures += check(matches_bearer_credential("\tbeAREr  test-key \t", "test-key") &&
                          !matches_bearer_credential("Bearer test-key-extra", "test-key") &&
                          !matches_bearer_credential("Bearertest-key", "test-key") &&
                          !matches_bearer_credential("Bearer ", ""),
                      "bearer parsing changed credential or whitespace semantics");

    failures += check(prompt_cache_key_session_sha256("session") ==
                          "488bad291dc503a116f3c408a545ff97861aaeed63ffe9a11686cf58f16eebbe",
                      "prompt-cache-key domain or terminating NUL changed");
    failures += check(authenticated_principal_scope("test-key") ==
                          "principal:befe7d16f64f80cd9d6526d92f1dcec2dcf46489a277f373d352fa90da838295" &&
                          !authenticated_principal_scope(""),
                      "bearer principal scope changed or anonymous scope was invented");
    const auto chat_body = RequestJson{{"model", "m"},
        {"messages", RequestJson::array({RequestJson{{"role", "user"}, {"content", "hi"}}})},
        {"prompt_cache_key", "session"}};
    auto keyed = parse_chat_completion_request(chat_body, {}).generation;
    failures += check(keyed.prompt_cache_session_sha256 == prompt_cache_key_session_sha256("session"),
                      "Chat did not hash prompt_cache_key at parsing");
    resolve_client_session(keyed, false, false);
    failures += check(!keyed.client_session_sha256 && !keyed.prompt_cache_session_sha256,
                      "unauthenticated key selected an ownership namespace");
    keyed = parse_chat_completion_request(chat_body, {}).generation;
    resolve_client_session(keyed, true, false);
    ContextCacheHints hints;
    hints.update_session_index = false;
    apply_client_identity_cache_hints(keyed, true, hints);
    failures += check(hints.session_key == "http:" + *keyed.client_session_sha256 &&
                          hints.reuse_scope == hints.session_key && !hints.update_session_index,
                      "keyed store:false lost its private scope or advanced the session");
    keyed = parse_chat_completion_request(chat_body, {}).generation;
    failures += check(error([&] { resolve_client_session(keyed, true, true); }).param == "prompt_cache_key",
                      "Chat silently accepted prompt_cache_key alongside an unbound session header");
    auto conflicting = chat_body;
    conflicting["ninfer_session"] = session;
    keyed = parse_chat_completion_request(conflicting, {}).generation;
    failures += check(error([&] { resolve_client_session(keyed, true, false); }).status == 400,
                      "key and explicit body capability were accepted together");
    auto explicit_identity = chat_body;
    explicit_identity.erase("prompt_cache_key");
    explicit_identity["ninfer_request_id"] = session;
    keyed = parse_chat_completion_request(explicit_identity, {}).generation;
    failures += check(error([&] { require_authenticated_client_identity(keyed, false); }).status == 401,
                      "request correlation digest did not require authentication");
    for (const RequestJson value : {RequestJson(""), RequestJson(7), RequestJson::array()}) {
        auto invalid = chat_body;
        invalid["prompt_cache_key"] = value;
        failures += check(error([&] { (void)parse_chat_completion_request(invalid, {}); }).status == 400,
                          "invalid prompt_cache_key shape accepted");
    }

    failures += check(new_opaque_id_with_entropy("resp_", fixed_entropy) ==
                          "resp_000102030405060708090a0b0c0d0e0f",
                      "opaque identifier lost its full entropy encoding");
    bool entropy_refused = false;
    try { (void)new_opaque_id_with_entropy("resp_", no_entropy); }
    catch (const std::runtime_error&) { entropy_refused = true; }
    failures += check(entropy_refused, "failed cryptographic entropy used a predictable fallback");

    ServeOptions options;
    options.max_request_bytes = 1234;
    const auto media_budget = request_error_to_api_error(ninfer::RequestError(
        ninfer::RequestErrorKind::MediaBudgetExceeded, "vision tokens exceed processor budget"));
    const auto invalid_media = request_error_to_api_error(ninfer::RequestError(
        ninfer::RequestErrorKind::InvalidMedia, "failed to open media"));
    const auto context_limit = request_error_to_api_error(ninfer::RequestError(
        ninfer::RequestErrorKind::ContextLengthExceeded,
        "prepared prompt has 200 tokens, exceeding Engine max_context 128"));
    const auto thinking_capacity = request_error_to_api_error(ninfer::RequestError(
        ninfer::RequestErrorKind::ThinkingBudgetCapacityInsufficient,
        "thinking control suffix does not fit"));
    const auto cancelled = request_error_to_api_error(ninfer::RequestError(
        ninfer::RequestErrorKind::Cancelled, "request cancelled during preparation"));
    failures += check(media_budget.status == 400 && media_budget.code == "media_budget_exceeded" &&
                          invalid_media.status == 400 && invalid_media.code == "invalid_media" &&
                          invalid_media.param == "messages" &&
                          context_limit.status == 400 && context_limit.code == "context_length_exceeded" &&
                          context_limit.message.find("200 tokens") != std::string::npos &&
                          context_limit.message.find("128") != std::string::npos &&
                          thinking_capacity.status == 400 &&
                          thinking_capacity.code == "thinking_budget_capacity_insufficient" &&
                          thinking_capacity.param.empty() &&
                          cancelled.status == 499 && cancelled.code == "client_disconnected" &&
                          cancelled.param.empty(),
                      "upstream generation HTTP error classifications changed");
    for (const auto* path : {"/v1/responses", "/v1/messages"}) {
        httplib::Request request;
        request.path = path;
        httplib::Response response;
        response.status = 400;
        failures += check(handle_unrendered_http_error(options, request, response) ==
                              httplib::Server::HandlerResponse::Unhandled && response.body.empty(),
                          "unrendered non-payload-limit error was rewritten");
        const bool anthropic = request.path.starts_with("/v1/messages");
        const char* request_header = anthropic ? "request-id" : "x-request-id";
        const auto request_id = response.get_header_value(request_header);
        failures += check(request_id.starts_with("req_"), "HTTP request ID was not established");
        response.status = 413;
        failures += check(handle_unrendered_http_error(options, request, response) ==
                              httplib::Server::HandlerResponse::Handled,
                          "empty payload-limit error was not rendered");
        const auto body = Json::parse(response.body);
        failures += check(response.get_header_value_count(request_header) == 1 &&
                              response.get_header_value(request_header) == request_id &&
                              (anthropic
                                  ? body.at("type") == "error" &&
                                    body.at("error").at("type") == "request_too_large" &&
                                    body.at("request_id") == request_id &&
                                    !response.has_header("x-request-id")
                                  : body.at("error").at("code") == "request_too_large"),
                          "payload-limit envelope or correlated request ID changed");
        failures += check(body.at("error").at("message").get<std::string>().find("1234 bytes") !=
                              std::string::npos,
                          "payload limit was omitted from the protocol error");
        response.set_content(R"({"error":{"code":"application_error"}})", "application/json");
        const auto authored = response.body;
        failures += check(handle_unrendered_http_error(options, request, response) ==
                              httplib::Server::HandlerResponse::Unhandled && response.body == authored &&
                              response.get_header_value_count(request_header) == 1 &&
                              response.get_header_value(request_header) == request_id,
                          "application-authored error was overwritten");
    }
    httplib::Request missing;
    missing.path = "/v1/messages/missing";
    httplib::Response not_found;
    not_found.status = 404;
    not_found.set_header("request-id", "req_existing");
    failures += check(handle_unrendered_http_error(options, missing, not_found) ==
                          httplib::Server::HandlerResponse::Handled &&
                          not_found.status == 404 &&
                          Json::parse(not_found.body).at("error").at("type") == "not_found_error" &&
                          Json::parse(not_found.body).at("request_id") == "req_existing" &&
                          not_found.get_header_value_count("request-id") == 1,
                      "missing Anthropic resource lost protocol envelope or request ID");
    const auto continuation = make_previous_response_not_found_error("resp_missing");
    const auto retrieval = make_response_not_found_error("resp_missing");
    failures += check(continuation.status == 404 && continuation.param == "previous_response_id" &&
                          continuation.code == "previous_response_not_found" &&
                          retrieval.status == 404 && retrieval.param == "response_id" &&
                          retrieval.code == "response_not_found",
                      "continuation and retrieval error taxonomy collapsed");
    if (failures == 0) { std::cout << "http identity and error tests passed\n"; }
    return failures == 0 ? 0 : 1;
}
