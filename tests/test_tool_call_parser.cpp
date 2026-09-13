#include "serve/tool_call_parser.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

namespace {

using Json = nlohmann::json;

const ninfer::serve::ToolArgumentTypeContracts kNoTypeContracts;

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

int check(bool condition, const std::string& message) { return condition ? 0 : fail(message); }

ninfer::serve::ToolArgumentTypeContracts
contracts_for(const std::string& tool_name, Json properties,
              ninfer::serve::ToolKind kind = ninfer::serve::ToolKind::Function) {
    ninfer::serve::GenerationRequest request;
    ninfer::serve::ToolDefinition tool;
    tool.name            = tool_name;
    tool.parameters_json = Json{{"type", "object"}, {"properties", std::move(properties)}}.dump();
    tool.kind            = kind;
    request.tools.push_back(std::move(tool));
    return ninfer::serve::build_tool_argument_type_contracts(request);
}

int test_single_call() {
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("Calling weather.\n"
                                                   "<tool_call>\n"
                                                   "<function=get_weather>\n"
                                                   "<parameter=city>\nParis\n</parameter>\n"
                                                   "<parameter=days>\n2\n</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, kNoTypeContracts);

    int failures = 0;
    failures += check(parsed.is_tool_call_response, "single call parsed as tool response");
    failures += check(parsed.content == "Calling weather.", "content prefix trimmed");
    failures += check(parsed.tool_calls.size() == 1, "one parsed call");
    failures += check(parsed.tool_calls[0].id.rfind("call_", 0) == 0, "generated call id prefix");
    failures += check(parsed.tool_calls[0].name == "get_weather", "function name parsed");
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("city") == "Paris", "string parameter parsed");
    failures += check(args.at("days") == 2, "number parameter parsed");
    return failures;
}

int test_multiple_calls_and_json_values() {
    const ninfer::serve::ParsedToolCallOutput parsed = ninfer::serve::parse_qwen_tool_call_output(
        "<tool_call>\n"
        "<function=first>\n"
        "<parameter=payload>\n{\"ok\":true,\"items\":[1,2]}\n</parameter>\n"
        "</function>\n"
        "</tool_call>\n"
        "<tool_call>\n"
        "<function=second>\n"
        "<parameter=value>\nplain text\n</parameter>\n"
        "</function>\n"
        "</tool_call>",
        64, kNoTypeContracts);

    int failures = 0;
    failures += check(parsed.is_tool_call_response, "multiple calls parsed as tool response");
    failures += check(parsed.tool_calls.size() == 2, "two parsed calls");
    failures += check(parsed.tool_calls[0].name == "first", "first call name");
    failures += check(parsed.tool_calls[1].name == "second", "second call name");
    const Json first = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(first.at("payload").at("ok") == true, "object parameter bool");
    failures += check(first.at("payload").at("items").at(1) == 2, "object parameter array");
    const Json second = Json::parse(parsed.tool_calls[1].arguments_json);
    failures += check(second.at("value") == "plain text", "plain text parameter string");
    return failures;
}

int test_malformed_falls_back_to_text() {
    const std::string text = "<tool_call>\n<function=get_weather>\n";
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output(text, 64, kNoTypeContracts);
    int failures = 0;
    failures += check(!parsed.is_tool_call_response, "malformed xml is not tool response");
    failures += check(parsed.content == text, "malformed xml preserved as text");
    failures += check(parsed.tool_calls.empty(), "malformed xml has no calls");
    return failures;
}

int test_suffix_after_tool_falls_back_to_text() {
    const std::string text = "<tool_call>\n"
                             "<function=get_weather>\n"
                             "<parameter=city>\nParis\n</parameter>\n"
                             "</function>\n"
                             "</tool_call>\n"
                             "extra answer";
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output(text, 64, kNoTypeContracts);
    int failures = 0;
    failures += check(!parsed.is_tool_call_response, "non-whitespace suffix falls back to text");
    failures += check(parsed.content == text, "suffix fallback preserves text");
    return failures;
}

int test_configured_name_limit() {
    const std::string name(128, 'a');
    const std::string text = "<tool_call>\n<function=" + name + ">\n</function>\n</tool_call>";

    const ninfer::serve::ParsedToolCallOutput anthropic =
        ninfer::serve::parse_qwen_tool_call_output(text, 128, kNoTypeContracts);
    const ninfer::serve::ParsedToolCallOutput openai =
        ninfer::serve::parse_qwen_tool_call_output(text, 64, kNoTypeContracts);
    const std::string too_long_text =
        "<tool_call>\n<function=" + std::string(129, 'a') + ">\n</function>\n</tool_call>";
    const ninfer::serve::ParsedToolCallOutput too_long =
        ninfer::serve::parse_qwen_tool_call_output(too_long_text, 128, kNoTypeContracts);

    int failures = 0;
    failures += check(anthropic.is_tool_call_response && anthropic.tool_calls.size() == 1 &&
                          anthropic.tool_calls[0].name == name,
                      "128-character name accepted with Anthropic limit");
    failures +=
        check(!openai.is_tool_call_response, "128-character name rejected with OpenAI limit");
    failures +=
        check(!too_long.is_tool_call_response, "129-character name rejected with Anthropic limit");
    return failures;
}

int test_declared_strings_are_not_json_sniffed() {
    const auto contracts = contracts_for(
        "TaskUpdate",
        Json{{"taskId", Json{{"type", "string"}}},
             {"content", Json{{"type", "string"}}},
             {"truthy", Json{{"type", "string"}}},
             {"nullish", Json{{"type", "string"}}},
             {"quoted", Json{{"type", "string"}}},
             {"windows", Json{{"type", "string"}}},
             {"string_or_number", Json{{"type", Json::array({"number", "string"})}}}});
    const ninfer::serve::ParsedToolCallOutput parsed = ninfer::serve::parse_qwen_tool_call_output(
        "<tool_call>\n"
        "<function=TaskUpdate>\n"
        "<parameter=taskId>\n1\n</parameter>\n"
        "<parameter=content>\n  {\"x\":1}\n\n</parameter>\n"
        "<parameter=truthy>\ntrue\n</parameter>\n"
        "<parameter=nullish>\nnull\n</parameter>\n"
        "<parameter=quoted>\n\"literal\"\n</parameter>\n"
        "<parameter=windows>\r\n  value  \r\n</parameter>\n"
        "<parameter=string_or_number>\n7\n</parameter>\n"
        "</function>\n"
        "</tool_call>",
        128, contracts);

    int failures = 0;
    failures += check(parsed.is_tool_call_response && parsed.tool_calls.size() == 1,
                      "declared-string tool call was not parsed");
    const Json args = Json::parse(parsed.tool_calls.at(0).arguments_json);
    failures += check(args.at("taskId").is_string() && args.at("taskId") == "1",
                      "numeric-shaped task ID was not preserved as a string");
    failures += check(args.at("content") == "  {\"x\":1}\n",
                      "string content lost meaningful whitespace or was JSON-decoded");
    failures += check(args.at("truthy") == "true" && args.at("nullish") == "null",
                      "boolean/null-shaped strings were promoted");
    failures += check(args.at("quoted") == "\"literal\"",
                      "string payload was reinterpreted as embedded JSON");
    failures += check(args.at("windows") == "  value  ",
                      "CRLF framing or string spaces were not preserved");
    failures += check(args.at("string_or_number") == "7",
                      "string-admitting union destructively promoted raw text");
    return failures;
}

int test_declared_non_string_values_are_json_decoded() {
    const auto contracts = contracts_for(
        "configure", Json{{"count", Json{{"type", "integer"}}},
                          {"total", Json{{"type", "number"}}},
                          {"ratio", Json{{"type", "number"}}},
                          {"enabled", Json{{"type", "boolean"}}},
                          {"payload", Json{{"type", "object"}}},
                          {"items", Json{{"type", "array"}}},
                          {"optional", Json{{"type", Json::array({"integer", "null"})}}},
                          {"flag_or_null", Json{{"type", Json::array({"null", "boolean"})}}}});
    const auto parsed =
        ninfer::serve::parse_qwen_tool_call_output("<tool_call>\n"
                                                   "<function=configure>\n"
                                                   "<parameter=count>\n7\n</parameter>\n"
                                                   "<parameter=total>\n8\n</parameter>\n"
                                                   "<parameter=ratio>\n1.5\n</parameter>\n"
                                                   "<parameter=enabled>\ntrue\n</parameter>\n"
                                                   "<parameter=payload>\n{\"x\":1}\n</parameter>\n"
                                                   "<parameter=items>\n[\"a\",2]\n</parameter>\n"
                                                   "<parameter=optional>\nnull\n</parameter>\n"
                                                   "<parameter=flag_or_null>\nfalse\n</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, contracts);

    int failures    = 0;
    const Json args = Json::parse(parsed.tool_calls.at(0).arguments_json);
    failures += check(args.at("count").is_number_integer() && args.at("count") == 7,
                      "integer parameter was not decoded");
    failures += check(args.at("total").is_number_integer() && args.at("total") == 8,
                      "integer JSON value did not satisfy number schema");
    failures += check(args.at("ratio").is_number_float() && args.at("ratio") == 1.5,
                      "number parameter was not decoded");
    failures += check(args.at("enabled").is_boolean() && args.at("enabled") == true,
                      "boolean parameter was not decoded");
    failures += check(args.at("payload").is_object() && args.at("payload").at("x") == 1,
                      "object parameter was not decoded");
    failures += check(args.at("items").is_array() && args.at("items").at(1) == 2,
                      "array parameter was not decoded");
    failures += check(args.at("optional").is_null(), "declared nullable integer rejected null");
    failures += check(args.at("flag_or_null").is_boolean() && args.at("flag_or_null") == false,
                      "type-array order changed boolean interpretation");
    return failures;
}

std::string parameter_call(std::string_view value) {
    return "prefix  \n<tool_call>\n<function=configure>\n<parameter=value>\n" + std::string(value) +
           "\n</parameter>\n</function>\n</tool_call>";
}

int check_parse_and_stream(const std::string& text,
                           const ninfer::serve::ToolArgumentTypeContracts& contracts,
                           const std::string* expected_arguments,
                           std::string_view expected_content = "prefix") {
    const auto parsed = ninfer::serve::parse_qwen_tool_call_output(text, 64, contracts);
    int failures      = 0;
    if (expected_arguments != nullptr) {
        failures += check(parsed.is_tool_call_response && parsed.tool_calls.size() == 1 &&
                              parsed.tool_calls.front().arguments_json == *expected_arguments &&
                              parsed.content == expected_content,
                          "argument contract mismatch: " + text);
    } else {
        failures += check(!parsed.is_tool_call_response && parsed.tool_calls.empty() &&
                              parsed.content == text,
                          "invalid region did not fall back atomically: " + text);
    }
    ninfer::serve::ToolCallStreamFilter bytewise;
    std::string bytewise_visible;
    for (const char& byte : text) { bytewise_visible += bytewise.feed(std::string_view(&byte, 1)); }
    bytewise_visible += bytewise.finish(parsed.is_tool_call_response);
    failures += check(bytewise_visible == parsed.content, "bytewise stream/terminal text differs");
    // Every two-chunk boundary includes splits inside delimiters, CRLF, and UTF-8.
    for (std::size_t split = 0; split <= text.size(); ++split) {
        ninfer::serve::ToolCallStreamFilter filter;
        std::string visible = filter.feed(std::string_view(text).substr(0, split));
        visible += filter.feed(std::string_view(text).substr(split));
        visible += filter.finish(parsed.is_tool_call_response);
        failures += check(visible == parsed.content,
                          "stream/terminal text differs at split " + std::to_string(split));
    }
    return failures;
}

int test_declared_types_normalize_or_fall_back() {
    int failures       = 0;
    const auto boolean = contracts_for("configure", Json{{"value", Json{{"type", "boolean"}}}});
    const std::string truth = R"({"value":true})";
    failures += check_parse_and_stream(parameter_call("TrUe"), boolean, &truth);
    for (const std::string_view value : {"1", "\"true\"", "null", "yes"}) {
        failures += check_parse_and_stream(parameter_call(value), boolean, nullptr);
    }
    const auto integer = contracts_for("configure", Json{{"value", Json{{"type", "integer"}}}});
    for (const std::string_view value : {"{}", "7.5", "1e-1", "9007199254740992.5"}) {
        failures += check_parse_and_stream(parameter_call(value), integer, nullptr);
    }
    for (const std::string_view value : {"7.0", "100e-2", "-0.0", "9007199254740992.0"}) {
        const std::string expected = "{\"value\":" + std::string(value) + "}";
        failures += check_parse_and_stream(parameter_call(value), integer, &expected);
    }
    const auto number = contracts_for("configure", Json{{"value", Json{{"type", "number"}}}});
    const std::string precise = R"({"value":9007199254740992.5})";
    failures += check_parse_and_stream(parameter_call("9007199254740992.5"), number, &precise);
    return failures;
}

int test_composed_type_contracts() {
    int failures             = 0;
    const auto boolean_union = contracts_for(
        "configure",
        Json{{"value",
              Json{{"anyOf", Json::array({Json{{"oneOf", Json::array({Json{{"type", "boolean"}},
                                                                      Json{{"type", "null"}}})}},
                                          Json{{"type", "integer"}}})}}}});
    const std::string falsity = R"({"value":false})";
    failures += check_parse_and_stream(parameter_call("False"), boolean_union, &falsity);
    failures += check_parse_and_stream(parameter_call("7.5"), boolean_union, nullptr);
    const auto string_union = contracts_for(
        "configure",
        Json{{"value",
              Json{{"oneOf", Json::array({Json{{"type", "string"}}, Json{{"type", "number"}}})}}}});
    const std::string string_value = R"({"value":"7"})";
    failures += check_parse_and_stream(parameter_call("7"), string_union, &string_value);
    const auto unsupported = contracts_for(
        "configure",
        Json{{"value", Json{{"anyOf", Json::array({Json{{"type", "integer"}},
                                                   Json{{"enum", Json::array({1, 2})}}})}}}});
    const std::string legacy = R"({"value":"False"})";
    failures += check_parse_and_stream(parameter_call("False"), unsupported, &legacy);
    return failures;
}

int test_balanced_markup_and_atomic_fallback() {
    const auto strings = contracts_for("configure", Json{{"value", Json{{"type", "string"}}}});
    const std::string markup   = "é \r\n<parameter=inner><parameter=deep>x</parameter></parameter> "
                                 "</function> </tool_call>\n";
    const std::string expected = Json{{"value", markup}}.dump();
    int failures               = check_parse_and_stream(parameter_call(markup), strings, &expected);
    for (const std::string_view value :
         {"echo '<parameter=unterminated>'", "echo '</parameter>'"}) {
        failures += check_parse_and_stream(parameter_call(value), strings, nullptr);
    }
    const std::string duplicate =
        "<tool_call><function=configure><parameter=value>first</parameter>"
        "<parameter=value>second</parameter></function></tool_call>";
    failures += check_parse_and_stream(duplicate, strings, nullptr);
    const auto boolean = contracts_for("configure", Json{{"value", Json{{"type", "boolean"}}}});
    const std::string valid_then_invalid =
        parameter_call("true") + "\n" + parameter_call("yes").substr(9);
    failures += check_parse_and_stream(valid_then_invalid, boolean, nullptr);
    const std::string non_format_suffix = parameter_call("text") + '\v';
    failures += check_parse_and_stream(non_format_suffix, strings, nullptr);
    const std::string prefix   = "prefix\v\f" + parameter_call("text").substr(9);
    const std::string ordinary = R"({"value":"text"})";
    failures += check_parse_and_stream(prefix, strings, &ordinary, "prefix\v\f");
    return failures;
}

int test_deep_supported_union_without_recursive_compilation() {
    ninfer::serve::GenerationRequest request;
    ninfer::serve::ToolDefinition tool;
    tool.name            = "configure";
    tool.parameters_json = R"({"type":"object","properties":{"value":)";
    for (int i = 0; i < 8192; ++i) { tool.parameters_json += R"({"anyOf":[)"; }
    tool.parameters_json += R"({"type":"boolean"})";
    for (int i = 0; i < 8192; ++i) { tool.parameters_json += "]}"; }
    tool.parameters_json += "}}";
    request.tools.push_back(std::move(tool));
    const auto contracts       = ninfer::serve::build_tool_argument_type_contracts(request);
    const std::string expected = R"({"value":false})";
    return check_parse_and_stream(parameter_call("False"), contracts, &expected);
}

int test_large_malformed_region_completes_without_reparse() {
    std::string text = "<tool_call><function=configure><parameter=value>";
    for (int i = 0; i < 22000; ++i) { text += "<parameter="; }
    text += "</parameter>";
    for (int i = 0; i < 1000; ++i) { text += "</tool_call><tool_call>"; }
    const auto parsed = ninfer::serve::parse_qwen_tool_call_output(text, 64, kNoTypeContracts);
    int failures =
        check(!parsed.is_tool_call_response && parsed.tool_calls.empty() && parsed.content == text,
              "large malformed region must finish with exact atomic fallback");
    ninfer::serve::ToolCallStreamFilter filter;
    std::string visible;
    for (const char& byte : text) { visible += filter.feed(std::string_view(&byte, 1)); }
    visible += filter.finish(parsed.is_tool_call_response);
    failures += check(visible == text, "large malformed stream must preserve every byte");
    return failures;
}

int test_unknown_schema_keeps_legacy_inference() {
    const auto contracts = contracts_for(
        "legacy", Json{{"missing_type", Json::object()}, {"invalid_type", Json{{"type", "int"}}}});
    const auto parsed =
        ninfer::serve::parse_qwen_tool_call_output("<tool_call>\n"
                                                   "<function=legacy>\n"
                                                   "<parameter=missing_type>\n7\n</parameter>\n"
                                                   "<parameter=invalid_type>\n8\n</parameter>\n"
                                                   "<parameter=undeclared>\n9\n</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, contracts);

    int failures    = 0;
    const Json args = Json::parse(parsed.tool_calls.at(0).arguments_json);
    failures += check(args.at("missing_type") == 7 && args.at("invalid_type") == 8 &&
                          args.at("undeclared") == 9,
                      "unknown-schema parameter changed legacy inference");
    return failures;
}

int test_custom_tool_raw_input() {
    const auto contracts        = contracts_for("eval", Json{{"input", Json{{"type", "string"}}}},
                                                ninfer::serve::ToolKind::Custom);
    const std::string raw_input = R"INPUT(print('ok')
literal = "</parameter> </function> </tool_call>"
{"looks":"like json"}
)INPUT";
    const std::string text      = "<tool_call>\n<function=eval>\n<parameter=input>\n" + raw_input +
                                  "\n</parameter>\n</function>\n</tool_call>";
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output(text, 64, contracts);

    int failures = 0;
    failures += check(parsed.is_tool_call_response && parsed.tool_calls.size() == 1,
                      "custom tool call parsed");
    failures += check(parsed.tool_calls[0].kind == ninfer::serve::ToolKind::Custom,
                      "requested custom tool kind retained");
    failures += check(parsed.tool_calls[0].arguments_json == raw_input,
                      "custom input retained as one raw string (expected bytes=" +
                          std::to_string(raw_input.size()) + ", observed bytes=" +
                          std::to_string(parsed.tool_calls[0].arguments_json.size()) + ")");

    const std::string malformed = R"TOOL(<tool_call>
<function=eval>
<parameter=code>print('wrong')</parameter>
</function>
</tool_call>)TOOL";
    const ninfer::serve::ParsedToolCallOutput rejected =
        ninfer::serve::parse_qwen_tool_call_output(malformed, 64, contracts);
    failures += check(!rejected.is_tool_call_response && rejected.content == malformed,
                      "custom call without the sole input parameter fails closed");
    return failures;
}

int test_custom_tool_kind_survives_history_only_generation() {
    ninfer::serve::GenerationRequest request;
    request.tool_choice.mode = ninfer::serve::ToolChoiceMode::None;
    ninfer::serve::ChatTurn prior;
    prior.role = ninfer::ChatRole::Assistant;
    ninfer::serve::ToolCall prior_call;
    prior_call.id             = "call_prior";
    prior_call.name           = "eval";
    prior_call.arguments_json = "print('prior')";
    prior_call.kind           = ninfer::serve::ToolKind::Custom;
    prior.tool_calls.push_back(std::move(prior_call));
    request.messages.push_back(std::move(prior));

    const auto contracts        = ninfer::serve::build_tool_argument_type_contracts(request);
    const std::string raw_input = R"INPUT({"a":1}
print('next'))INPUT";
    const std::string text      = "<tool_call>\n<function=eval>\n<parameter=input>\n" + raw_input +
                                  "\n</parameter>\n</function>\n</tool_call>";
    const auto parsed           = ninfer::serve::parse_qwen_tool_call_output(text, 64, contracts);

