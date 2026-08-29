#include "serve/http_server.h"
#include "serve/responses_schema.h"

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
    int failures = 0;
    ServeOptions options;
    options.max_request_bytes = 1234;

    const std::string session_digest(64, 'a');
    failures += check(ninfer::serve::parse_checkpoint_save_request_body(
                          Json{{"session_sha256", session_digest}}.dump()) == session_digest,
                      "exact checkpoint save body did not yield its session digest");
    const auto checkpoint_error = [](std::string body) {
        try {
            (void)ninfer::serve::parse_checkpoint_save_request_body(body);
            ninfer::serve::ApiError missing;
            missing.status = 0;
            return missing;
        } catch (const ninfer::serve::ApiException& error) { return error.error(); }
    };
    const ninfer::serve::ApiError malformed_checkpoint = checkpoint_error("{");
    failures += check(malformed_checkpoint.status == 400 &&
                          malformed_checkpoint.message == "request body is not valid JSON",
                      "malformed checkpoint save body was accepted");
    const ninfer::serve::ApiError missing_checkpoint = checkpoint_error("{}");
    failures += check(missing_checkpoint.status == 400 &&
                          missing_checkpoint.message == "session_sha256 is required",
                      "missing checkpoint session digest was accepted");
    const ninfer::serve::ApiError extra_checkpoint =
        checkpoint_error(Json{{"session_sha256", session_digest}, {"unexpected", true}}.dump());
    failures += check(extra_checkpoint.status == 400 &&
                          extra_checkpoint.message == "unsupported checkpoint field 'unexpected'",
                      "extra checkpoint save field was accepted");
    const ninfer::serve::ApiError typed_checkpoint =
        checkpoint_error(Json{{"session_sha256", 7}}.dump());
    failures += check(typed_checkpoint.status == 400 && typed_checkpoint.code == "invalid_session",
                      "non-string checkpoint session digest was accepted");
    const ninfer::serve::ApiError uppercase_checkpoint =
        checkpoint_error(Json{{"session_sha256", std::string(64, 'A')}}.dump());
    failures +=
        check(uppercase_checkpoint.status == 400 && uppercase_checkpoint.code == "invalid_session",
              "non-lowercase checkpoint session digest was accepted");
    httplib::Response erased_checkpoint;
    ninfer::serve::write_checkpoint_delete_response(
        erased_checkpoint, ninfer::serve::SessionCheckpointEraseResult::Erased);
    const Json erased_body = Json::parse(erased_checkpoint.body);
    failures += check(erased_checkpoint.status == 200 && erased_body.at("state") == "deleted",
                      "successful checkpoint DELETE did not return deleted/200");

    httplib::Response missing_delete;
    ninfer::serve::write_checkpoint_delete_response(
        missing_delete, ninfer::serve::SessionCheckpointEraseResult::Missing);
    const Json missing_delete_body = Json::parse(missing_delete.body);
    failures += check(missing_delete.status == 404 && missing_delete_body.at("state") == "missing",
                      "absent checkpoint DELETE did not return missing/404");

    httplib::Response conflicting_delete;
    ninfer::serve::write_checkpoint_delete_response(
        conflicting_delete, ninfer::serve::SessionCheckpointEraseResult::Conflict);
    const Json conflicting_delete_body = Json::parse(conflicting_delete.body);
    failures += check(conflicting_delete.status == 409 &&
                          conflicting_delete_body.at("state") == "conflict" &&
                          conflicting_delete.body.find(session_digest) == std::string::npos,
                      "refused checkpoint DELETE did not return content-safe conflict/409");

    const ninfer::serve::ApiError media_budget = ninfer::serve::request_error_to_api_error(
        ninfer::RequestError(ninfer::RequestErrorKind::MediaBudgetExceeded,
                             "vision tokens exceed processor budget"));
    failures += check(media_budget.status == 400 && media_budget.code == "media_budget_exceeded",
                      "media resource rejection did not map to HTTP 400");
    const ninfer::serve::ApiError context_limit = ninfer::serve::request_error_to_api_error(
        ninfer::RequestError(ninfer::RequestErrorKind::ContextLengthExceeded,
                             "prepared prompt has 200 tokens, exceeding Engine max_context 128"));
    failures +=
        check(context_limit.status == 400 && context_limit.code == "context_length_exceeded" &&
                  context_limit.message.find("200 tokens") != std::string::npos &&
                  context_limit.message.find("128") != std::string::npos,
              "context rejection lost its HTTP classification or capacity details");
    const ninfer::serve::ApiError thinking_capacity = ninfer::serve::request_error_to_api_error(
        ninfer::RequestError(ninfer::RequestErrorKind::ThinkingBudgetCapacityInsufficient,
                             "thinking control suffix does not fit"));
    failures += check(thinking_capacity.status == 400 &&
                          thinking_capacity.code == "thinking_budget_capacity_insufficient" &&
                          thinking_capacity.param.empty(),
                      "thinking budget capacity error mapping mismatch");
    const ninfer::serve::ApiError cancelled =
        ninfer::serve::request_error_to_api_error(ninfer::RequestError(
            ninfer::RequestErrorKind::Cancelled, "request cancelled during preparation"));
    failures += check(cancelled.status == 499 && cancelled.code == "client_disconnected",
                      "preparation cancellation did not retain its HTTP classification");

    const ninfer::serve::ApiError continuation =
        ninfer::serve::make_previous_response_not_found_error("resp_missing");
    failures +=
        check(continuation.status == 404 && continuation.code == "previous_response_not_found" &&
                  continuation.param == "previous_response_id",
              "failed previous_response_id continuation used the generic lookup error");
    const ninfer::serve::ApiError retrieval =
        ninfer::serve::make_response_not_found_error("resp_missing");
    failures += check(retrieval.status == 404 && retrieval.code == "response_not_found" &&
                          retrieval.param == "response_id",
                      "retrieval route lookup error changed with continuation handling");

    httplib::Request messages_request;
    messages_request.path = "/v1/messages";
    httplib::Response messages_response;
    messages_response.status = 413;
    const auto messages_result =
        ninfer::serve::handle_unrendered_http_error(options, messages_request, messages_response);
    const Json messages_body = Json::parse(messages_response.body);
    failures += check(messages_result == httplib::Server::HandlerResponse::Handled &&
                          messages_body.at("type") == "error" &&
                          messages_body.at("error").at("type") == "invalid_request_error" &&
                          messages_body.at("error").at("message").get<std::string>().find(
                              "1234 bytes") != std::string::npos,
                      "empty Anthropic 413 did not become a payload-limit error");

    httplib::Request openai_request;
    openai_request.path = "/v1/responses";
    httplib::Response openai_response;
    openai_response.status = 413;
    const auto openai_result =
        ninfer::serve::handle_unrendered_http_error(options, openai_request, openai_response);
    const Json openai_body = Json::parse(openai_response.body);
    failures += check(openai_result == httplib::Server::HandlerResponse::Handled &&
                          openai_body.at("error").at("code") == "request_too_large" &&
                          openai_body.at("error").at("message").get<std::string>().find(
                              "1234 bytes") != std::string::npos,
                      "empty OpenAI 413 did not become a payload-limit error");

    httplib::Response authored_response;
    authored_response.status = 413;
    authored_response.set_content(R"({"error":{"code":"application_error"}})", "application/json");
    const std::string authored_body = authored_response.body;
    const auto authored_result =
        ninfer::serve::handle_unrendered_http_error(options, openai_request, authored_response);
    failures += check(authored_result == httplib::Server::HandlerResponse::Unhandled &&
                          authored_response.body == authored_body,
                      "application-authored 413 was overwritten by the payload-limit handler");

    httplib::Response other_response;
    other_response.status = 400;
    const auto other_result =
        ninfer::serve::handle_unrendered_http_error(options, openai_request, other_response);
    failures += check(other_result == httplib::Server::HandlerResponse::Unhandled &&
                          other_response.body.empty(),
                      "non-413 response was changed by the payload-limit handler");

    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
