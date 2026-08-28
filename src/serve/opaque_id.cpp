#include "serve/opaque_id.h"

#include <openssl/rand.h>

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ninfer::serve {

std::string new_opaque_id_with_entropy(std::string_view prefix, OpaqueIdEntropySource source) {
    if (prefix.empty()) {
        throw std::invalid_argument("opaque identifier prefix must not be empty");
    }
    if (source == nullptr) {
        throw std::invalid_argument("opaque identifier entropy source is null");
    }

    std::array<unsigned char, 16> bytes{};
    if (source(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("cryptographic random identifier generation failed");
    }

    static constexpr char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(prefix.size() + bytes.size() * 2);
    id.append(prefix);
    for (const unsigned char byte : bytes) {
        id.push_back(hex[byte >> 4U]);
        id.push_back(hex[byte & 0x0fU]);
    }
    return id;
}

std::string new_opaque_id(std::string_view prefix) {
    return new_opaque_id_with_entropy(prefix, RAND_bytes);
}

} // namespace ninfer::serve
