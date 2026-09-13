#include "serve/tool_call_parser.h"

#include "serve/opaque_id.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ninfer::serve {
namespace {

using Json         = nlohmann::json;
using Contract     = ToolArgumentTypeContracts;
using DecodePolicy = Contract::DecodePolicy;
using SchemaType   = Contract::SchemaType;
using TypeSet      = Contract::TypeSet;

constexpr std::string_view kToolOpen      = "<tool_call>";
constexpr std::string_view kToolClose     = "</tool_call>";
constexpr std::string_view kFunctionOpen  = "<function=";
constexpr std::string_view kFunctionClose = "</function>";
constexpr std::string_view kParamOpen     = "<parameter=";
constexpr std::string_view kParamClose    = "</parameter>";

struct RawParameter {
    std::string_view name;
    std::string_view value;
};

enum class JsonValueKind : std::uint8_t {
    Null,
    Boolean,
    Integer,
    Number,
    String,
    Object,
    Array,
};

constexpr bool is_format_whitespace(char byte) {
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

constexpr bool is_ascii_digit(char byte) { return byte >= '0' && byte <= '9'; }

constexpr bool is_ascii_alphanumeric(char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || is_ascii_digit(byte);
}

std::string_view trim_format_whitespace(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && is_format_whitespace(text[begin])) { ++begin; }
    std::size_t end = text.size();
    while (end > begin && is_format_whitespace(text[end - 1])) { --end; }
    return text.substr(begin, end - begin);
}

std::string rtrim_format_whitespace(std::string_view text) {
    std::size_t end = text.size();
    while (end != 0 && is_format_whitespace(text[end - 1])) { --end; }
    return std::string(text.substr(0, end));
}

void skip_format_whitespace(std::string_view text, std::size_t& pos) {
    while (pos < text.size() && is_format_whitespace(text[pos])) { ++pos; }
}

bool starts_with_at(std::string_view text, std::size_t pos, std::string_view prefix) {
    return pos <= text.size() && text.substr(pos, prefix.size()) == prefix;
}

bool valid_function_name(std::string_view name, std::size_t max_name_length) {
    if (name.empty() || name.size() > max_name_length) { return false; }
    return std::all_of(name.begin(), name.end(), [](char byte) {
        return is_ascii_alphanumeric(byte) || byte == '_' || byte == '-';
    });
}

std::string new_tool_call_id() { return new_opaque_id("call_"); }

constexpr std::uint8_t type_bit(SchemaType type) { return static_cast<std::uint8_t>(type); }

constexpr bool admits_type(TypeSet types, SchemaType type) {
    return (types.bits & type_bit(type)) != 0;
}

bool schema_type(std::string_view name, SchemaType& type) {
    if (name == "null") {
        type = SchemaType::Null;
    } else if (name == "boolean") {
        type = SchemaType::Boolean;
    } else if (name == "integer") {
        type = SchemaType::Integer;
    } else if (name == "number") {
        type = SchemaType::Number;
    } else if (name == "string") {
        type = SchemaType::String;
    } else if (name == "object") {
        type = SchemaType::Object;
    } else if (name == "array") {
        type = SchemaType::Array;
    } else {
        return false;
    }
    return true;
}

bool compile_direct_types(const Json& type_definition, TypeSet& types) {
    types = {};
    if (type_definition.is_string()) {
        SchemaType type;
        if (!schema_type(type_definition.get_ref<const std::string&>(), type)) { return false; }
        types.bits = type_bit(type);
        return true;
    }
    if (!type_definition.is_array() || type_definition.empty()) { return false; }
    for (const Json& member : type_definition) {
        if (!member.is_string()) { return false; }
        SchemaType type;
        if (!schema_type(member.get_ref<const std::string&>(), type)) { return false; }
        types.bits |= type_bit(type);
    }
    return types.bits != 0;
}

bool compile_schema_types(const Json& schema, TypeSet& types) {
    if (!schema.is_object()) { return false; }
    const auto direct = schema.find("type");
    if (direct != schema.end()) { return compile_direct_types(*direct, types); }

    const auto any_of     = schema.find("anyOf");
    const auto one_of     = schema.find("oneOf");
    const bool has_any_of = any_of != schema.end();
    const bool has_one_of = one_of != schema.end();
    if (has_any_of == has_one_of) { return false; }

    const Json& alternatives = has_any_of ? *any_of : *one_of;
    if (!alternatives.is_array() || alternatives.empty()) { return false; }

    TypeSet combined;
    for (const Json& alternative : alternatives) {
        TypeSet branch;
        if (!compile_schema_types(alternative, branch)) { return false; }
        combined.bits |= branch.bits;
    }
    if (combined.bits == 0) { return false; }
    types = combined;
    return true;
}

ToolArgumentTypeContracts::Tool compile_tool_contract(const ToolDefinition& definition) {
    ToolArgumentTypeContracts::Tool contract;
    contract.name = definition.name;
    contract.kind = definition.kind;

    const Json schema = Json::parse(definition.parameters_json, nullptr, false);
    if (!schema.is_object()) { return contract; }
    const auto properties = schema.find("properties");
    if (properties == schema.end() || !properties->is_object()) { return contract; }

    contract.parameters.reserve(properties->size());
    for (const auto& [name, property] : properties->items()) {
        Contract::Parameter parameter;
        parameter.name = name;
        if (compile_schema_types(property, parameter.types)) {
            parameter.policy = DecodePolicy::DeclaredTypes;
        }
        contract.parameters.push_back(std::move(parameter));
    }
    return contract;
}

bool same_contract(const ToolArgumentTypeContracts::Tool& lhs,
                   const ToolArgumentTypeContracts::Tool& rhs) {
    if (lhs.kind != rhs.kind || lhs.parameters.size() != rhs.parameters.size()) { return false; }
    for (std::size_t i = 0; i < lhs.parameters.size(); ++i) {
        if (lhs.parameters[i].name != rhs.parameters[i].name ||
            lhs.parameters[i].policy != rhs.parameters[i].policy ||
            lhs.parameters[i].types.bits != rhs.parameters[i].types.bits) {
            return false;
        }
    }
    return true;
}

void append_tool_contract(ToolArgumentTypeContracts& contracts, const ToolDefinition& definition) {
    ToolArgumentTypeContracts::Tool compiled = compile_tool_contract(definition);
    const auto existing =
        std::find_if(contracts.tools.begin(), contracts.tools.end(),
                     [&](const auto& tool) { return tool.name == compiled.name; });
    if (existing == contracts.tools.end()) {
        contracts.tools.push_back(std::move(compiled));
        return;
    }
    if (existing->unambiguous && !same_contract(*existing, compiled)) {
        existing->parameters.clear();
        existing->unambiguous = false;
    }
}

void append_history_tool_contract(ToolArgumentTypeContracts& contracts, const ToolCall& call) {
    const auto existing = std::find_if(contracts.tools.begin(), contracts.tools.end(),
                                       [&](const auto& tool) { return tool.name == call.name; });
    if (existing != contracts.tools.end()) {
        if (existing->kind != call.kind) {
            existing->parameters.clear();
            existing->unambiguous = false;
        }
        return;
    }

    ToolArgumentTypeContracts::Tool contract;
    contract.name = call.name;
    contract.kind = call.kind;
    contracts.tools.push_back(std::move(contract));
}

const ToolArgumentTypeContracts::Tool*
find_tool_contract(const ToolArgumentTypeContracts& contracts, std::string_view tool_name) {
    const auto tool =
        std::find_if(contracts.tools.begin(), contracts.tools.end(),
                     [&](const auto& candidate) { return candidate.name == tool_name; });
    return tool == contracts.tools.end() || !tool->unambiguous ? nullptr : &*tool;
}

const ToolArgumentTypeContracts::Parameter*
find_parameter_contract(const ToolArgumentTypeContracts& contracts, std::string_view tool_name,
                        std::string_view parameter_name) {
    const ToolArgumentTypeContracts::Tool* tool = find_tool_contract(contracts, tool_name);
    if (tool == nullptr) { return nullptr; }
    const auto parameter =
        std::find_if(tool->parameters.begin(), tool->parameters.end(),
                     [&](const auto& candidate) { return candidate.name == parameter_name; });
    return parameter == tool->parameters.end() ? nullptr : &*parameter;
}

std::string_view remove_parameter_framing_newlines(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end   = text.size();
    if (text.starts_with("\r\n")) {
        begin = 2;
    } else if (text.starts_with('\n')) {
        begin = 1;
    }
    if (end >= begin + 2 && text.substr(end - 2, 2) == "\r\n") {
        end -= 2;
    } else if (end > begin && text[end - 1] == '\n') {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool ascii_case_equal(std::string_view text, std::string_view lowercase) {
    if (text.size() != lowercase.size()) { return false; }
    for (std::size_t i = 0; i < text.size(); ++i) {
        char byte = text[i];
        if (byte >= 'A' && byte <= 'Z') { byte = static_cast<char>(byte + ('a' - 'A')); }
        if (byte != lowercase[i]) { return false; }
    }
    return true;
}

bool json_number_is_integer(std::string_view number) {
    std::size_t pos = number.starts_with('-') ? 1 : 0;
    if (pos >= number.size()) { return false; }

    const std::size_t integer_begin = pos;
    while (pos < number.size() && is_ascii_digit(number[pos])) { ++pos; }
    const std::size_t integer_end = pos;

    std::size_t fraction_begin = pos;
    std::size_t fraction_end   = pos;
    if (pos < number.size() && number[pos] == '.') {
        fraction_begin = ++pos;
        while (pos < number.size() && is_ascii_digit(number[pos])) { ++pos; }
        fraction_end = pos;
    }

    bool exponent_negative     = false;
    std::size_t exponent_value = 0;
    if (pos < number.size() && (number[pos] == 'e' || number[pos] == 'E')) {
        ++pos;
        if (pos < number.size() && (number[pos] == '+' || number[pos] == '-')) {
            exponent_negative = number[pos] == '-';
            ++pos;
        }
        const std::size_t cap = number.size();
        while (pos < number.size() && is_ascii_digit(number[pos])) {
            const std::size_t digit = static_cast<std::size_t>(number[pos] - '0');
            if (exponent_value != cap) {
                if (exponent_value > cap / 10 || (exponent_value == cap / 10 && digit > cap % 10)) {
                    exponent_value = cap;
                } else {
                    exponent_value = exponent_value * 10 + digit;
                }
            }
            ++pos;
        }
    }
    if (integer_begin == integer_end || pos != number.size()) { return false; }

    bool coefficient_is_zero   = true;
    std::size_t trailing_zeros = 0;
    const auto observe_digit   = [&](char digit) {
        if (digit == '0') {
            ++trailing_zeros;
        } else {
            coefficient_is_zero = false;
            trailing_zeros      = 0;
        }
    };
    for (std::size_t i = integer_begin; i < integer_end; ++i) { observe_digit(number[i]); }
    for (std::size_t i = fraction_begin; i < fraction_end; ++i) { observe_digit(number[i]); }
    if (coefficient_is_zero) { return true; }

    const std::size_t fraction_digits = fraction_end - fraction_begin;
    if (!exponent_negative) {
        if (exponent_value >= fraction_digits) { return true; }
        return fraction_digits - exponent_value <= trailing_zeros;
    }
    if (exponent_value > trailing_zeros) { return false; }
    return fraction_digits <= trailing_zeros - exponent_value;
}

bool classify_json_value(std::string_view value, JsonValueKind& kind) {
    if (value.empty() || !Json::accept(value.begin(), value.end())) { return false; }
    switch (value.front()) {
    case 'n':
        kind = JsonValueKind::Null;
        return true;
    case 't':
    case 'f':
        kind = JsonValueKind::Boolean;
        return true;
    case '"':
        kind = JsonValueKind::String;
        return true;
    case '{':
        kind = JsonValueKind::Object;
        return true;
    case '[':
        kind = JsonValueKind::Array;
        return true;
    default:
        if (value.front() == '-' || is_ascii_digit(value.front())) {
            kind = json_number_is_integer(value) ? JsonValueKind::Integer : JsonValueKind::Number;
            return true;
        }
        return false;
    }
}

bool admits_value(TypeSet types, JsonValueKind kind) {
    switch (kind) {
    case JsonValueKind::Null:
        return admits_type(types, SchemaType::Null);
    case JsonValueKind::Boolean:
        return admits_type(types, SchemaType::Boolean);
    case JsonValueKind::Integer:
        return admits_type(types, SchemaType::Integer) || admits_type(types, SchemaType::Number);
    case JsonValueKind::Number:
        return admits_type(types, SchemaType::Number);
    case JsonValueKind::String:
        return admits_type(types, SchemaType::String);
    case JsonValueKind::Object:
        return admits_type(types, SchemaType::Object);
    case JsonValueKind::Array:
        return admits_type(types, SchemaType::Array);
    }
    return false;
}

std::string encode_json_string(std::string_view value) { return Json(std::string(value)).dump(); }

bool decode_declared_parameter(std::string_view encoded_value, TypeSet types,
                               std::string& json_value) {
    const std::string_view framed = remove_parameter_framing_newlines(encoded_value);
    if (admits_type(types, SchemaType::String)) {
        json_value = encode_json_string(framed);
        return true;
    }

    const std::string_view value = trim_format_whitespace(framed);
    JsonValueKind kind;
    if (classify_json_value(value, kind)) {
        if (!admits_value(types, kind)) { return false; }
        json_value = std::string(value);
        return true;
    }

    if (!admits_type(types, SchemaType::Boolean)) { return false; }
    if (ascii_case_equal(value, "true")) {
        json_value = "true";
        return true;
    }
    if (ascii_case_equal(value, "false")) {
        json_value = "false";
        return true;
    }
    return false;
}

bool decode_parameter(std::string_view encoded_value, const Contract::Parameter* parameter,
                      std::string& json_value) {
    if (parameter != nullptr && parameter->policy == DecodePolicy::DeclaredTypes) {
        return decode_declared_parameter(encoded_value, parameter->types, json_value);
    }

    const std::string_view value = trim_format_whitespace(encoded_value);
    if (Json::accept(value.begin(), value.end())) {
        json_value = std::string(value);
    } else {
        json_value = encode_json_string(value);
    }
    return true;
}

bool find_parameter_open_before(std::string_view text, std::size_t scan, std::size_t limit,
                                std::size_t& open_end) {
    std::size_t candidate = text.find(kParamOpen, scan);
    while (candidate != std::string_view::npos && candidate < limit) {
        const std::size_t name_begin = candidate + kParamOpen.size();
        const std::size_t name_end   = text.find('>', name_begin);
        if (name_end != std::string_view::npos && name_end < limit && name_end != name_begin) {
            open_end = name_end + 1;
            return true;
        }
        candidate = text.find(kParamOpen, candidate + 1);
    }
    return false;
}

bool parse_parameter(std::string_view block, std::size_t& pos,
                     std::vector<RawParameter>& parameters) {
    if (!starts_with_at(block, pos, kParamOpen)) { return false; }
    const std::size_t name_begin = pos + kParamOpen.size();
    const std::size_t name_end   = block.find('>', name_begin);
    if (name_end == std::string_view::npos || name_end == name_begin) { return false; }
    const std::string_view name = block.substr(name_begin, name_end - name_begin);
    if (std::any_of(parameters.begin(), parameters.end(),
                    [&](const RawParameter& parameter) { return parameter.name == name; })) {
        return false;
    }
    const std::size_t value_begin = name_end + 1;
    std::size_t depth             = 1;
    std::size_t scan              = value_begin;
    for (;;) {
        const std::size_t close = block.find(kParamClose, scan);
        if (close == std::string_view::npos) { return false; }
        std::size_t nested_open_end = 0;
        if (find_parameter_open_before(block, scan, close, nested_open_end)) {
            ++depth;
            scan = nested_open_end;
            continue;
        }
        if (--depth == 0) {
            parameters.push_back({name, block.substr(value_begin, close - value_begin)});
            pos = close + kParamClose.size();
            return true;
        }
        scan = close + kParamClose.size();
    }
}

bool parse_one_tool_call(std::string_view block, std::size_t max_name_length,
                         const ToolArgumentTypeContracts& contracts, ToolCall& out) {
    std::size_t pos = 0;
    skip_format_whitespace(block, pos);
    if (!starts_with_at(block, pos, kFunctionOpen)) { return false; }
    const std::size_t name_begin = pos + kFunctionOpen.size();
    const std::size_t name_end   = block.find('>', name_begin);
    if (name_end == std::string_view::npos) { return false; }
    const std::string_view name = block.substr(name_begin, name_end - name_begin);
    if (!valid_function_name(name, max_name_length)) { return false; }
    pos = name_end + 1;

    const Contract::Tool* contract = find_tool_contract(contracts, name);
    if (contract == nullptr && contracts.names_authoritative) { return false; }
    const bool custom = contract != nullptr && contract->kind == ToolKind::Custom;
    std::string arguments;
    if (custom) {
        // Custom tools carry raw input, not the balanced function-argument grammar.
        // Preserve the existing outermost framing even when input contains closing tags.
        const std::size_t function_end = block.rfind(kFunctionClose);
        if (function_end == std::string_view::npos || function_end < pos) { return false; }
        const std::string_view params          = block.substr(pos, function_end - pos);
        constexpr std::string_view kCustomOpen = "<parameter=input>";
        std::size_t param_pos                  = 0;
        skip_format_whitespace(params, param_pos);
        if (!starts_with_at(params, param_pos, kCustomOpen)) { return false; }
        const std::size_t value_begin = param_pos + kCustomOpen.size();
        const std::size_t value_end   = params.rfind(kParamClose);
        if (value_end == std::string_view::npos || value_end < value_begin) { return false; }
        param_pos = value_end + kParamClose.size();
        skip_format_whitespace(params, param_pos);
        if (param_pos != params.size()) { return false; }
        arguments =
            remove_parameter_framing_newlines(params.substr(value_begin, value_end - value_begin));
        pos = function_end + kFunctionClose.size();
    } else {
        std::vector<RawParameter> parameters;
        for (;;) {
            skip_format_whitespace(block, pos);
            if (starts_with_at(block, pos, kFunctionClose)) {
                pos += kFunctionClose.size();
                break;
            }
            if (!parse_parameter(block, pos, parameters)) { return false; }
        }
        arguments = "{";
        for (const RawParameter& parameter : parameters) {
            std::string value;
            if (!decode_parameter(parameter.value,
                                  find_parameter_contract(contracts, name, parameter.name),
                                  value)) {
                return false;
            }
            if (arguments.size() != 1) { arguments.push_back(','); }
            arguments += encode_json_string(parameter.name);
            arguments.push_back(':');
            arguments += value;
        }
        arguments.push_back('}');
    }
    skip_format_whitespace(block, pos);
    if (pos != block.size()) { return false; }
    out.id             = new_tool_call_id();
    out.name           = name;
    out.arguments_json = std::move(arguments);
    out.kind           = custom ? ToolKind::Custom : ToolKind::Function;
    return true;
}

ParsedToolCallOutput fallback(const std::string& text) {
    ParsedToolCallOutput out;
    out.content = text;
    return out;
}

} // namespace

ToolArgumentTypeContracts build_tool_argument_type_contracts(const GenerationRequest& request) {
    ToolArgumentTypeContracts contracts;
    if (request.uses_tools() && request.tool_choice.mode == ToolChoiceMode::Named) {
        const auto selected =
            std::find_if(request.tools.begin(), request.tools.end(),
                         [&](const auto& tool) { return tool.name == request.tool_choice.name; });
        if (selected != request.tools.end()) { append_tool_contract(contracts, *selected); }
    } else if (request.uses_tools()) {
        contracts.tools.reserve(request.tools.size());
        for (const ToolDefinition& tool : request.tools) { append_tool_contract(contracts, tool); }
    }
    for (const ChatTurn& message : request.messages) {
        for (const ToolCall& call : message.tool_calls) {
            append_history_tool_contract(contracts, call);
        }
    }
    contracts.names_authoritative = !contracts.tools.empty();
    return contracts;
}

ParsedToolCallOutput parse_qwen_tool_call_output(const std::string& text,
                                                 std::size_t max_tool_name_length,
                                                 const ToolArgumentTypeContracts& contracts) {
    const std::size_t first = text.find(kToolOpen);
    if (first == std::string::npos) { return fallback(text); }

    ParsedToolCallOutput out;
    out.content = rtrim_format_whitespace(std::string_view(text).substr(0, first));

    std::size_t pos = first;
    while (pos < text.size()) {
        skip_format_whitespace(text, pos);
        if (pos >= text.size()) { break; }
        if (!starts_with_at(text, pos, kToolOpen)) { return fallback(text); }
        const std::size_t inner_begin = pos + kToolOpen.size();
        std::size_t close             = text.find(kToolClose, inner_begin);
        ToolCall call;
        bool parsed = false;
        while (close != std::string::npos) {
            ToolCall candidate;
            std::size_t after = close + kToolClose.size();
            skip_format_whitespace(text, after);
            const bool valid_boundary =
                after == text.size() || starts_with_at(text, after, kToolOpen);
            if (valid_boundary &&
                parse_one_tool_call(std::string_view(text).substr(inner_begin, close - inner_begin),
                                    max_tool_name_length, contracts, candidate)) {
                call   = std::move(candidate);
                parsed = true;
                break;
            }
            close = text.find(kToolClose, close + kToolClose.size());
        }
        if (!parsed) { return fallback(text); }
        out.tool_calls.push_back(std::move(call));
        pos = close + kToolClose.size();
    }

    if (out.tool_calls.empty()) { return fallback(text); }
    out.is_tool_call_response = true;
    return out;
}

std::string ToolCallStreamFilter::feed(std::string_view text) {
    if (finished_) { throw std::logic_error("tool-call stream filter is already finished"); }
    if (text.empty()) { return {}; }
    if (saw_tool_marker_) {
        tool_region_.append(text);
        return {};
    }

    std::string visible;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char byte = text[index];
        if (marker_prefix_bytes_ != 0) {
            if (byte == kToolOpen[marker_prefix_bytes_]) {
                ++marker_prefix_bytes_;
                if (marker_prefix_bytes_ == kToolOpen.size()) {
                    tool_region_ = std::move(trailing_whitespace_);
                    trailing_whitespace_.clear();
                    tool_region_.append(kToolOpen);
                    tool_region_.append(text.substr(index + 1));
                    marker_prefix_bytes_ = 0;
                    saw_tool_marker_     = true;
                    break;
                }
                continue;
            }
            visible.append(trailing_whitespace_);
            trailing_whitespace_.clear();
            visible.append(kToolOpen.substr(0, marker_prefix_bytes_));
            marker_prefix_bytes_ = 0;
        }

        if (byte == kToolOpen.front()) {
            marker_prefix_bytes_ = 1;
        } else if (is_format_whitespace(byte)) {
            trailing_whitespace_.push_back(byte);
        } else {
            visible.append(trailing_whitespace_);
            trailing_whitespace_.clear();
            visible.push_back(byte);
        }
    }
    emitted_bytes_ += visible.size();
    return visible;
}

std::string ToolCallStreamFilter::finish(bool is_tool_call_response) {
    if (finished_) { throw std::logic_error("tool-call stream filter is already finished"); }
    finished_ = true;
    if (is_tool_call_response) {
        trailing_whitespace_.clear();
        tool_region_.clear();
        marker_prefix_bytes_ = 0;
        return {};
    }
    std::string tail = std::move(trailing_whitespace_);
    tail.append(kToolOpen.substr(0, marker_prefix_bytes_));
    marker_prefix_bytes_ = 0;
    tail += tool_region_;
    tool_region_.clear();
    emitted_bytes_ += tail.size();
    return tail;
}

} // namespace ninfer::serve
