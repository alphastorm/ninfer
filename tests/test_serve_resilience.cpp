// Request-to-template and tool-response regressions; no Engine, artifact or GPU required.
#include "models/common/qwen/frontend/chat_template.h"
#include "serve/anthropic_messages.h"
#include "serve/generation_service.h"
#include "serve/openai_chat.h"
#include "serve/translate.h"
#include "text/tool_arguments.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fi = ninfer::models::qwen::frontend;
using namespace ninfer::serve;
using Json = RequestJson;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

fi::RenderedChat render(const fi::CompiledChatTemplate& compiled, const GenerationRequest& request) {
    const auto semantics = resolve_prompt_semantics(request, ServeOptions{});
    const auto input = to_prompt_input(request, semantics, {});
    std::vector<fi::ChatMessage> messages;
    for (const auto& message : input.messages) {
        fi::ChatMessage next;
        next.role = message.role;
        next.reasoning_content = message.reasoning_content;
        next.tool_call_id = message.tool_call_id;
        for (const auto& part : message.parts) next.parts.push_back(fi::ChatPart::text_part(part.text));
        for (const auto& call : message.tool_calls)
            next.tool_calls.push_back({call.id, call.name, call.arguments_json, call.kind});
        messages.push_back(std::move(next));
    }
    return compiled.render(messages, {.enable_thinking = input.options.enable_thinking,
                                      .reasoning_effort = input.options.reasoning_effort,
                                      .preserve_thinking = input.options.preserve_thinking,
                                      .chat_template_kwargs_json = input.options.chat_template_kwargs_json,
                                      .tool_jsons = input.options.tool_jsons});
}

Json chat_body() {
    return Json{{"model", "local"}, {"messages", Json::array({Json{{"role", "user"}, {"content", "hi"}}})},
                {"max_tokens", 4}};
}

Json event_body(const std::string& wire) {
    const auto begin = wire.find("data: ");
    require(begin != std::string::npos, "missing event data");
    return Json::parse(wire.substr(begin + 6));
}

void malformed_history(const fi::CompiledChatTemplate& compiled) {
    for (const auto& arguments : {std::string("{\"text\":\"unterminated"), std::string("[1,2]"),
                                  std::string("null"), std::string("\"raw\"")}) {
        auto body = chat_body();
        body["reasoning_effort"] = "medium";
        body["messages"].push_back(Json{{"role", "assistant"}, {"content", ""},
            {"tool_calls", Json::array({Json{{"id", "call_1"}, {"type", "function"},
                {"function", Json{{"name", "lookup"}, {"arguments", arguments}}}}})}});
        body["messages"].push_back(Json{{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "bad input"}});
        body["messages"].push_back(Json{{"role", "user"}, {"content", "try again"}});
        const auto rendered = render(compiled, parse_chat_completion_request(body, {}).generation);
        require(rendered.text.find(arguments) != std::string::npos, "raw malformed arguments were lost");
        require(rendered.text.find("try again") != std::string::npos, "later turn was not rendered");
    }
}

void effort_tiers(const fi::CompiledChatTemplate& compiled) {
    const std::vector<std::string> tiers{"none", "minimal", "low", "medium", "high", "xhigh", "max"};
    std::vector<std::string> rendered;
    for (const auto& tier : tiers) {
        auto body = chat_body();
        body["reasoning_effort"] = tier;
        const auto output = render(compiled, parse_chat_completion_request(body, {}).generation);
        require(output.starts_in_reasoning == (tier != "none"), "effort changed thinking toggle");
        rendered.push_back(output.text);
    }
    require(rendered[1] == rendered[2], "minimal did not map to low");
    require(rendered[4] == rendered[5] && rendered[6] == rendered[5], "high/max did not map to xhigh");
}

void disabled_thinking(const fi::CompiledChatTemplate& compiled) {
    for (const char* tier : {"none", "minimal", "low", "medium", "high", "xhigh", "max"}) {
        const Json body{{"model", "local"}, {"messages", Json::array({Json{{"role", "user"}, {"content", "hi"}}})},
                        {"max_tokens", 4}, {"thinking", Json{{"type", "disabled"}}},
                        {"output_config", Json{{"effort", tier}}}};
        const auto request = parse_anthropic_messages_request(body, {});
        require(!render(compiled, request.generation).starts_in_reasoning, "disabled thinking was overridden");
    }
}

