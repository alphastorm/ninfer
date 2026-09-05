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
#include <vector>

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

// Restore payload files are written contiguously in device-segment enumeration order, so a
// reader can cover a run of whole (or partial) segments with one staging-sized read instead of
// one read per KV page segment. A fixed-cost read request (DirectStorage submit and fence wait,
// or an open/seek/read/close) per page segment is what held native-lane restores near 10 MB/s.
struct ContinuationCheckpointReadPiece {
    std::size_t segment = 0; // index into the enumerated device segments
    std::size_t offset  = 0; // byte offset inside that segment
    std::size_t bytes   = 0;

    friend bool operator==(const ContinuationCheckpointReadPiece&,
                           const ContinuationCheckpointReadPiece&) = default;
};

struct ContinuationCheckpointReadWindow {
    std::uint64_t file_offset = 0;
    std::size_t bytes         = 0;
    std::size_t first_piece   = 0;
    std::size_t piece_count   = 0;

    friend bool operator==(const ContinuationCheckpointReadWindow&,
                           const ContinuationCheckpointReadWindow&) = default;
};

struct ContinuationCheckpointReadPlan {
    std::vector<ContinuationCheckpointReadPiece> pieces;
    std::vector<ContinuationCheckpointReadWindow> windows;
    std::uint64_t total_bytes = 0;
};

// Every window holds at most window_bytes and, except for the last, exactly window_bytes; pieces
// appear in segment order and partition every segment exactly once.
[[nodiscard]] inline ContinuationCheckpointReadPlan
plan_continuation_checkpoint_reads(std::span<const std::size_t> segment_bytes,
                                   std::size_t window_bytes) {
    if (window_bytes == 0) {
        throw std::invalid_argument("continuation checkpoint read window must be non-empty");
    }
    ContinuationCheckpointReadPlan plan;
    ContinuationCheckpointReadWindow window{};
    const auto close = [&]() {
        if (window.bytes == 0) { return; }
        plan.windows.push_back(window);
        window = ContinuationCheckpointReadWindow{
            .file_offset = window.file_offset + window.bytes,
            .first_piece = plan.pieces.size(),
        };
    };
    for (std::size_t index = 0; index < segment_bytes.size(); ++index) {
        const std::size_t bytes = segment_bytes[index];
        if (bytes == 0) {
            throw std::invalid_argument("continuation checkpoint device segment is empty");
        }
        std::size_t offset = 0;
        while (offset < bytes) {
            if (window.bytes == window_bytes) { close(); }
            const std::size_t amount = std::min(window_bytes - window.bytes, bytes - offset);
            plan.pieces.push_back({.segment = index, .offset = offset, .bytes = amount});
            ++window.piece_count;
            window.bytes += amount;
            offset += amount;
        }
        plan.total_bytes += bytes;
    }
    close();
    return plan;
}

// One reader call may span more bytes than a single queue request accepts; split it into
// contiguous requests of at most max_request_bytes so the queue still receives one batch.
[[nodiscard]] inline std::vector<ContinuationCheckpointReadRequest>
split_continuation_checkpoint_read(std::uint64_t file_offset, std::span<std::byte> destination,
                                   std::size_t max_request_bytes) {
    if (max_request_bytes == 0) {
        throw std::invalid_argument("continuation checkpoint request bound must be non-empty");
    }
    std::vector<ContinuationCheckpointReadRequest> requests;
    requests.reserve(destination.size() / max_request_bytes + 1);
    std::size_t offset = 0;
    while (offset < destination.size()) {
        const std::size_t amount = std::min(max_request_bytes, destination.size() - offset);
        requests.push_back({.file_offset = file_offset + offset,
                            .destination = destination.subspan(offset, amount)});
        offset += amount;
    }
    return requests;
}

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
