#pragma once

#include "runtime/contract/continuation_checkpoint.h"
#include "runtime/contract/durable_engine_api.h"
#include "serve/openai_responses_store.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <system_error>
#include <vector>

namespace ninfer::serve {
inline constexpr std::uint32_t kSessionCheckpointSchemaVersion =
    runtime::kDurableSessionSchemaVersion;
inline constexpr std::string_view kResponseStoreSnapshotCodec = "ninfer-responses-cbor/1";

// Server-log diagnostic only. Operation is a fixed developer label, never a path/session/content.
// An omitted code is extracted from the active exception, without logging exception.what().
void log_checkpoint_failure(std::string_view operation, runtime::DurableReason reason,
                            std::error_code code = {}) noexcept;

struct SessionCheckpointStoreOptions {
    std::filesystem::path root;
    std::uint64_t disk_quota_bytes = 64ULL << 30;
    // When the quota is exceeded, other sessions' current checkpoints whose frontier is below this
    // many tokens are reclaimed before longer ones, oldest first within each group. A short session
    // costs little to prefill again; oldest-first alone let a run of short sessions reclaim every
    // long session's checkpoint. Zero keeps plain oldest-first order.
    std::uint64_t short_session_tokens = 0;
    std::size_t staging_bytes          = 256ULL << 20;
    // Bounds owned chunk copies queued to the hashing/file drain thread (ninfer#34).
    // Backpressure and finish() run off the Engine execution lock; the export lease keeps
    // native content stable while copies retire.
    std::size_t write_buffer_bytes = 6ULL << 30;
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
    std::shared_ptr<runtime::ContinuationCheckpointReadQueue> read_queue;
    std::function<std::uint64_t(const std::filesystem::path&)> generation_size;
    std::function<bool(const std::filesystem::path&)> tombstone_cleanup;
};

struct SessionCheckpointSaveResult {
    std::string generation;
    runtime::DurableCheckpointStats engine;
    std::uint64_t bytes = 0;
    // POSIX directory fsync versus NTFS journal + write-through replacement.
    std::string_view dir_sync;
    bool origin_authenticated = false;
};

struct VerifiedSessionCheckpoint {
    OpenAIResponseStoreSnapshot responses;
    std::shared_ptr<const runtime::ContinuationCheckpointReader> engine;
    runtime::DurableCheckpointStats expected_engine;
    std::string generation;
    std::uint64_t bytes = 0;
    bool origin_authenticated = false;
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
    runtime::DurableReason reason = runtime::DurableReason::None;
};

enum class SessionCheckpointEraseResult : std::uint8_t {
    Erased,
    Missing,
    Conflict,
};

// Exact, binary-preserving encoding for response bodies, typed tool items, media bytes, thinking,
// and the shared context DAG. Decode bounds input before parsing; encode refuses oversized output
// before any snapshot bytes are written to a generation.
[[nodiscard]] std::vector<std::byte>
encode_response_store_snapshot(const OpenAIResponseStoreSnapshot& snapshot, std::size_t byte_limit);
[[nodiscard]] std::optional<OpenAIResponseStoreSnapshot>
decode_response_store_snapshot(std::span<const std::byte> bytes, std::size_t byte_limit);

// Log-only, bounded storage: formatting a refusal never allocates or throws. Ordinary response
// ids fit in full; overlong tags are explicitly truncated so both tags and the session remain.
class SessionCheckpointSkipMessage {
public:
    [[nodiscard]] std::string_view view() const noexcept { return {bytes_.data(), size_}; }

private:
    friend SessionCheckpointSkipMessage format_session_checkpoint_skip(
        std::string_view prefix, std::string_view session_sha256,
        const runtime::SessionCheckpointSkipDetail& skip) noexcept;
    void append(std::string_view value) noexcept;
    void number(std::uint64_t value) noexcept;
    std::array<char, 1024> bytes_;
    std::size_t size_ = 0;
};

[[nodiscard]] SessionCheckpointSkipMessage format_session_checkpoint_skip(
    std::string_view prefix, std::string_view session_sha256,
    const runtime::SessionCheckpointSkipDetail& skip) noexcept;

class SessionCheckpointStore {
public:
    using ExportCheckpoint = std::function<runtime::DurableExportResult(
        runtime::ContinuationCheckpointWriter&)>;
    using PublishExport = std::function<runtime::DurablePublicationResult(
        runtime::DurableSessionEpoch, const std::function<bool()>&)>;

