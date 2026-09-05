#pragma once

#include "runtime/contract/continuation_checkpoint.h"
#include "serve/response_store.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <vector>

namespace ninfer::serve {
inline constexpr std::uint32_t kSessionCheckpointSchemaVersion = 3;

struct SessionCheckpointStoreOptions {
    std::filesystem::path root;
    std::uint64_t disk_quota_bytes = 64ULL << 30;
    std::size_t staging_bytes      = 256ULL << 20;
    std::uint32_t io_timeout_ms    = 30'000;
    // Bounds the in-memory chunk queue between the engine-held export and the drain thread
    // that performs the disk writes (ninfer#34). The engine blocks only when the queue is
    // full; sizing it at or above the typical payload keeps engine-held time at staging cost.
    std::size_t write_buffer_bytes = 6ULL << 30;
    std::shared_ptr<runtime::ContinuationCheckpointReadQueue> read_queue;
    std::function<bool(const std::filesystem::path&)> tombstone_cleanup;
    // Host tests inject the post-publication directory sync and final size scan.
    std::function<void(const std::filesystem::path&)> current_pointer_sync;
    // Test-only fault seam for the final generation-size scan before reader ownership begins.
    std::function<std::uint64_t(const std::filesystem::path&)> generation_size;
    // Authenticates checkpoint manifest ORIGIN, not just consistency (alphastorm/ninfer#32):
    // 32 raw bytes of HMAC-SHA256 key material held outside the checkpoint root (derived from
    // the bearer key, never the bearer key itself). Saves write a manifest.mac sibling; loads
    // verify it. Empty disables production and verification - without a configured bearer key
    // no authenticated origin exists.
    std::string origin_mac_key;
    // Refuse generations without a valid origin MAC (the NAS/S3 import posture). Off keeps
    // the compatibility window: unMAC'd locally-produced generations still load, and only a
    // present-but-wrong MAC is treated as tampering.
    bool require_origin_auth = false;
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
    save(const runtime::AuthenticatedCheckpointNamespace& checkpoint_namespace,
         const ResponseStoreSnapshot& responses, const nlohmann::json& runtime_fingerprint,
         const EngineExporter& exporter, bool reclaim_superseded_generation = false);

    // Verifies identity, manifest schema, every size/checksum, and the requested response id before
    // exposing either ResponseStore state or an Engine reader. Verified corruption is quarantined;
    // transient filesystem failures preserve current for a later retry.
    [[nodiscard]] SessionCheckpointLoadResult
    load(const runtime::AuthenticatedCheckpointNamespace& checkpoint_namespace,
         const nlohmann::json& runtime_fingerprint,
         std::optional<std::string_view> required_response_id = std::nullopt);

    [[nodiscard]] nlohmann::json status(
                                        const runtime::AuthenticatedCheckpointNamespace&
                                            checkpoint_namespace,
                                        const nlohmann::json& runtime_fingerprint) const;
    // True when the current catalogued generation is fingerprint-compatible and already
    // records response_id as the session's newest stored response. Lets the automatic
    // save path skip a redundant re-export without status ever exposing response
    // identity (that non-disclosure is a tested boundary).
    [[nodiscard]] bool covers(const runtime::AuthenticatedCheckpointNamespace&
                                  checkpoint_namespace,
                              const nlohmann::json& runtime_fingerprint,
                              std::string_view response_id) const;
    [[nodiscard]] SessionCheckpointEraseResult
    erase(const runtime::AuthenticatedCheckpointNamespace& checkpoint_namespace);
    void collect_garbage();

private:
    [[nodiscard]] std::filesystem::path
    session_path(const runtime::AuthenticatedCheckpointNamespace& checkpoint_namespace) const;

    SessionCheckpointStoreOptions options_;
    std::shared_ptr<Impl> impl_;
};

// Injectable boundary around the two Engine operations that touch device continuation state. The
// production adapter calls runtime::CheckpointEngineAccess; host tests provide a deterministic
// in-memory codec without constructing an Engine or loading a model.
struct SessionCheckpointEngine {
    using Checkpoint = std::function<std::optional<runtime::ContinuationCheckpointStats>(
        const runtime::AuthenticatedCheckpointNamespace&, std::string_view,
        runtime::ContinuationCheckpointWriter&, std::size_t)>;
    using Restore = std::function<std::optional<runtime::ContinuationCheckpointStats>(
        const runtime::AuthenticatedCheckpointNamespace&, std::string_view,
        const runtime::ContinuationCheckpointReader&, runtime::ContinuationCheckpointStats,
        std::size_t)>;

    Checkpoint checkpoint;
    Restore restore;
};

enum class SessionCheckpointSaveState : std::uint8_t {
    Disabled,
    Saved,
    Missing,
    Failed,
    Unavailable,
};

struct SessionCheckpointSaveOutcome {
    SessionCheckpointSaveState state = SessionCheckpointSaveState::Disabled;
    std::optional<SessionCheckpointSaveResult> checkpoint;
};

enum class SessionCheckpointRestoreState : std::uint8_t {
    Disabled,
    Restored,
    Missing,
    Incompatible,
    Corrupt,
    Unavailable,
    Failed,
};

class SessionCheckpointManager {
public:
    SessionCheckpointManager() = default;
    SessionCheckpointManager(SessionCheckpointStoreOptions options,
                             nlohmann::json runtime_fingerprint, std::string tenant_sha256,
                             SessionCheckpointEngine engine);

    [[nodiscard]] bool enabled() const noexcept { return store_ != nullptr; }
    [[nodiscard]] SessionCheckpointSaveOutcome
    save(std::string_view session_sha256, std::string_view required_response_id,
         ResponseStore& responses);
    [[nodiscard]] SessionCheckpointRestoreState
    restore(std::string_view session_sha256, std::string_view required_response_id,
            ResponseStore& responses);
    [[nodiscard]] nlohmann::json status(std::string_view session_sha256);
    // True when the catalogued checkpoint already records response_id as the session's
    // newest stored response under the manager's runtime fingerprint (redundant-save skip).
    [[nodiscard]] bool covers(std::string_view session_sha256, std::string_view response_id);
    [[nodiscard]] SessionCheckpointEraseResult erase(std::string_view session_sha256);
    [[nodiscard]] SessionCheckpointEraseResult
    erase_response(std::string_view session_sha256, std::string_view response_id,
                   ResponseStore& responses);

private:
    [[nodiscard]] SessionCheckpointRestoreState
    restore_locked(std::string_view session_sha256, std::string_view required_response_id,
                   ResponseStore& responses);

    std::unique_ptr<SessionCheckpointStore> store_;
    nlohmann::json runtime_fingerprint_;
    std::string tenant_sha256_;
    SessionCheckpointEngine engine_;
    std::mutex mutex_;
};

} // namespace ninfer::serve
