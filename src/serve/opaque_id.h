#pragma once

#include <string>
#include <string_view>

namespace ninfer::serve {

using OpaqueIdEntropySource = int (*)(unsigned char*, int);

// Append 128 bits from the platform entropy source to an exact wire prefix.
std::string new_opaque_id(std::string_view prefix);
// Injectable only so the failure and wire-format contracts are deterministic in unit tests.
std::string new_opaque_id_with_entropy(std::string_view prefix, OpaqueIdEntropySource source);

} // namespace ninfer::serve