    int failures = 0;
    failures += check(parsed.is_tool_call_response && parsed.tool_calls.size() == 1,
                      "history-only custom call parsed");
    failures += check(parsed.tool_calls[0].kind == ninfer::serve::ToolKind::Custom,
                      "history-only call silently changed to function kind");
    failures += check(parsed.tool_calls[0].arguments_json == raw_input,
                      "history-only custom input was JSON-coerced or wrapped");

    const std::string unknown =
        "<tool_call>\n<function=other>\n<parameter=input>\nx\n</parameter>\n"
        "</function>\n</tool_call>";
    const auto rejected = ninfer::serve::parse_qwen_tool_call_output(unknown, 64, contracts);
    failures += check(!rejected.is_tool_call_response && rejected.content == unknown,
                      "history-authoritative parser accepted an unknown tool name");
    return failures;
}

int test_incremental_filter_valid_tool() {
    ninfer::serve::ToolCallStreamFilter filter;
    std::string visible;
    visible += filter.feed("Calling weather.  \n<tool_");
    visible += filter.feed("call>\n<function=get_weather>");
    visible += filter.feed("\n</function>\n</tool_call>");
    visible += filter.finish(true);
    int failures = 0;
    failures += check(visible == "Calling weather.",
                      "valid tool filter did not stream the trimmed content prefix");
    failures +=
        check(filter.emitted_bytes() == visible.size(), "valid tool filter byte count mismatch");
    return failures;
}

