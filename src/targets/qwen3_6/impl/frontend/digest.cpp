#include "targets/qwen3_6/impl/frontend/digest.h"

#include "core/sha256.h"

namespace ninfer::targets::qwen3_6::frontend_internal {

Sha256Digest sha256(std::span<const std::uint8_t> input) {
    return crypto::sha256(std::as_bytes(input));
}

Sha256Digest sha256(std::string_view input) {
    return crypto::sha256(std::as_bytes(std::span(input.data(), input.size())));
}

std::string sha256_hex(const Sha256Digest& digest) { return crypto::sha256_hex(digest); }

} // namespace ninfer::targets::qwen3_6::frontend_internal
