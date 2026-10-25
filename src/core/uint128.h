#pragma once

#include <cstdint>
#include <limits>

namespace ninfer::core {

// Exact two-limb arithmetic for admission and cost calculations, including MSVC.
struct Uint128 {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    [[nodiscard]] friend constexpr bool operator==(Uint128, Uint128) noexcept = default;
};

[[nodiscard]] constexpr bool wide_less(Uint128 a, Uint128 b) noexcept {
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

[[nodiscard]] constexpr Uint128 wide_multiply(std::uint64_t a, std::uint64_t b) noexcept {
    const auto al = a & 0xffffffffULL, ah = a >> 32U;
    const auto bl = b & 0xffffffffULL, bh = b >> 32U;
    const auto ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    const auto middle = (ll >> 32U) + (lh & 0xffffffffULL) + (hl & 0xffffffffULL);
    return {hh + (lh >> 32U) + (hl >> 32U) + (middle >> 32U),
            (middle << 32U) | (ll & 0xffffffffULL)};
}

[[nodiscard]] constexpr Uint128 wide_add_saturating(Uint128 a, Uint128 b) noexcept {
    const auto lo = a.lo + b.lo;
    const auto carry = std::uint64_t(lo < a.lo);
    const auto hi = a.hi + b.hi;
    if (hi < a.hi || hi + carry < hi)
        return {UINT64_MAX, UINT64_MAX};
    return {hi + carry, lo};
}

// Saturates rather than discarding an overflow beyond the two limbs.
[[nodiscard]] constexpr Uint128 wide_multiply_saturating(Uint128 a, std::uint64_t b) noexcept {
    const auto low = wide_multiply(a.lo, b), high = wide_multiply(a.hi, b);
    if (high.hi || high.lo > UINT64_MAX - low.hi) return {UINT64_MAX, UINT64_MAX};
    return {high.lo + low.hi, low.lo};
}

// Requires 0 < bits < 64.
[[nodiscard]] constexpr Uint128 wide_shift_right(Uint128 value, unsigned bits) noexcept {
    return {value.hi >> bits, (value.lo >> bits) | (value.hi << (64U - bits))};
}

[[nodiscard]] constexpr std::uint64_t saturate_to_uint64(Uint128 value) noexcept {
    return value.hi ? UINT64_MAX : value.lo;
}

struct WideDivision {
    Uint128 quotient;
    Uint128 remainder;
};

// Requires a nonzero divisor. Binary long division is exact over all 128 input bits.
[[nodiscard]] constexpr WideDivision wide_divide(Uint128 numerator, Uint128 divisor) noexcept {
    WideDivision result{};
    for (unsigned bit = 128; bit-- != 0;) {
        const auto input = bit >= 64 ? (numerator.hi >> (bit - 64)) & 1U
                                    : (numerator.lo >> bit) & 1U;
        const bool carry = (result.remainder.hi >> 63U) != 0;
        result.remainder = {(result.remainder.hi << 1U) | (result.remainder.lo >> 63U),
                            (result.remainder.lo << 1U) | input};
        if (carry || !wide_less(result.remainder, divisor)) {
            const auto borrow = std::uint64_t(result.remainder.lo < divisor.lo);
            result.remainder = {result.remainder.hi - divisor.hi - borrow,
                                result.remainder.lo - divisor.lo};
            if (bit >= 64) result.quotient.hi |= std::uint64_t{1} << (bit - 64);
            else result.quotient.lo |= std::uint64_t{1} << bit;
        }
    }
    return result;
}

} // namespace ninfer::core
