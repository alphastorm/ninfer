#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/sha256.h"

namespace ninfer::serve {

// Constant-time credential equality (alphastorm/ninfer#22). Both sides are reduced to
// fixed-length SHA-256 digests first, so neither the byte position of the first mismatch nor
// the length relationship between the presented and stored credential shapes the comparison
// time. Use for every bearer-key and session-ownership equality; never for plain data.
[[nodiscard]] inline bool credential_equal(std::string_view presented,
                                           std::string_view expected) noexcept {
    const crypto::Sha256Digest lhs =
        crypto::sha256(std::as_bytes(std::span<const char>(presented.data(), presented.size())));
    const crypto::Sha256Digest rhs =
        crypto::sha256(std::as_bytes(std::span<const char>(expected.data(), expected.size())));
    std::uint8_t acc = 0U;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        acc = static_cast<std::uint8_t>(acc | (lhs[index] ^ rhs[index]));
    }
    return acc == 0U;
}

} // namespace ninfer::serve
