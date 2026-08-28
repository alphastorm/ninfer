#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ninfer::runtime {

// A checkpoint namespace may only be constructed at an authenticated caller boundary. Both
// components are fixed-size digests so the runtime never accepts raw tenant or session names and
// can bind disk and device state without an ambiguous concatenation.
class AuthenticatedCheckpointNamespace {
public:
    [[nodiscard]] static AuthenticatedCheckpointNamespace
    authenticated(std::string tenant_sha256, std::string session_sha256) {
        if (!valid_sha256(tenant_sha256) || !valid_sha256(session_sha256)) {
            throw std::invalid_argument(
                "authenticated checkpoint tenant and session must be lowercase SHA-256");
        }
        return AuthenticatedCheckpointNamespace(std::move(tenant_sha256),
                                                std::move(session_sha256));
    }

    [[nodiscard]] std::string_view tenant_sha256() const noexcept { return tenant_sha256_; }
    [[nodiscard]] std::string_view session_sha256() const noexcept { return session_sha256_; }

    [[nodiscard]] static bool valid_sha256(std::string_view digest) noexcept {
        return digest.size() == 64 &&
               std::all_of(digest.begin(), digest.end(), [](unsigned char byte) {
                   return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
               });
    }

    friend bool operator==(const AuthenticatedCheckpointNamespace&,
                           const AuthenticatedCheckpointNamespace&) = default;

private:
    AuthenticatedCheckpointNamespace(std::string tenant_sha256, std::string session_sha256)
        : tenant_sha256_(std::move(tenant_sha256)), session_sha256_(std::move(session_sha256)) {}

    std::string tenant_sha256_;
    std::string session_sha256_;
};

class ContinuationCheckpointWriter {
public:
    virtual ~ContinuationCheckpointWriter() = default;
    virtual bool write_file(std::string_view path, std::uint64_t offset,
                            std::uint64_t total_bytes,
                            std::span<const std::byte> bytes) = 0;
};

class ContinuationCheckpointReader {
public:
    virtual ~ContinuationCheckpointReader() = default;
    [[nodiscard]] virtual std::optional<std::uint64_t>
    file_size(std::string_view path) const = 0;
    virtual bool read_file(std::string_view path, std::uint64_t offset,
                           std::span<std::byte> destination) const = 0;
};

struct ContinuationCheckpointStats {
    std::uint32_t frontier_tokens = 0;
    std::uint32_t restored_tokens = 0;
    std::uint64_t payload_bytes   = 0;

    friend bool operator==(const ContinuationCheckpointStats&,
                           const ContinuationCheckpointStats&) = default;
};

} // namespace ninfer::runtime