void malformed_response() {
    const auto identity = make_anthropic_response_identity("req_bad", "local");
    for (const auto& arguments : {std::string("{\"text\":\"unterminated"), std::string("[1,2]"),
                                  std::string("null"), std::string("\"raw\"")}) {
        GenerationOutcome outcome;
        outcome.finish_reason = ninfer::FinishReason::StopToken;
        outcome.tool_calls.push_back({.name = "lookup", .arguments_json = arguments});
        const auto aggregate = Json::parse(make_anthropic_messages_response(identity, outcome));
        require(aggregate["content"][0]["input"] == Json{{"input", arguments}},
                "aggregate lost raw tool input");
        AnthropicMessagesStream stream(identity, 0);
        (void)stream.start();
        const auto events = stream.finish(outcome);
        bool saw_arguments = false;
        for (const auto& wire : events) {
            const auto event = event_body(wire);
            if (event["type"] == "content_block_delta" && event["delta"]["type"] == "input_json_delta")
                saw_arguments = Json::parse(event["delta"]["partial_json"].get<std::string>()) ==
                                Json{{"input", arguments}};
        }
        require(saw_arguments, "stream lost raw tool input");
    }
}

void unchanged_inputs(const std::string& source, const fi::CompiledChatTemplate& compiled) {
    const ninfer::text::JinjaTemplate golden(source, "unmodified-template");
    for (const auto effort : {ninfer::ReasoningEffort::Low, ninfer::ReasoningEffort::Medium,
                               ninfer::ReasoningEffort::XHigh}) {
        const Json context{{"messages", Json::array({Json{{"role", "user"}, {"content", "hi"}}})},
                           {"add_generation_prompt", true},
                           {"reasoning_effort", ninfer::reasoning_effort_name(effort)}};
        fi::ChatMessage user;
        user.parts.push_back(fi::ChatPart::text_part("hi"));
        require(compiled.render({user}, {.reasoning_effort = effort}).text ==
                    golden.render(context).text,
                "previously accepted effort changed rendered bytes");
    }
    const auto custom = fi::CompiledChatTemplate::resolve("{{ reasoning_effort }}");
    fi::ChatMessage user;
    user.parts.push_back(fi::ChatPart::text_part("hi"));
    require(custom.render({user}, {.reasoning_effort = ninfer::ReasoningEffort::High}).text == "high",
            "a template supporting high was unnecessarily normalized");
    require(fi::normalize_reasoning_effort("high", 0) == "high" &&
                fi::normalize_reasoning_effort("high", (1u << 3) | (1u << 5)) == "xhigh",
            "effort probing fallback or upward tie-break changed");
    const std::string object = R"({ "zeta": 1, "alpha": 2 })";
    bool is_object = false;
    const auto parsed = ninfer::text::tool_arguments_object(object, &is_object);
    require(is_object && parsed.begin().key() == "zeta", "object key order changed");
    require(ninfer::text::tool_arguments_object("").empty(), "empty arguments did not become {}");
    GenerationOutcome outcome;
    outcome.finish_reason = ninfer::FinishReason::StopToken;
    outcome.tool_calls.push_back({.name = "lookup", .arguments_json = object});
    AnthropicMessagesStream stream(make_anthropic_response_identity("req_valid", "local"), 0);
    (void)stream.start();
    bool unchanged = false;
    for (const auto& wire : stream.finish(outcome)) {
        const auto event = event_body(wire);
        if (event["type"] == "content_block_delta" && event["delta"]["type"] == "input_json_delta")
            unchanged = event["delta"]["partial_json"] == object;
    }
    require(unchanged, "valid tool argument wire bytes changed");
}

