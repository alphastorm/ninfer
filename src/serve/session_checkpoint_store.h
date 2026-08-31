#pragma once

#include "runtime/contract/continuation_checkpoint.h"
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
inline constexpr std::uint32_t kSessionCheckpointSchemaVersion = 2;

struct SessionCheckpointStoreOptions {
    std::filesystem::path root;
    std::uint64_t disk_quota_bytes = 64ULL << 30;
    std::size_t staging_bytes      = 256ULL << 20;
    std::shared_ptr<runtime::ContinuationCheckpointReadQueue> read_queue;
    std::function<std::uint64_t(const std::filesystem::path&)> generation_size;
    std::function<bool(const std::filesystem::path&)> tombstone_cleanup;
};

struct SessionCheckpointSaveResult {
    std::string generation;
    runtime::ContinuationCheckpointStats engine;
    std::uint64_t bytes = 0;
};

struct VerifiedSessionCheckpoint {
    ResponseStoreSnapshot responses;
    std::shared_ptr<const runtime::ContinuationCheckpointReader> engine;
    runtime::ContinuationCheckpointStats expected_engine;
    std::string generation;
    std::uint64_t bytes = 0;
};

enum class SessionCheckpointLoadState : std::uint8_t {
    Available,
    Missing,
    Incompatible,
    Corrupt,
    Unavailable,
};

struct SessionCheckpointLoadResult {
    SessionCheckpointLoadState state = SessionCheckpointLoadState::Missing;
    std::optional<VerifiedSessionCheckpoint> checkpoint;
};

enum class SessionCheckpointEraseResult : std::uint8_t {
    Erased,
    Missing,
    Conflict,
};

// Exact, binary-preserving encoding for response bodies, typed tool items, media bytes, thinking,
// and the shared context DAG. The limit is checked before allocation and again after encoding.
[[nodiscard]] std::vector<std::byte>
encode_response_store_snapshot(const ResponseStoreSnapshot& snapshot, std::size_t byte_limit);
[[nodiscard]] std::optional<ResponseStoreSnapshot>
decode_response_store_snapshot(std::span<const std::byte> bytes, std::size_t byte_limit);

class SessionCheckpointStore {
public:
    using EngineExporter = std::function<std::optional<runtime::ContinuationCheckpointStats>(
        runtime::ContinuationCheckpointWriter&)>;

    class Impl;
    explicit SessionCheckpointStore(SessionCheckpointStoreOptions options);

    [[nodiscard]] const SessionCheckpointStoreOptions& options() const noexcept { return options_; }

    // The exporter writes every Engine payload into a unique staging generation. Payloads and their
    // checksums are flushed first; manifest.json is written last; only then is current atomically
    // replaced. A failed or cancelled save never changes the previously published generation.
    [[nodiscard]] std::optional<SessionCheckpointSaveResult>
    save(const ResponseStoreSnapshot& responses, const nlohmann::json& runtime_fingerprint,
         const EngineExporter& exporter, runtime::SessionCheckpointSkipDetail* skip = nullptr);

    // Verifies identity, manifest schema, every size/checksum, and the requested response id before
    // exposing either ResponseStore state or an Engine reader. Verified corruption is quarantined;
    // transient filesystem failures preserve current for a later retry.
    [[nodiscard]] SessionCheckpointLoadResult
    load(std::string_view session_sha256, const nlohmann::json& runtime_fingerprint,
         std::optional<std::string_view> required_response_id = std::nullopt);

    [[nodiscard]] nlohmann::json status(std::string_view session_sha256,
                                        const nlohmann::json& runtime_fingerprint) const;
    [[nodiscard]] SessionCheckpointEraseResult erase(std::string_view session_sha256);
    void collect_garbage();

private:
    [[nodiscard]] std::filesystem::path session_path(std::string_view digest) const;
    [[nodiscard]] static bool valid_digest(std::string_view digest) noexcept;

    SessionCheckpointStoreOptions options_;
    std::shared_ptr<Impl> impl_;
};

} // namespace ninfer::serve
