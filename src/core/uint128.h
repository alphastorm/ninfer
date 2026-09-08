#pragma once

#include <cstdint>
#include <limits>

// Exact unsigned 128-bit arithmetic on two 64-bit limbs. The runtime's cost and admission
// arithmetic saturates at 64 bits after computing exact products; MSVC has no __int128, and
// these few operations are off the hot path, so a constexpr schoolbook implementation serves
// every toolchain identically.
namespace ninfer::core {

struct Uint128 {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;

    [[nodiscard]] friend constexpr bool operator==(Uint128, Uint128) noexcept = default;
};

[[nodiscard]] constexpr bool wide_less(Uint128 left, Uint128 right) noexcept {
    return left.hi < right.hi || (left.hi == right.hi && left.lo < right.lo);
}

[[nodiscard]] constexpr Uint128 wide_multiply(std::uint64_t left, std::uint64_t right) noexcept {
    const std::uint64_t left_low   = left & 0xffffffffULL;
    const std::uint64_t left_high  = left >> 32U;
    const std::uint64_t right_low  = right & 0xffffffffULL;
    const std::uint64_t right_high = right >> 32U;

    const std::uint64_t low_low   = left_low * right_low;
    const std::uint64_t low_high  = left_low * right_high;
    const std::uint64_t high_low  = left_high * right_low;
    const std::uint64_t high_high = left_high * right_high;

    const std::uint64_t middle = (low_low >> 32U) + (low_high & 0xffffffffULL) +
                                 (high_low & 0xffffffffULL);
    Uint128 result;
    result.lo = (middle << 32U) | (low_low & 0xffffffffULL);
    result.hi = high_high + (low_high >> 32U) + (high_low >> 32U) + (middle >> 32U);
    return result;
}

// Saturates at 2^128 - 1 instead of wrapping.
[[nodiscard]] constexpr Uint128 wide_add_saturating(Uint128 left, Uint128 right) noexcept {
    Uint128 result;
    result.lo                = left.lo + right.lo;
    const std::uint64_t carry = result.lo < left.lo ? 1U : 0U;
    result.hi                = left.hi + right.hi + carry;
    if (result.hi < left.hi || (carry == 1U && result.hi == left.hi)) {
        return Uint128{std::numeric_limits<std::uint64_t>::max(),
                       std::numeric_limits<std::uint64_t>::max()};
    }
    return result;
}

// Requires 0 < bits < 64.
[[nodiscard]] constexpr Uint128 wide_shift_right(Uint128 value, unsigned bits) noexcept {
    return Uint128{value.hi >> bits, (value.lo >> bits) | (value.hi << (64U - bits))};
}

[[nodiscard]] constexpr std::uint64_t saturate_to_uint64(Uint128 value) noexcept {
    return value.hi != 0 ? std::numeric_limits<std::uint64_t>::max() : value.lo;
}

} // namespace ninfer::core
