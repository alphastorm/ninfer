#include "serve/anthropic_schema.h"
#include "serve/client_identity.h"
#include "serve/openai_schema.h"
#include "serve/responses_schema.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <iostream>
#include <string>

namespace {

using Json = nlohmann::json;
using namespace ninfer::serve;

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << message << '\n';
    return 1;
}

std::string api_code(const std::function<void()>& action) {
    try {
        action();
    } catch (const ApiException& error) { return error.error().code; } catch (...) {
        return "wrong_exception";
    }
    return {};
}

RequestLimits limits() {
    RequestLimits value;
    value.default_max_tokens = 64;
    return value;
}

Json identity() {
    return Json{{"ninfer_session", std::string(64, 'a')},
                {"ninfer_request_id", std::string(64, 'b')}};
}

int test_all_protocols() {
    Json chat = identity();
    chat["model"] = "m";
    chat["messages"] = Json::array({Json{{"role", "user"}, {"content", "hello"}}});

    Json messages = chat;
    messages["max_tokens"] = 8;

    Json responses = identity();
    responses["model"] = "m";
    responses["input"] = "hello";

    const GenerationRequest chat_request = parse_chat_completion_request(chat, limits());
    const GenerationRequest messages_request = parse_messages_request(messages, limits());
    const GenerationRequest responses_request = parse_responses_request(responses, limits()).generation;
    const GenerationRequest counted_request =
        parse_response_input_tokens_request(responses, limits()).generation;

    int failures = 0;
    for (const GenerationRequest* request :
         {&chat_request, &messages_request, &responses_request, &counted_request}) {
        failures += check(request->client_session_sha256 == std::string(64, 'a') &&
                              request->client_request_id == std::string(64, 'b'),
                          "protocol dropped authenticated client identity");
        failures += check(api_code([&] { require_authenticated_client_identity(*request, false); }) ==
                              "authentication_required",
                          "identity was accepted without configured authentication");
        failures += check(api_code([&] { require_authenticated_client_identity(*request, true); }).empty(),
                          "configured authentication rejected a valid identity");
    }
    return failures;
}

int test_digest_validation() {
    Json body = identity();
    body["model"] = "m";
    body["messages"] = Json::array({Json{{"role", "user"}, {"content", "hello"}}});

    int failures = 0;
    failures += check(parse_client_identity_sha256(std::string(64, 'a'), "ninfer_session") ==
                          std::string(64, 'a'),
                      "shared session header parser changed a valid digest");
    failures += check(api_code([&] {
                          (void)parse_client_identity_sha256(std::string(64, 'A'),
                                                            "ninfer_session");
                      }) == "invalid_ninfer_identity",
                      "shared session header parser accepted uppercase digest bytes");
    Json uppercase = body;
    uppercase["ninfer_session"] = std::string(64, 'A');
    failures += check(api_code([&] { (void)parse_chat_completion_request(uppercase, limits()); }) ==
                          "invalid_ninfer_identity",
                      "uppercase session digest was accepted");

    Json non_string = body;
    non_string["ninfer_request_id"] = 7;
    failures += check(api_code([&] { (void)parse_chat_completion_request(non_string, limits()); }) ==
                          "invalid_ninfer_identity",
                      "non-string request digest was accepted");

    Json omitted = body;
    omitted.erase("ninfer_session");
    omitted.erase("ninfer_request_id");
    const GenerationRequest ordinary = parse_chat_completion_request(omitted, limits());
    failures += check(api_code([&] { require_authenticated_client_identity(ordinary, false); }).empty(),
                      "ordinary unauthenticated request was rejected");
    return failures;
}

} // namespace

int main() {
    const int failures = test_all_protocols() + test_digest_validation();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
