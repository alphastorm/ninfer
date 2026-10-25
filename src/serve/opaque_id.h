#pragma once

#include <string>
#include <string_view>

namespace ninfer::serve {
using OpaqueIdEntropySource = int (*)(unsigned char*, int);
std::string new_opaque_id_with_entropy(std::string_view prefix, OpaqueIdEntropySource source);
std::string new_opaque_id(std::string_view prefix);
} // namespace ninfer::serve
