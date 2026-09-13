#include "serve/tool_call_parser.h"

#include "serve/opaque_id.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_set>
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
    types               = {};
    const Json* current = &schema;
    std::vector<const Json*> pending;
    for (;;) {
        if (!current->is_object()) { return false; }
        const auto direct = current->find("type");
        if (direct != current->end()) {
            TypeSet branch;
            if (!compile_direct_types(*direct, branch)) { return false; }
            types.bits |= branch.bits;
        } else {
            const auto any_of     = current->find("anyOf");
            const auto one_of     = current->find("oneOf");
            const bool has_any_of = any_of != current->end();
            const bool has_one_of = one_of != current->end();
            if (has_any_of == has_one_of) { return false; }
            const Json& alternatives = has_any_of ? *any_of : *one_of;
            if (!alternatives.is_array() || alternatives.empty()) { return false; }
            for (std::size_t i = 1; i < alternatives.size(); ++i) {
                pending.push_back(&alternatives[i]);
            }
            current = &alternatives.front();
            continue;
        }
        if (pending.empty()) { return types.bits != 0; }
        current = pending.back();
        pending.pop_back();
    }
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
    text                  = text.substr(0, limit);
    std::size_t candidate = text.find(kParamOpen, scan);
    while (candidate != std::string_view::npos) {
        const std::size_t name_begin = candidate + kParamOpen.size();
        const std::size_t name_end   = text.find('>', name_begin);
        // No later candidate can end before this close either. Do not rescan the suffix.
        if (name_end == std::string_view::npos) { return false; }
        if (name_end != name_begin) {
            open_end = name_end + 1;
            return true;
        }
        candidate = text.find(kParamOpen, name_end + 1);
    }
    return false;
}

bool parse_parameter(std::string_view block, std::size_t& pos, RawParameter& parameter) {
    if (!starts_with_at(block, pos, kParamOpen)) { return false; }
    const std::size_t name_begin = pos + kParamOpen.size();
    const std::size_t name_end   = block.find('>', name_begin);
    if (name_end == std::string_view::npos || name_end == name_begin) { return false; }
    const std::size_t value_begin = name_end + 1;
    std::size_t depth             = 1;
    std::size_t scan              = value_begin;
    std::size_t close             = block.find(kParamClose, scan);
    for (;;) {
        if (close == std::string_view::npos) { return false; }
        std::size_t nested_open_end = 0;
        if (find_parameter_open_before(block, scan, close, nested_open_end)) {
            ++depth;
            scan = nested_open_end;
            continue;
        }
        if (--depth == 0) {
            parameter = {block.substr(name_begin, name_end - name_begin),
                         block.substr(value_begin, close - value_begin)};
            pos       = close + kParamClose.size();
            return true;
        }
        scan  = close + kParamClose.size();
        close = block.find(kParamClose, scan);
    }
}

bool consume_suffix(std::string_view block, std::size_t& end, std::string_view token) {
    while (end != 0 && is_format_whitespace(block[end - 1])) { --end; }
    if (end < token.size() || block.substr(end - token.size(), token.size()) != token) {
        return false;
    }
    end -= token.size();
    return true;
}

bool parse_custom_input(std::string_view block, std::size_t pos, std::string& arguments,
                        std::size_t& consumed) {
    constexpr std::string_view kCustomOpen = "<parameter=input>";
    skip_format_whitespace(block, pos);
    if (!starts_with_at(block, pos, kCustomOpen)) { return false; }
    const std::size_t value_begin = pos + kCustomOpen.size();
    for (std::size_t close = block.find(kToolClose, value_begin); close != std::string_view::npos;
         close             = block.find(kToolClose, close + kToolClose.size())) {
        std::size_t after = close + kToolClose.size();
        skip_format_whitespace(block, after);
        if (after != block.size() && !starts_with_at(block, after, kToolOpen)) { continue; }
        std::size_t end = close;
        // Preserve custom outermost raw framing, including standalone tags in its input.
        if (!consume_suffix(block, end, kFunctionClose) ||
            !consume_suffix(block, end, kParamClose) || end < value_begin) {
            continue;
        }
        arguments = remove_parameter_framing_newlines(block.substr(value_begin, end - value_begin));
        consumed  = after;
        return true;
    }
    return false;
}

bool parse_one_tool_call(std::string_view block, std::size_t max_name_length,
                         const ToolArgumentTypeContracts& contracts, ToolCall& out,
                         std::size_t& consumed) {
    std::size_t pos = 0;
    skip_format_whitespace(block, pos);
    if (!starts_with_at(block, pos, kFunctionOpen)) { return false; }
    const std::size_t name_begin = pos + kFunctionOpen.size();
    const std::size_t name_end   = block.find('>', name_begin);
    if (name_end == std::string_view::npos) { return false; }
    const std::string_view name = block.substr(name_begin, name_end - name_begin);
    if (!valid_function_name(name, max_name_length)) { return false; }
    pos                            = name_end + 1;
    const Contract::Tool* contract = find_tool_contract(contracts, name);
    if (contract == nullptr && contracts.names_authoritative) { return false; }
    const bool custom = contract != nullptr && contract->kind == ToolKind::Custom;
    std::string arguments;
    if (custom) {
        if (!parse_custom_input(block, pos, arguments, consumed)) { return false; }
    } else {
        std::unordered_set<std::string_view> names;
        arguments = "{";
        for (;;) {
            skip_format_whitespace(block, pos);
            if (starts_with_at(block, pos, kFunctionClose)) {
                pos += kFunctionClose.size();
                break;
            }
            RawParameter parameter;
            if (!parse_parameter(block, pos, parameter) || !names.insert(parameter.name).second) {
                return false;
            }
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
        skip_format_whitespace(block, pos);
        if (!starts_with_at(block, pos, kToolClose)) { return false; }
        pos += kToolClose.size();
        skip_format_whitespace(block, pos);
        if (pos != block.size() && !starts_with_at(block, pos, kToolOpen)) { return false; }
        consumed = pos;
    }
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
        ToolCall call;
        std::size_t consumed = 0;
        if (!parse_one_tool_call(std::string_view(text).substr(inner_begin), max_tool_name_length,
                                 contracts, call, consumed)) {
            return fallback(text);
        }
        out.tool_calls.push_back(std::move(call));
        pos = inner_begin + consumed;
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
