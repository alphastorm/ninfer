#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ninfer::runtime {

struct ContinuationCheckpointReadRequest {
    std::uint64_t file_offset = 0;
    std::span<std::byte> destination;
};

class ContinuationCheckpointReadCompletion {
public:
    virtual ~ContinuationCheckpointReadCompletion() = default;
    virtual void wait()                             = 0;
};

class ContinuationCheckpointReadQueue {
public:
    virtual ~ContinuationCheckpointReadQueue() = default;

    [[nodiscard]] virtual std::string_view backend_name() const noexcept       = 0;
    [[nodiscard]] virtual bool available() const noexcept                      = 0;
    [[nodiscard]] virtual std::string_view unavailable_reason() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<ContinuationCheckpointReadCompletion>
    submit(const std::filesystem::path& path,
           std::span<const ContinuationCheckpointReadRequest> requests) = 0;
};

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

// Names the first failed gate when a session checkpoint export is refused. The engine and the
// serve layer surface this so a skipped save is diagnosable from one log line instead of a
// precondition audit across layers.
enum class SessionCheckpointSkipReason : std::uint8_t {
    None = 0,
    StoreDisabled,
    NoSessionRecords,
    CacheDisabled,
    EmptyTag,
    TransactionBusy,
    SessionNotIndexed,
    IndexEntryInvalid,
    CatalogIdentityDrift,
    TagMismatch,
    ProgramRejected,
    QuotaExceeded,
};

[[nodiscard]] constexpr std::string_view
session_checkpoint_skip_reason_name(SessionCheckpointSkipReason reason) noexcept {
    switch (reason) {
    case SessionCheckpointSkipReason::None:
        return "none";
    case SessionCheckpointSkipReason::StoreDisabled:
        return "checkpoint store disabled";
    case SessionCheckpointSkipReason::NoSessionRecords:
        return "session has no stored responses";
    case SessionCheckpointSkipReason::CacheDisabled:
        return "engine context cache disabled";
    case SessionCheckpointSkipReason::EmptyTag:
        return "empty checkpoint tag";
    case SessionCheckpointSkipReason::TransactionBusy:
        return "resource transaction in progress";
    case SessionCheckpointSkipReason::SessionNotIndexed:
        return "session is not indexed in the engine";
    case SessionCheckpointSkipReason::IndexEntryInvalid:
        return "session index entry is invalid";
    case SessionCheckpointSkipReason::CatalogIdentityDrift:
        return "catalogued continuation identity drifted";
    case SessionCheckpointSkipReason::TagMismatch:
        return "catalogued checkpoint tag mismatch";
    case SessionCheckpointSkipReason::ProgramRejected:
        return "program refused continuation export";
    case SessionCheckpointSkipReason::QuotaExceeded:
        return "checkpoint disk quota exceeded";
    }
    return "unknown";
}

struct SessionCheckpointSkipDetail {
    SessionCheckpointSkipReason reason = SessionCheckpointSkipReason::None;
    // TagMismatch only: the tag the catalogued continuation actually carries.
    std::string catalogued_tag;
};

} // namespace ninfer::runtime
