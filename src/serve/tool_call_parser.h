#pragma once

#include "serve/request.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ninfer::serve {

struct ParsedToolCallOutput {
    bool is_tool_call_response = false;
    std::string content;
    std::vector<ToolCall> tool_calls;
};

// Qwen arguments are untyped tag payloads. Retain supported top-level JSON Schema
// types for decoding; this is not recursive validation or constrained generation.
struct ToolArgumentTypeContracts {
    enum class SchemaType : std::uint8_t {
        Null    = 1U << 0U,
        Boolean = 1U << 1U,
        Integer = 1U << 2U,
        Number  = 1U << 3U,
        String  = 1U << 4U,
        Object  = 1U << 5U,
        Array   = 1U << 6U,
    };

    struct TypeSet {
        std::uint8_t bits = 0;
    };

    enum class DecodePolicy : std::uint8_t {
        Legacy,
        DeclaredTypes,
    };

    struct Parameter {
        std::string name;
        DecodePolicy policy = DecodePolicy::Legacy;
        TypeSet types;
    };

    struct Tool {
        std::string name;
        std::vector<Parameter> parameters;
        bool unambiguous = true;
        ToolKind kind    = ToolKind::Function;
    };

    std::vector<Tool> tools;
    bool names_authoritative = false;
};

ToolArgumentTypeContracts build_tool_argument_type_contracts(const GenerationRequest& request);

ParsedToolCallOutput parse_qwen_tool_call_output(const std::string& text,
                                                 std::size_t max_tool_name_length,
                                                 const ToolArgumentTypeContracts& contracts);

// Incrementally publishes text that is provably outside a possible Qwen
// <tool_call> suffix. At terminal time, a valid tool response discards the
// buffered tool region; malformed/non-tool output flushes it verbatim.
class ToolCallStreamFilter {
public:
    std::string feed(std::string_view text);
    std::string finish(bool is_tool_call_response);

    [[nodiscard]] std::size_t emitted_bytes() const noexcept { return emitted_bytes_; }

private:
    std::string trailing_whitespace_;
    std::string tool_region_;
    std::size_t marker_prefix_bytes_ = 0;
    std::size_t emitted_bytes_       = 0;
    bool saw_tool_marker_            = false;
    bool finished_                   = false;
};

} // namespace ninfer::serve
