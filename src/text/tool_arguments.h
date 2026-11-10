#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace ninfer::text {

// Preserve object member order. Empty input becomes {}; malformed or non-object input retains
// its exact text as {"input": raw}, matching Custom tools and the templates' mapping contract.
// object_input lets a wire adapter retain a valid object's original serialization.
[[nodiscard]] nlohmann::ordered_json tool_arguments_object(std::string_view raw,
                                                          bool* object_input = nullptr);

} // namespace ninfer::text