    class Impl;
    explicit SessionCheckpointStore(SessionCheckpointStoreOptions options);

    [[nodiscard]] const SessionCheckpointStoreOptions& options() const noexcept { return options_; }

    // The exporter writes every Engine payload into a unique staging generation. Payloads and their
    // checksums are flushed first; manifest.json is written last; only then is current atomically
    // replaced. A failed or cancelled save never changes the previously published generation.
    // Saves are single-flight: competing saves return TransactionBusy rather than blocking an
    // Engine worker. Neither exporter nor publication-gate entry holds the catalog mutex.
    [[nodiscard]] std::optional<SessionCheckpointSaveResult>
    save(const OpenAIResponseStoreSnapshot& responses, const nlohmann::json& runtime_fingerprint,
         const ExportCheckpoint& exporter, const PublishExport& publish_export,
         runtime::SessionCheckpointSkipDetail* skip = nullptr);

    // Authenticated post-delete survivors, explicitly without a native continuation.
    // Caller holds its per-session mutation guard through the subsequent no-throw RAM change.
    [[nodiscard]] std::optional<SessionCheckpointSaveResult>
    save_responses(const OpenAIResponseStoreSnapshot& responses,
                   const nlohmann::json& runtime_fingerprint,
                   runtime::SessionCheckpointSkipDetail* skip = nullptr);

    // Verifies identity, manifest schema, origin, every payload's presence and size, the response
    // snapshot's checksum, and the requested response id before exposing either ResponseStore
    // state or an Engine reader. Engine payload checksums are verified by the reader as the
    // engine streams them, exactly once; a mismatch fails the import closed and quarantines the
    // generation on the next load. Verified corruption is quarantined; transient filesystem
    // failures preserve current for a later retry.
    [[nodiscard]] SessionCheckpointLoadResult
    load(std::string_view session_sha256, const nlohmann::json& runtime_fingerprint,
         std::optional<std::string_view> required_response_id = std::nullopt);

    [[nodiscard]] nlohmann::json status(std::string_view session_sha256,
                                        const nlohmann::json& runtime_fingerprint) const;
    // True when the current catalogued generation is fingerprint-compatible and already
    // records response_id as the session's newest stored response. Lets the automatic
    // save path skip a redundant re-export without status ever exposing response
    // identity (that non-disclosure is a tested boundary).
    [[nodiscard]] bool covers(std::string_view session_sha256,
                              const nlohmann::json& runtime_fingerprint,
                              std::string_view response_id) const;
    [[nodiscard]] SessionCheckpointEraseResult erase(std::string_view session_sha256);
    // Integrity evidence only. Never removes a newer current generation; active readers keep
    // their payload paths until their final lease retires. False means no quarantine ack.
    [[nodiscard]] bool quarantine(std::string_view session_sha256, std::string_view generation,
                                  runtime::DurableReason reason) noexcept;
    // Lock-free pre-filter: false only when the session has no published current generation, so
    // a caller can skip load() and its locks for sessions that have nothing to restore.
    [[nodiscard]] bool may_hold(std::string_view session_sha256) const noexcept;
    void collect_garbage();

private:
    [[nodiscard]] std::optional<SessionCheckpointSaveResult>
    save_impl(const OpenAIResponseStoreSnapshot&, const nlohmann::json&,
              const ExportCheckpoint&, const PublishExport&, bool replay_only,
              runtime::SessionCheckpointSkipDetail*);
    [[nodiscard]] std::filesystem::path session_path(std::string_view digest) const;
    [[nodiscard]] static bool valid_digest(std::string_view digest) noexcept;

    SessionCheckpointStoreOptions options_;
    std::shared_ptr<Impl> impl_;
};

} // namespace ninfer::serve
