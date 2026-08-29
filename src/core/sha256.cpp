#include "core/sha256.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace ninfer::crypto {
namespace {

constexpr std::array<std::uint32_t, 64> kRound{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};

std::uint32_t load_be32(const std::byte* bytes) noexcept {
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
           std::to_integer<std::uint32_t>(bytes[3]);
}

void store_be32(std::uint32_t value, std::uint8_t* bytes) noexcept {
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[3] = static_cast<std::uint8_t>(value);
}

} // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
             0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

void Sha256::process_block(const std::byte* block) noexcept {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i) { words[i] = load_be32(block + 4 * i); }
    for (std::size_t i = 16; i < words.size(); ++i) {
        const std::uint32_t s0 =
            std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3U);
        const std::uint32_t s1 =
            std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10U);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::uint32_t sum1     = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t choose   = (e & f) ^ (~e & g);
        const std::uint32_t t1       = h + sum1 + choose + kRound[i] + words[i];
        const std::uint32_t sum0     = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2       = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(std::span<const std::byte> input) {
    if (finished_) { throw std::logic_error("SHA-256 has already been finalized"); }
    if (input.size() > std::numeric_limits<std::uint64_t>::max() - total_bytes_) {
        throw std::overflow_error("SHA-256 input length overflowed");
    }
    total_bytes_ += static_cast<std::uint64_t>(input.size());

    if (tail_bytes_ != 0) {
        const std::size_t count = std::min(input.size(), tail_.size() - tail_bytes_);
        std::copy_n(input.data(), count, tail_.data() + tail_bytes_);
        tail_bytes_ += count;
        input = input.subspan(count);
        if (tail_bytes_ != tail_.size()) { return; }
        if (tail_bytes_ == tail_.size()) {
            process_block(tail_.data());
            tail_bytes_ = 0;
        }
    }
    while (input.size() >= tail_.size()) {
        process_block(input.data());
        input = input.subspan(tail_.size());
    }
    std::copy(input.begin(), input.end(), tail_.begin());
    tail_bytes_ = input.size();
}

Sha256Digest Sha256::finish() {
    if (finished_) { throw std::logic_error("SHA-256 has already been finalized"); }
    if (total_bytes_ > std::numeric_limits<std::uint64_t>::max() / 8ULL) {
        throw std::overflow_error("SHA-256 bit length overflowed");
    }
    const std::uint64_t bits = total_bytes_ * 8ULL;
    tail_[tail_bytes_++] = std::byte{0x80};
    if (tail_bytes_ > 56) {
        std::fill(tail_.begin() + static_cast<std::ptrdiff_t>(tail_bytes_), tail_.end(), std::byte{});
        process_block(tail_.data());
        tail_bytes_ = 0;
    }
    std::fill(tail_.begin() + static_cast<std::ptrdiff_t>(tail_bytes_), tail_.begin() + 56,
              std::byte{});
    for (std::size_t i = 0; i < 8; ++i) {
        tail_[63 - i] = static_cast<std::byte>(bits >> (8U * i));
    }
    process_block(tail_.data());
    finished_ = true;

    Sha256Digest digest{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        store_be32(state_[i], digest.data() + 4 * i);
    }
    return digest;
}

Sha256Digest sha256(std::span<const std::byte> input) {
    Sha256 hasher;
    hasher.update(input);
    return hasher.finish();
}

std::string sha256_hex(const Sha256Digest& digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '\0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        result[2 * i]     = hex[digest[i] >> 4U];
        result[2 * i + 1] = hex[digest[i] & 0x0fU];
    }
    return result;
}

} // namespace ninfer::crypto
