#pragma once

// Independent mathematical oracle: no production codec helpers or CUDA intrinsics.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace rk4_reference {
inline double even(double x) {
    const double f = std::floor(x), d = x - f;
    return f + (d > 0.5 || (d == 0.5 && std::fmod(f, 2.0) != 0.0));
}
inline std::uint16_t half_bits(double x) {
    const unsigned sign = std::signbit(x) ? 0x8000 : 0;
    x = std::abs(x);
    if (x == 0) return sign;
    int e;
    std::frexp(x, &e);
    e -= 1;
    if (e < -14) return static_cast<std::uint16_t>(sign | static_cast<unsigned>(even(std::ldexp(x, 24))));
    unsigned mantissa = static_cast<unsigned>(even(std::ldexp(x, 10 - e)));
    if (mantissa == 2048) { mantissa = 1024; ++e; }
    if (e > 15) return static_cast<std::uint16_t>(sign | 0x7c00);
    return static_cast<std::uint16_t>(sign | ((e + 15) << 10) | (mantissa - 1024));
}
inline double half(std::uint16_t bits) {
    const int e = (bits >> 10) & 31, m = bits & 1023;
    const double x = e == 0 ? std::ldexp(double(m), -24) : std::ldexp(1.0 + m / 1024.0, e - 15);
    return bits & 0x8000 ? -x : x;
}
inline void rotate(std::array<double, 256>& x) {
    for (int step = 1; step < 256; step *= 2)
        for (int base = 0; base < 256; base += 2 * step)
            for (int i = 0; i < step; ++i) {
                const double a = x[base+i], b = x[base+i+step];
                x[base+i] = a+b; x[base+i+step] = a-b;
            }
    for (auto& v : x) v /= 16.0;
}
struct Row {
    std::array<std::uint8_t, 128> codes{};
    std::array<std::uint16_t, 8> scales{};
    std::array<double, 256> decoded{};
};
inline Row encode(std::array<double, 256> x, bool key) {
    Row row;
    if (key) rotate(x);
    constexpr double boundaries[] = {0.25825, 0.52245, 0.79960, 1.09930, 1.43715, 1.84355, 2.40080};
    constexpr int levels[] = {6, 18, 31, 44, 58, 75, 96, 127};
    const int group = key ? 64 : 32;
    for (int base = 0; base < 256; base += group) {
        std::array<int, 64> indices{}, codes{};
        double sumsq = 0, maximum = 0, xc = 0, cc = 0;
        for (int d = 0; d < group; ++d) { sumsq += x[base+d]*x[base+d]; maximum = std::max(maximum, std::abs(x[base+d])); }
        const double rms = std::sqrt(sumsq / group);
        if (key) {
            for (int d = 0; d < group; ++d) {
                int m = 0;
                const double z = rms ? std::abs(x[base+d]) / rms : 0;
                for (auto boundary : boundaries) m += z >= boundary;
                indices[d] = m + (x[base+d] < 0 ? 8 : 0);
                codes[d] = (x[base+d] < 0 ? -1 : 1) * levels[m];
                xc += x[base+d] * codes[d]; cc += codes[d] * codes[d];
            }
            row.scales[base/group] = half_bits(xc/cc);
        } else row.scales[base/group] = half_bits(maximum / 7.0);
        const double scale = half(row.scales[base/group]);
        for (int d = 0; d < group; ++d) {
            if (!key) { codes[d] = scale ? std::clamp(int(even(x[base+d]/scale)), -7, 7) : 0; indices[d] = codes[d] & 15; }
            row.codes[(base+d)/2] |= static_cast<std::uint8_t>(indices[d] << (4*((base+d)&1)));
            row.decoded[base+d] = codes[d]*scale;
        }
    }
    return row;
}
} // namespace rk4_reference
