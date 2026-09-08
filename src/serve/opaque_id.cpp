#include "serve/opaque_id.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
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
    if (output == nullptr || length < 0) { return 0; }
#if defined(_WIN32)
    const NTSTATUS status = BCryptGenRandom(nullptr, output, static_cast<ULONG>(length),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0 ? 1 : 0;
#elif defined(__linux__)
    std::size_t offset = 0;
    while (offset < static_cast<std::size_t>(length)) {
        const ssize_t read = getrandom(output + offset, static_cast<std::size_t>(length) - offset, 0);
        if (read > 0) {
            offset += static_cast<std::size_t>(read);
            continue;
        }
        if (read < 0 && errno == EINTR) { continue; }
        return 0;
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
    } catch (...) {
        return 0;
    }
#endif
}

} // namespace

std::string new_opaque_id_with_entropy(std::string_view prefix, OpaqueIdEntropySource source) {
    if (prefix.empty()) {
        throw std::invalid_argument("opaque identifier prefix must not be empty");
    }
    if (source == nullptr) {
        throw std::invalid_argument("opaque identifier entropy source is null");
    }

    std::array<unsigned char, 16> bytes{};
    if (source(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("cryptographic random identifier generation failed");
    }

    static constexpr char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(prefix.size() + bytes.size() * 2);
    id.append(prefix);
    for (const unsigned char byte : bytes) {
        id.push_back(hex[byte >> 4U]);
        id.push_back(hex[byte & 0x0fU]);
    }
    return id;
}

std::string new_opaque_id(std::string_view prefix) {
    return new_opaque_id_with_entropy(prefix, system_entropy);
}

} // namespace ninfer::serve
