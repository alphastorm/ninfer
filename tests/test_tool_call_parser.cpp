#include "serve/tool_call_parser.h"
#include "serve/request.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;

const ninfer::serve::ToolArgumentTypeContracts kNoTypeContracts;

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

int check(bool condition, const std::string& message) { return condition ? 0 : fail(message); }

ninfer::serve::ToolArgumentTypeContracts contracts_for(const std::string& tool_name,
                                                       Json properties) {
    ninfer::serve::GenerationRequest request;
    ninfer::serve::ToolDefinition tool;
    tool.name            = tool_name;
    tool.parameters_json = Json{{"type", "object"}, {"properties", std::move(properties)}}.dump();
    request.tools.push_back(std::move(tool));
    return ninfer::serve::build_tool_argument_type_contracts(request);
}

int test_single_call() {
    // build_tool_param_type_map records only non-string types; city (string)
    // and any unknown param are absent, so the parser preserves raw text.
    ninfer::serve::ToolParamTypeMap map;
    map["get_weather"]["days"] = {"integer"};

    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("Calling weather.\n"
                                                   "   <tool_call>\n"
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
    failures += check(args.at("days") == 2, "integer-typed days deserialized to number");
    return failures;
}

int test_multiple_calls_and_json_values() {
    // payload is object => recorded; value is string => absent.
    ninfer::serve::ToolParamTypeMap map;
    map["first"]["payload"] = {"object"};

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

int test_string_param_keeps_numeric_looking_value() {
    // priority is integer => recorded; taskId and status are string =>
    // absent (exactly what build_tool_param_type_map produces). The tool
    // is known, yet its string-typed params still preserve raw text.
    ninfer::serve::ToolParamTypeMap map;
    map["TaskUpdate"]["priority"] = {"integer"};

    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output(text, 64, kNoTypeContracts);
    int failures = 0;
    failures += check(!parsed.is_tool_call_response, "malformed xml is not tool response");
    failures += check(parsed.content == text, "malformed xml preserved as text");
    failures += check(parsed.tool_calls.empty(), "malformed xml has no calls");
    return failures;
}

int test_suffix_after_tool_falls_back_to_text() {
    const std::string text = "   <tool_call>\n"
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
    const std::string text = "   <tool_call>\n<function=" + name + ">\n</function>\n</tool_call>";

    const ninfer::serve::ParsedToolCallOutput anthropic =
        ninfer::serve::parse_qwen_tool_call_output(text, 128, kNoTypeContracts);
    const ninfer::serve::ParsedToolCallOutput openai =
        ninfer::serve::parse_qwen_tool_call_output(text, 64, kNoTypeContracts);
    const std::string too_long_text =
        "   <tool_call>\n<function=" + std::string(129, 'a') + ">\n</function>\n</tool_call>";
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

int test_declared_type_mismatches_are_forwarded_without_coercion() {
    const auto contracts =
        contracts_for("configure", Json{{"object_as_integer", Json{{"type", "integer"}}},
                                        {"one_as_boolean", Json{{"type", "boolean"}}},
                                        {"string_as_boolean", Json{{"type", "boolean"}}},
                                        {"python_boolean", Json{{"type", "boolean"}}},
                                        {"null_as_boolean", Json{{"type", "boolean"}}}});

    const auto parsed = ninfer::serve::parse_qwen_tool_call_output(
        "<tool_call>\n"
        "<function=configure>\n"
        "<parameter=object_as_integer>\n{}\n</parameter>\n"
        "<parameter=one_as_boolean>\n1\n</parameter>\n"
        "<parameter=string_as_boolean>\n\"true\"\n</parameter>\n"
        "<parameter=null_as_boolean>\nnull\n</parameter>\n"
        "</function>\n"
        "</tool_call>",
        64, contracts);

    int failures = 0;
    failures += check(parsed.is_tool_call_response && parsed.tool_calls.size() == 1,
                      "valid JSON was rejected because it did not match the declared type");
    const Json args = Json::parse(parsed.tool_calls.at(0).arguments_json);
    failures += check(args.at("object_as_integer").is_object(),
                      "object-shaped JSON was coerced to the declared integer type");
    failures += check(args.at("one_as_boolean").is_number_integer(),
                      "numeric JSON was coerced to the declared boolean type");
    failures += check(args.at("string_as_boolean").is_string(),
                      "string JSON was coerced to the declared boolean type");
    failures += check(args.at("null_as_boolean").is_null(),
                      "null JSON was coerced to the declared boolean type");

    const std::string invalid =
        "<tool_call>\n<function=configure>\n<parameter=python_boolean>\nTrue\n</parameter>\n"
        "</function>\n</tool_call>";
    const auto rejected = ninfer::serve::parse_qwen_tool_call_output(invalid, 64, contracts);
    failures += check(!rejected.is_tool_call_response && rejected.content == invalid &&
                          rejected.tool_calls.empty(),
                      "non-JSON value for a declared non-string parameter did not fall back");
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

// Schema-driven coverage: construct real ToolDefinition schemas and exercise
// build_tool_param_type_map end-to-end instead of hand-fabricating the map.
int test_schema_driven_type_map() {
    // taskId is string; days is integer; count is nullable integer; note has a
    // misspelled "strnig" type; flag is boolean; payload is object.
    const ninfer::serve::ToolDefinition tool =
        make_tool("TaskUpdate", R"({"type":"object","properties":{)"
                                R"("taskId":{"type":"string"},)"
                                R"("days":{"type":"integer"},)"
                                R"("count":{"type":["integer","null"]},)"
                                R"("note":{"type":"strnig"},)"
                                R"("flag":{"type":"boolean"},)"
                                R"("payload":{"type":"object"})"
                                R"(}})");
    const ninfer::serve::ToolParamTypeMap map = ninfer::serve::build_tool_param_type_map({tool});

    int failures = 0;
    failures += check(map.count("TaskUpdate") == 1, "tool recorded");
    const auto& inner = map.at("TaskUpdate");
    failures += check(inner.count("taskId") == 0, "string-typed taskId not recorded");
    failures += check(inner.count("days") == 1, "integer-typed days recorded");
    failures += check(inner.count("count") == 1, "nullable integer count recorded");
    failures += check(inner.count("note") == 0, "misspelled strnig type not recorded");
    failures += check(inner.count("flag") == 1, "boolean-typed flag recorded");
    failures += check(inner.count("payload") == 1, "object-typed payload recorded");
    return failures;
}

// (a) numeric-looking string param (taskId=1 -> "1" string).
int test_schema_string_param_keeps_numeric_looking_value() {
    const ninfer::serve::ToolDefinition tool =
        make_tool("TaskUpdate", R"({"type":"object","properties":{"taskId":{"type":"string"}}})");
    const ninfer::serve::ToolParamTypeMap map = ninfer::serve::build_tool_param_type_map({tool});

    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("   <tool_call>\n"
                                                   "<function=TaskUpdate>\n"
                                                   "<parameter=taskId>1</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, map);

    int failures = 0;
    failures += check(parsed.is_tool_call_response, "schema string call parsed as tool response");
    failures += check(parsed.tool_calls.size() == 1, "one schema string call");
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("taskId").is_string(), "schema taskId is a string, not a number");
    failures += check(args.at("taskId") == "1", "schema string taskId keeps numeric-looking value");
    return failures;
}

// (b) genuine integer (days=2 -> 2 number).
int test_schema_integer_param_deserializes() {
    const ninfer::serve::ToolDefinition tool =
        make_tool("get_weather", R"({"type":"object","properties":{"days":{"type":"integer"}}})");
    const ninfer::serve::ToolParamTypeMap map = ninfer::serve::build_tool_param_type_map({tool});

    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("   <tool_call>\n"
                                                   "<function=get_weather>\n"
                                                   "<parameter=days>2</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, map);

    int failures = 0;
    failures += check(parsed.is_tool_call_response, "schema integer call parsed");
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("days").is_number(), "schema integer days is a number");
    failures += check(args.at("days") == 2, "schema integer days deserialized to number 2");
    return failures;
}

// (c) valid nullable integer (["integer","null"] count=7 -> 7 number).
int test_schema_nullable_integer_deserializes() {
    const ninfer::serve::ToolDefinition tool = make_tool(
        "get_items", R"({"type":"object","properties":{"count":{"type":["integer","null"]}}})");
    const ninfer::serve::ToolParamTypeMap map = ninfer::serve::build_tool_param_type_map({tool});

    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("   <tool_call>\n"
                                                   "<function=get_items>\n"
                                                   "<parameter=count>7</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, map);

    int failures = 0;
    failures += check(parsed.is_tool_call_response, "nullable integer call parsed");
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("count").is_number(), "nullable count deserialized to number");
    failures += check(args.at("count") == 7, "nullable count value 7 preserved as number");
    return failures;
}

// ["string","null"] => string allowed => not recorded; 5 -> "5".
int test_schema_nullable_string_preserves_raw() {
    const ninfer::serve::ToolDefinition tool = make_tool(
        "get_opt", R"({"type":"object","properties":{"opt":{"type":["string","null"]}}})");
    const ninfer::serve::ToolParamTypeMap map = ninfer::serve::build_tool_param_type_map({tool});

    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("   <tool_call>\n"
                                                   "<function=get_opt>\n"
                                                   "<parameter=opt>5</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, map);

    int failures = 0;
    failures += check(parsed.is_tool_call_response, "nullable string call parsed");
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("opt").is_string(), "nullable string opt stays a string");
    failures += check(args.at("opt") == "5", "nullable string opt value preserved as text");
    return failures;
}

// ["integer","string"] => string allowed => not recorded; 9 -> "9".
int test_schema_mixed_integer_string_preserves_raw() {
    const ninfer::serve::ToolDefinition tool =
        make_tool("mix", R"({"type":"object","properties":{"v":{"type":["integer","string"]}}})");
    const ninfer::serve::ToolParamTypeMap map = ninfer::serve::build_tool_param_type_map({tool});

    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("   <tool_call>\n"
                                                   "<function=mix>\n"
                                                   "<parameter=v>9</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, map);

    int failures    = 0;
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("v").is_string(), "mixed integer/string v stays a string");
    failures += check(args.at("v") == "9", "mixed integer/string v value preserved as text");
    return failures;
}

// (d) invalid type spelling ("strnig" -> raw text).
int test_schema_invalid_type_spelling_preserves_raw() {
    const ninfer::serve::ToolDefinition tool =
        make_tool("bad", R"({"type":"object","properties":{"note":{"type":"strnig"}}})");
    const ninfer::serve::ToolParamTypeMap map = ninfer::serve::build_tool_param_type_map({tool});

    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("   <tool_call>\n"
                                                   "<function=bad>\n"
                                                   "<parameter=note>hi</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, map);

    int failures    = 0;
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("note").is_string(), "invalid-type note stays a string");
    failures += check(args.at("note") == "hi", "invalid-type note value preserved as text");
    return failures;
}

// (d) boolean param: Python-style scalars coerce to JSON booleans
// (vLLM qwen3coder coercion); non-boolean text stays raw.
int test_schema_boolean_param_coerces_python_scalars() {
    const ninfer::serve::ToolDefinition tool = make_tool(
        "set_flags", R"({"type":"object","properties":{)"
                     R"("a":{"type":"boolean"},"b":{"type":"boolean"},"c":{"type":"boolean"},)"
                     R"("d":{"type":"boolean"},"e":{"type":"boolean"},"f":{"type":"boolean"},)"
                     R"("g":{"type":"boolean"}}})");
    const ninfer::serve::ToolParamTypeMap map = ninfer::serve::build_tool_param_type_map({tool});

    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("    <tool_call>\n"
                                                   "<function=set_flags>\n"
                                                   "<parameter=a>True</parameter>\n"
                                                   "<parameter=b>1</parameter>\n"
                                                   "<parameter=c>False</parameter>\n"
                                                   "<parameter=d>0</parameter>\n"
                                                   "<parameter=e>maybe</parameter>\n"
                                                   "<parameter=f>true</parameter>\n"
                                                   "<parameter=g> TRUE </parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, map);

    int failures = 0;
    failures += check(parsed.is_tool_call_response, "schema boolean call parsed as tool response");
    failures += check(parsed.tool_calls.size() == 1, "one schema boolean call");
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("a").is_boolean() && args.at("a") == true,
                      "boolean param True coerces to true");
    failures +=
        check(args.at("b").is_boolean() && args.at("b") == true, "boolean param 1 coerces to true");
    failures += check(args.at("c").is_boolean() && args.at("c") == false,
                      "boolean param False coerces to false");
    failures += check(args.at("d").is_boolean() && args.at("d") == false,
                      "boolean param 0 coerces to false");
    failures += check(args.at("e").is_string() && args.at("e") == "maybe",
                      "non-boolean text for a boolean param stays raw");
    failures += check(args.at("f").is_boolean() && args.at("f") == true,
                      "JSON true for a boolean param still coerces");
    failures += check(args.at("g").is_boolean() && args.at("g") == true,
                      "padded all-caps TRUE coerces to true");
    return failures;
}

// (g) nullable boolean: Python scalars coerce, the literal null is JSON
// null, and the result does not depend on the type-array order.
int test_schema_nullable_boolean_param() {
    const ninfer::serve::ToolDefinition tool = make_tool(
        "flags",
        R"({"type":"object","properties":{)"
        R"("a":{"type":["boolean","null"]},"b":{"type":["null","boolean"]},)"
        R"("c":{"type":"boolean"},"d":{"type":["boolean","null"]},"e":{"type":["null","boolean"]},"f":{"type":["boolean","null"]}}})");
    const ninfer::serve::ToolParamTypeMap map = ninfer::serve::build_tool_param_type_map({tool});

    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("    <tool_call>\n"
                                                   "<function=flags>\n"
                                                   "<parameter=a>True</parameter>\n"
                                                   "<parameter=b>null</parameter>\n"
                                                   "<parameter=c>null</parameter>\n"
                                                   "<parameter=d>maybe</parameter>\n"
                                                   "<parameter=e>False</parameter>\n"
                                                   "<parameter=f>Null</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, map);

    int failures = 0;
    failures +=
        check(parsed.is_tool_call_response, "nullable boolean call parsed as tool response");
    failures += check(parsed.tool_calls.size() == 1, "one nullable boolean call");
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("a").is_boolean() && args.at("a") == true,
                      "nullable boolean True coerces to true");
    failures +=
        check(args.at("b").is_null(), "nullable boolean null is JSON null (null listed first)");
    failures += check(args.at("c").is_null(), "plain boolean null is JSON null");
    failures += check(args.at("d").is_string() && args.at("d") == "maybe",
                      "non-boolean text for a nullable boolean stays raw");
    failures += check(args.at("e").is_boolean() && args.at("e") == false,
                      "nullable boolean False coerces to false (null listed first)");
    failures += check(args.at("f").is_null(), "capitalized Null is JSON null (case-insensitive)");
    return failures;
}

// (e) boolean true for a string param -> raw text "true".
// (f) null for a string param -> raw text "null".
int test_schema_string_param_bool_and_null_preserve_raw() {
    const ninfer::serve::ToolDefinition tool =
        make_tool("flaggy", R"({"type":"object","properties":{"s":{"type":"string"}}})");
    const ninfer::serve::ToolParamTypeMap map = ninfer::serve::build_tool_param_type_map({tool});

    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("   <tool_call>\n"
                                                   "<function=flaggy>\n"
                                                   "<parameter=s>true</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>\n"
                                                   "<tool_call>\n"
                                                   "<function=flaggy>\n"
                                                   "<parameter=s>null</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, map);

    int failures = 0;
    failures += check(parsed.tool_calls.size() == 2, "two string-param calls parsed");
    const Json a1 = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(a1.at("s").is_string(), "string param bool stays a string");
    failures += check(a1.at("s") == "true", "string param bool value preserved as text");
    const Json a2 = Json::parse(parsed.tool_calls[1].arguments_json);
    failures += check(a2.at("s").is_string(), "string param null stays a string");
    failures += check(a2.at("s") == "null", "string param null value preserved as text");
    return failures;
}

// (g) object-looking text for a string param -> raw text.
int test_schema_string_param_object_text_preserves_raw() {
    const ninfer::serve::ToolDefinition tool =
        make_tool("obj", R"({"type":"object","properties":{"s":{"type":"string"}}})");
    const ninfer::serve::ToolParamTypeMap map = ninfer::serve::build_tool_param_type_map({tool});

    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("   <tool_call>\n"
                                                   "<function=obj>\n"
                                                   "<parameter=s>{\"k\":1}</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, map);

    int failures    = 0;
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("s").is_string(), "string param object text stays a string");
    failures += check(args.at("s") == "{\"k\":1}", "string param object text preserved verbatim");
    return failures;
}

// (h) empty type array ("type":[] -> raw text, no crash).
int test_schema_empty_type_array_preserves_raw() {
    const ninfer::serve::ToolDefinition tool =
        make_tool("emptytype", R"({"type":"object","properties":{"n":{"type":[]}}})");
    const ninfer::serve::ToolParamTypeMap map = ninfer::serve::build_tool_param_type_map({tool});

    int failures = 0;
    failures += check(map.at("emptytype").count("n") == 0, "empty type array leaves n unrecorded");
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("    <tool_call>\n"
                                                   "<function=emptytype>\n"
                                                   "<parameter=n>3</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, map);
    failures += check(parsed.is_tool_call_response, "empty-array call parsed");
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("n").is_string(), "empty type array n stays a string");
    failures += check(args.at("n") == "3", "empty type array n value preserved as text");
    return failures;
}

// (i) non-string non-array "type" (e.g. "type":5) preserves raw text.
int test_schema_non_string_non_array_type_preserves_raw() {
    const ninfer::serve::ToolDefinition tool =
        make_tool("numtype", R"({"type":"object","properties":{"n":{"type":5}}})");
    const ninfer::serve::ToolParamTypeMap map = ninfer::serve::build_tool_param_type_map({tool});

    int failures = 0;
    failures +=
        check(map.at("numtype").count("n") == 0, "non-string non-array type leaves n unrecorded");
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("    <tool_call>\n"
                                                   "<function=numtype>\n"
                                                   "<parameter=n>3</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, map);
    failures += check(parsed.is_tool_call_response, "non-string-type call parsed");
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("n").is_string(), "non-string-type n stays a string");
    failures += check(args.at("n") == "3", "non-string-type n value preserved as text");
    return failures;
}

// A second same-name definition whose object schema has no "properties"
// must replace the first definition's recorded (integer) permissions,
// leaving the param unrecorded (raw text) instead of leaking the first.
int test_duplicate_tool_definition_no_properties_replaces() {
    const ninfer::serve::ToolDefinition first =
        make_tool("dup2", R"({"type":"object","properties":{"count":{"type":"integer"}}})");
    const ninfer::serve::ToolDefinition second = make_tool("dup2", R"({"type":"object"})");
    const ninfer::serve::ToolParamTypeMap map =
        ninfer::serve::build_tool_param_type_map({first, second});

    int failures = 0;
    failures += check(map.count("dup2") == 1, "no-properties duplicate has one entry");
    failures += check(map.at("dup2").count("count") == 0,
                      "second no-properties definition replaced the first (count not recorded)");
    const ninfer::serve::ParsedToolCallOutput parsed =
        ninfer::serve::parse_qwen_tool_call_output("    <tool_call>\n"
                                                   "<function=dup2>\n"
                                                   "<parameter=count>3</parameter>\n"
                                                   "</function>\n"
                                                   "</tool_call>",
                                                   64, map);
    failures += check(parsed.is_tool_call_response, "no-properties dup call parsed");
    const Json args = Json::parse(parsed.tool_calls[0].arguments_json);
    failures += check(args.at("count").is_string(), "no-properties dup count stays a string");
    failures += check(args.at("count") == "3", "no-properties dup count value preserved as text");
    return failures;
}

// A redefinition of the same tool name must replace the prior entry so a
// second (string) definition cannot leak the first's integer permission.
int test_duplicate_tool_definition_replaced() {
    const ninfer::serve::ToolDefinition first =
        make_tool("dup", R"({"type":"object","properties":{"count":{"type":"integer"}}})");
    const ninfer::serve::ToolDefinition second =
        make_tool("dup", R"({"type":"object","properties":{"count":{"type":"string"}}})");
    const ninfer::serve::ToolParamTypeMap map =
        ninfer::serve::build_tool_param_type_map({first, second});

    int failures = 0;
    failures += check(map.count("dup") == 1, "duplicate tool name has one entry");
    failures += check(map.at("dup").count("count") == 0,
                      "second (string) definition replaced the first (integer) entry");
    return failures;
}

} // namespace

int main() {
    int failures = 0;
    failures += test_single_call();
    failures += test_multiple_calls_and_json_values();
    failures += test_string_param_keeps_numeric_looking_value();
    failures += test_unknown_param_defaults_to_string();
    failures += test_malformed_falls_back_to_text();
    failures += test_suffix_after_tool_falls_back_to_text();
    failures += test_configured_name_limit();
    failures += test_declared_strings_are_not_json_sniffed();
    failures += test_declared_non_string_values_are_json_decoded();
    failures += test_declared_type_mismatches_are_forwarded_without_coercion();
    failures += test_unknown_schema_keeps_legacy_inference();
    failures += test_incremental_filter_valid_tool();
    failures += test_incremental_filter_fallback();
    failures += test_schema_driven_type_map();
    failures += test_schema_string_param_keeps_numeric_looking_value();
    failures += test_schema_integer_param_deserializes();
    failures += test_schema_nullable_integer_deserializes();
    failures += test_schema_nullable_string_preserves_raw();
    failures += test_schema_boolean_param_coerces_python_scalars();
    failures += test_schema_nullable_boolean_param();
    failures += test_schema_mixed_integer_string_preserves_raw();
    failures += test_schema_invalid_type_spelling_preserves_raw();
    failures += test_schema_string_param_bool_and_null_preserve_raw();
    failures += test_schema_string_param_object_text_preserves_raw();
    failures += test_schema_empty_type_array_preserves_raw();
    failures += test_schema_non_string_non_array_type_preserves_raw();
    failures += test_duplicate_tool_definition_no_properties_replaces();
    failures += test_duplicate_tool_definition_replaced();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
