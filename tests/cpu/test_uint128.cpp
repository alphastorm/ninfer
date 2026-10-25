#include "core/uint128.h"

#include <iostream>
#include <stdexcept>

using namespace ninfer::core;
static_assert(wide_multiply(UINT64_MAX, UINT64_MAX) == Uint128{UINT64_MAX - 1, 1});
static_assert(wide_add_saturating({UINT64_MAX, UINT64_MAX}, {0, 1}) ==
              Uint128{UINT64_MAX, UINT64_MAX});
static_assert(wide_divide({1, 0}, {0, 3}).quotient == Uint128{0, 6148914691236517205ULL});

int main() {
    char operation;
    std::cin >> std::hex;
    std::cout << std::hex;
    while (std::cin >> operation) {
        Uint128 a{}, b{}, result{};
        if (!(std::cin >> a.hi >> a.lo >> b.hi >> b.lo)) return 1;
        switch (operation) {
        case 'm': result = wide_multiply(a.lo, b.lo); break;
        case 'a': result = wide_add_saturating(a, b); break;
        case 'p': result = wide_multiply_saturating(a, b.lo); break;
        case 's': result = wide_shift_right(a, static_cast<unsigned>(b.lo)); break;
        case 'd': {
            if (b == Uint128{}) return 1;
            const auto division = wide_divide(a, b);
            std::cout << division.quotient.hi << ' ' << division.quotient.lo << ' '
                      << division.remainder.hi << ' ' << division.remainder.lo << '\n';
            continue;
        }
        default: return 1;
        }
        std::cout << result.hi << ' ' << result.lo << '\n';
    }
    return std::cin.eof() ? 0 : 1;
}
