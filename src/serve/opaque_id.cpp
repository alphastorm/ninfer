#include "serve/opaque_id.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__)
#include <cstdlib>
#else
#include <random>
#endif

namespace ninfer::serve {
namespace {
int system_entropy(unsigned char* output, int length) {
    if (!output || length < 0) { return 0; }
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, output, static_cast<ULONG>(length),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0 ? 1 : 0;
#elif defined(__linux__)
    std::size_t offset = 0;
    while (offset < static_cast<std::size_t>(length)) {
        const ssize_t count = getrandom(output + offset, static_cast<std::size_t>(length) - offset, 0);
        if (count > 0) { offset += static_cast<std::size_t>(count); }
        else if (count < 0 && errno == EINTR) { continue; }
        else { return 0; }
    }
    return 1;
#elif defined(__APPLE__)
    arc4random_buf(output, static_cast<std::size_t>(length));
    return 1;
#else
    try {
        std::random_device source;
        for (int index = 0; index < length; ++index) {
            output[index] = static_cast<unsigned char>(source());
        }
        return 1;
    } catch (...) { return 0; }
#endif
}
} // namespace

std::string new_opaque_id_with_entropy(std::string_view prefix, OpaqueIdEntropySource source) {
    if (prefix.empty() || !source) { throw std::invalid_argument("invalid opaque ID source or prefix"); }
    std::array<unsigned char, 16> bytes{};
    if (source(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("cryptographic random identifier generation failed");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(prefix.size() + bytes.size() * 2);
    id.append(prefix);
    for (const auto byte : bytes) {
        id.push_back(hex[byte >> 4U]);
        id.push_back(hex[byte & 15U]);
    }
    return id;
}
std::string new_opaque_id(std::string_view prefix) {
    return new_opaque_id_with_entropy(prefix, system_entropy);
}
} // namespace ninfer::serve
