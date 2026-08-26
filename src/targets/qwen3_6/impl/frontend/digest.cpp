#include "targets/qwen3_6/impl/frontend/digest.h"

#include "core/sha256.h"

#include <algorithm>
#include <cstddef>

namespace ninfer::targets::qwen3_6::frontend_internal {

Sha256Digest sha256(std::span<const std::uint8_t> input) { return sha256(input, {}); }

Sha256Digest sha256(std::span<const std::uint8_t> input,
                    const std::function<void()>& checkpoint) {
    crypto::Sha256 hasher;
    constexpr std::size_t kCheckpointBytes = 1ULL << 20;
    std::size_t offset                     = 0;
    while (offset < input.size()) {
        if (checkpoint) { checkpoint(); }
        const std::size_t count = std::min(kCheckpointBytes, input.size() - offset);
        hasher.update(std::as_bytes(input.subspan(offset, count)));
        offset += count;
    }
    if (input.empty() && checkpoint) { checkpoint(); }
    return hasher.finish();
}

Sha256Digest sha256(std::string_view input) {
    return crypto::sha256(std::as_bytes(std::span(input.data(), input.size())));
}

std::string sha256_hex(const Sha256Digest& digest) { return crypto::sha256_hex(digest); }

} // namespace ninfer::targets::qwen3_6::frontend_internal
