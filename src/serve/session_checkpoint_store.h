#pragma once

#include "serve/response_store.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <vector>

namespace ninfer::serve {
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
};

inline constexpr std::uint32_t kSessionCheckpointSchemaVersion = 2;

struct SessionCheckpointStoreOptions {
    std::filesystem::path root;
    std::uint64_t disk_quota_bytes = 64ULL << 30;
    std::size_t staging_bytes      = 256ULL << 20;
};

struct SessionCheckpointSaveResult {
    std::string generation;
    ContinuationCheckpointStats engine;
    std::uint64_t bytes = 0;
};

struct VerifiedSessionCheckpoint {
    ResponseStoreSnapshot responses;
    std::shared_ptr<const ContinuationCheckpointReader> engine;
    ContinuationCheckpointStats expected_engine;
    std::string generation;
    std::uint64_t bytes = 0;
};

// Exact, binary-preserving encoding for response bodies, typed tool items, media bytes, thinking,
// and the shared context DAG. The limit is checked before allocation and again after encoding.
[[nodiscard]] std::vector<std::byte>
encode_response_store_snapshot(const ResponseStoreSnapshot& snapshot, std::size_t byte_limit);
[[nodiscard]] std::optional<ResponseStoreSnapshot>
decode_response_store_snapshot(std::span<const std::byte> bytes, std::size_t byte_limit);

class SessionCheckpointStore {
public:
    using EngineExporter = std::function<std::optional<ContinuationCheckpointStats>(
        ContinuationCheckpointWriter&)>;

    class Impl;
    explicit SessionCheckpointStore(SessionCheckpointStoreOptions options);

    [[nodiscard]] const SessionCheckpointStoreOptions& options() const noexcept { return options_; }

    // The exporter writes every Engine payload into a unique staging generation. Payloads and their
    // checksums are flushed first; manifest.json is written last; only then is current atomically
    // replaced. A failed or cancelled save never changes the previously published generation.
    [[nodiscard]] std::optional<SessionCheckpointSaveResult>
    save(const ResponseStoreSnapshot& responses, const nlohmann::json& runtime_fingerprint,
         const EngineExporter& exporter);

    // Verifies identity, manifest schema, every size/checksum, and the requested response id before
    // exposing either ResponseStore state or an Engine reader. Incompatibility is a miss; corrupt
    // current generations are quarantined and ignored.
    [[nodiscard]] std::optional<VerifiedSessionCheckpoint>
    load(std::string_view session_sha256, const nlohmann::json& runtime_fingerprint,
         std::optional<std::string_view> required_response_id = std::nullopt);

    [[nodiscard]] nlohmann::json status(std::string_view session_sha256,
                                        const nlohmann::json& runtime_fingerprint) const;
    bool erase(std::string_view session_sha256);
    void collect_garbage();

private:
    [[nodiscard]] std::filesystem::path session_path(std::string_view digest) const;
    [[nodiscard]] static bool valid_digest(std::string_view digest) noexcept;

    SessionCheckpointStoreOptions options_;
    std::shared_ptr<Impl> impl_;
};

} // namespace ninfer::serve
