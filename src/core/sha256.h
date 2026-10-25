#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ninfer::crypto {

using Sha256Digest = std::array<std::uint8_t, 32>;

class Sha256 {
public:
    Sha256() noexcept;

    void update(std::span<const std::byte> input);
    [[nodiscard]] Sha256Digest finish();

private:
    void process_block(const std::byte* block) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::array<std::byte, 64> tail_{};
    std::uint64_t total_bytes_ = 0;
    std::size_t tail_bytes_    = 0;
    bool finished_             = false;
};

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> input);
[[nodiscard]] std::string sha256_hex(const Sha256Digest& digest);
// RFC 2104 HMAC-SHA256. Keys longer than the 64-byte block are pre-hashed per the RFC.
[[nodiscard]] Sha256Digest hmac_sha256(std::span<const std::byte> key,
                                       std::span<const std::byte> message);

} // namespace ninfer::crypto
