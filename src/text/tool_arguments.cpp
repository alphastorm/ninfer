#include "text/tool_arguments.h"

namespace ninfer::text {

nlohmann::ordered_json tool_arguments_object(std::string_view raw, bool* object_input) {
    using Json = nlohmann::ordered_json;
    Json parsed = raw.empty() ? Json::object() : Json::parse(raw, nullptr, false);
    const bool object = !raw.empty() && parsed.is_object();
    if (object_input) *object_input = object;
    if (raw.empty() || object) return parsed;
    return Json{{"input", raw}};
}

} // namespace ninfer::text