// P21 EUP: replay every upstream rejection vector through the real request/template adapters.
void p21_eup_tool_history(const fi::CompiledChatTemplate& compiled) {
    for (const auto bad : {
             R"({"x": nope})",    // 4461c9683, Qwen/GLM tool_history_arguments: invalid JSON token.
             R"({"x":)",          // 4461c9683, Qwen/GLM tool_history_arguments: truncated JSON.
             "[]",               // 4461c9683, Qwen/GLM tool_history_arguments: non-object array.
             "null",             // 4461c9683, Qwen/GLM tool_history_arguments: non-object null.
             R"({"x":1e99999})",  // 4461c9683, Qwen/GLM tool_history_arguments: number overflow.
         }) {
        auto body = chat_body();
        body["messages"][0]["content"] = "run the tool";
        body["messages"].push_back(Json{
            {"role", "assistant"}, {"content", ""},
            {"tool_calls", Json::array({
                Json{{"id", "first"}, {"type", "function"},
                     {"function", Json{{"name", "noop"}, {"arguments", "{}"}}}},
                Json{{"id", "second"}, {"type", "function"},
                     {"function", Json{{"name", "legacy"}, {"arguments", bad}}}},
            })}});
        body["messages"].push_back(
            Json{{"role", "tool"}, {"tool_call_id", "second"}, {"content", "done"}});
        for (int turn = 0; turn != 2; ++turn) {
            if (turn != 0) {
                body["messages"].push_back(Json{{"role", "assistant"}, {"content", "recovered"}});
                body["messages"].push_back(
                    Json{{"role", "user"}, {"content", "continue with this history"}});
            }
            try {
                const auto request = parse_chat_completion_request(body, {});
                const auto rendered = render(compiled, request.generation);
                require(rendered.text.find("<parameter=input>\n" + std::string(bad) +
                                           "\n</parameter>") != std::string::npos,
                        "raw malformed history was not rendered as an input field");
                auto canonical = body;
                canonical["messages"][1]["tool_calls"][1]["function"]["arguments"] =
                    Json{{"input", bad}}.dump();
                require(rendered.text ==
                            render(compiled, parse_chat_completion_request(canonical, {}).generation).text,
                        "malformed and explicitly wrapped history rendered differently");
                require(turn == 0 ||
                            rendered.text.find("continue with this history") != std::string::npos,
                        "later request carrying malformed history was not rendered");
                require(body["messages"][1]["tool_calls"][1]["function"]["arguments"] == bad,
                        "serving rewrote the client's stored malformed history");
            } catch (const std::exception& error) {
                throw std::runtime_error("P21 EUP vector " + Json(bad).dump() + ", turn " +
                                         std::to_string(turn) + ": " + error.what());
            }
        }
    }
}
// P21 EUP end.

void numerical_error_mapping() {
    const auto error = request_error_to_api_error(ninfer::RequestError(
        ninfer::RequestErrorKind::NumericalFailure, "non-finite sample"));
    require(error.status == 500 && error.type == "server_error" &&
                error.code == "numerical_failure" && error.param.empty(),
            "numerical request failure lost its HTTP classification");
    const auto wire = Json::parse(make_anthropic_error_body(error, "req_numeric"));
    require(wire["error"]["type"] == "api_error", "Anthropic numerical failure was not a server error");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) return 2;
    const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto compiled = fi::CompiledChatTemplate::resolve(source);
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        if (argc == 3 && std::string_view(argv[2]) != name) return;
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    };
    run("malformed_history", [&] { malformed_history(compiled); });
    run("effort_tiers", [&] { effort_tiers(compiled); });
    run("effort_high", [&] {
        auto body = chat_body();
        body["reasoning_effort"] = "high";
        (void)render(compiled, parse_chat_completion_request(body, {}).generation);
    });
    run("disabled_thinking", [&] { disabled_thinking(compiled); });
    run("malformed_response", malformed_response);
    run("unchanged_inputs", [&] { unchanged_inputs(source, compiled); });
    run("numerical_error_mapping", numerical_error_mapping);
    // P21 EUP
    run("p21_eup_tool_history", [&] { p21_eup_tool_history(compiled); });
    return failures ? 1 : 0;
}