int test_incremental_filter_fallback() {
    const std::string original = "prefix  \n<tool_call>\n<function=broken>";
    ninfer::serve::ToolCallStreamFilter malformed;
    std::string restored;
    restored += malformed.feed(original.substr(0, 10));
    restored += malformed.feed(original.substr(10));
    restored += malformed.finish(false);

    ninfer::serve::ToolCallStreamFilter normal;
    std::string ordinary;
    ordinary += normal.feed("ordinary text  ");
    ordinary += normal.finish(false);

    const std::string partial_original = "  <tool_x then <tool_";
    ninfer::serve::ToolCallStreamFilter partial;
    std::string partial_restored;
    partial_restored += partial.feed("  <too");
    partial_restored += partial.feed("l_x then <tool_");
    partial_restored += partial.finish(false);

    int failures = 0;
    failures += check(restored == original, "malformed tool filter fallback lost raw bytes");
    failures +=
        check(ordinary == "ordinary text  ", "ordinary filtered output lost trailing whitespace");
    failures += check(partial_restored == partial_original,
                      "partial marker mismatch did not preserve raw bytes");
    return failures;
}

} // namespace

int main() {
    int failures = 0;
    failures += test_single_call();
    failures += test_multiple_calls_and_json_values();
    failures += test_malformed_falls_back_to_text();
    failures += test_suffix_after_tool_falls_back_to_text();
    failures += test_configured_name_limit();
    failures += test_declared_strings_are_not_json_sniffed();
    failures += test_declared_non_string_values_are_json_decoded();
    failures += test_declared_types_normalize_or_fall_back();
    failures += test_composed_type_contracts();
    failures += test_deep_supported_union_without_recursive_compilation();
    failures += test_balanced_markup_and_atomic_fallback();
    failures += test_large_malformed_region_completes_without_reparse();
    failures += test_unknown_schema_keeps_legacy_inference();
    failures += test_custom_tool_raw_input();
    failures += test_custom_tool_kind_survives_history_only_generation();
    failures += test_incremental_filter_valid_tool();
    failures += test_incremental_filter_fallback();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
