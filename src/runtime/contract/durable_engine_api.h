#pragma once

#include "runtime/contract/durable_checkpoint_set.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ninfer {
class Engine;
class PreparedPrompt;
class GenerationHandle;
}

namespace ninfer::runtime {

// Streaming implementations live in continuation_checkpoint.h (DurableStore ownership).
// Writer.finish() drains owned writes; Reader.verify_complete() authenticates every delivered
// engine payload byte exactly once. Neither success may mean merely "queued" or "file exists".
class ContinuationCheckpointWriter;
class ContinuationCheckpointReader;
class ContinuationCheckpointReadQueue;
class PressureCheckpointHandler;
class ReclaimableSessionOracle;

enum class DurableExportState : std::uint8_t { Exported, Refused, Stale };
enum class DurableImportState : std::uint8_t {
    Imported, CapacityRefused, Corrupt, IdentityMismatch, PolicyDisabled, Unavailable
};
enum class DurablePrepareState : std::uint8_t {
    Prepared, CapacityRefused, Corrupt, IdentityMismatch, PolicyDisabled, Unavailable
};

// Stable snake_case spellings in K7, never ordinal serialization. No raw session/tag/path detail.
enum class DurableReason : std::uint8_t {
    None,
    StoreDisabled,
    CacheDisabled,
    NoSessionRecords,
    EmptyTag,
    SessionNotIndexed,
    TransactionBusy,
    TagMismatch,
    StalePublication,
    StaleRevision,
    UnsupportedCodec,
    IncompatibleSchema,
    FingerprintMismatch,
    OriginRequired,
    OriginKeyUnavailable,
    OriginMacMismatch,
    InvalidManifest,
    InvalidIndex,
    PayloadSizeMismatch,
    PayloadDigestMismatch,
    MetadataLimit,
    StagingLimit,
    Capacity,
    Quota,
    ForeignSessionCollision,
    RequiredResponseMissing,
    IoUnavailable,
    DurabilityUnsupported,
    Cancelled
};

// Single spelling authority for bounded diagnostics and K7 receipts. "unknown" diagnoses an
// invalid enum value; it is not an additional wire reason and MUST NOT enter a valid receipt.
[[nodiscard]] constexpr std::string_view durable_reason_name(DurableReason reason) noexcept {
    switch (reason) {
    case DurableReason::None: return "none";
    case DurableReason::StoreDisabled: return "store_disabled";
    case DurableReason::CacheDisabled: return "cache_disabled";
    case DurableReason::NoSessionRecords: return "no_session_records";
    case DurableReason::EmptyTag: return "empty_tag";
    case DurableReason::SessionNotIndexed: return "session_not_indexed";
    case DurableReason::TransactionBusy: return "transaction_busy";
    case DurableReason::TagMismatch: return "tag_mismatch";
    case DurableReason::StalePublication: return "stale_publication";
    case DurableReason::StaleRevision: return "stale_revision";
    case DurableReason::UnsupportedCodec: return "unsupported_codec";
    case DurableReason::IncompatibleSchema: return "incompatible_schema";
    case DurableReason::FingerprintMismatch: return "fingerprint_mismatch";
    case DurableReason::OriginRequired: return "origin_required";
    case DurableReason::OriginKeyUnavailable: return "origin_key_unavailable";
    case DurableReason::OriginMacMismatch: return "origin_mac_mismatch";
    case DurableReason::InvalidManifest: return "invalid_manifest";
    case DurableReason::InvalidIndex: return "invalid_index";
    case DurableReason::PayloadSizeMismatch: return "payload_size_mismatch";
    case DurableReason::PayloadDigestMismatch: return "payload_digest_mismatch";
    case DurableReason::MetadataLimit: return "metadata_limit";
    case DurableReason::StagingLimit: return "staging_limit";
    case DurableReason::Capacity: return "capacity";
    case DurableReason::Quota: return "quota";
    case DurableReason::ForeignSessionCollision: return "foreign_session_collision";
    case DurableReason::RequiredResponseMissing: return "required_response_missing";
    case DurableReason::IoUnavailable: return "io_unavailable";
    case DurableReason::DurabilityUnsupported: return "durability_unsupported";
    case DurableReason::Cancelled: return "cancelled";
    }
    return "unknown";
}

struct DurableLimits {
    std::uint64_t staging_bytes = 256ULL << 20;
    std::uint64_t write_buffer_bytes = 6ULL << 30;
    std::uint64_t manifest_bytes = kDurableManifestMaxBytes;
    std::uint32_t max_files = kDurableMaxFiles;
    std::uint32_t max_points = kDurableMaxPoints;
};

// Runtime-only compare-and-publish token; never persisted or reused across Engine lifetimes.
struct DurableSessionEpoch {
    std::uint64_t value = 0;
    friend bool operator==(DurableSessionEpoch, DurableSessionEpoch) = default;
};

struct DurableExportResult {
    DurableExportState state = DurableExportState::Refused;
    DurableReason reason = DurableReason::None;
    DurableCheckpointStats stats;
    DurableSessionEpoch captured_epoch;
};

struct DurablePublicationResult {
    DurableExportState state = DurableExportState::Refused;
    DurableReason reason = DurableReason::None;
};

struct DurableImportResult {
    DurableImportState state = DurableImportState::PolicyDisabled;
    DurableReason reason = DurableReason::None;
    DurableCheckpointStats stats;
};

struct DurableImportTarget {
    std::string session_sha256;
    std::string expected_tag;
    DurableSessionEpoch expected_epoch;
    // Authenticated principal scope; caller-derived, never read from the checkpoint manifest.
    // Private scope is derived from session_sha256 as "http:" + digest.
    std::optional<std::string> shared_reuse_scope;
    // Serving resolves CacheParticipation::Disabled (including warmup) to false. Both gates must
    // be true; disk state never enables caching or overrides request policy.
    bool allow_prefix_reuse = false;
    bool cache_participation = false;
};

// Move-only Engine-owned transaction. Implementation owns native ImportReservation plus all
// ResourceManager publication capacity. Destruction/abort drains outstanding writers before
// releasing reservations. Prepared means invisible; only commit publishes native owners.
class DurableEngineImport {
public:
    DurableEngineImport(DurableEngineImport&&) noexcept;
    DurableEngineImport& operator=(DurableEngineImport&&) noexcept;
    DurableEngineImport(const DurableEngineImport&) = delete;
    DurableEngineImport& operator=(const DurableEngineImport&) = delete;
    ~DurableEngineImport();

    // Revalidates epoch/policy and publishes with a fresh publication_order; preallocated commit
    // has no fallible step after native publication. May run under the Responses publication lock.
    [[nodiscard]] DurableImportResult commit() noexcept;
    void abort() noexcept;

private:
    struct Impl;
    explicit DurableEngineImport(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class CheckpointEngineAccess;
};

struct DurablePrepareResult {
    DurablePrepareState state = DurablePrepareState::PolicyDisabled;
    DurableReason reason = DurableReason::None;
    // Present iff Prepared. Prepare is not Imported and must never be logged as visible success.
    std::optional<DurableEngineImport> prepared;
};

// Internal adapter only: no persistence surface is added to the public Engine product API.
// Existing shared Engine/PreparedPrompt friendship is integration-owned.
class CheckpointEngineAccess {
public:
    // Loaded immutable identity, queried once at serving startup and cached in K2.
    // Never reconstruct it from paths or re-hash resources for individual saves.
    [[nodiscard]] static DurableNativeLayout checkpoint_layout(const Engine&);
    [[nodiscard]] static DurableFrontendIdentity frontend_identity(const Engine&);

    // Live serving installs trusted store-backed providers, never authority from disk metadata.
    // With no oracle, import cannot reclaim an unrelated session.
    static void set_pressure_checkpoint_handler(
        Engine&, std::shared_ptr<PressureCheckpointHandler>);
    static void set_reclaimable_session_oracle(
        Engine&, std::shared_ptr<const ReclaimableSessionOracle>);
    static void notify_checkpoint_progress(Engine&) noexcept;

    static void set_checkpoint_tag(PreparedPrompt&, std::string_view tag);
    [[nodiscard]] static DurableSessionEpoch session_epoch(Engine&, std::string_view session_sha256);
    [[nodiscard]] static DurableSessionEpoch publication_epoch(const GenerationHandle&);
    [[nodiscard]] static std::shared_ptr<ContinuationCheckpointReadQueue> make_read_queue(
        Engine&, const std::filesystem::path& root);

    // Stage immutable native content under Engine serialization; hash/write off-lock. Retain
    // leases until copies retire. Recheck expected tag/epoch before caller publishes current.
    [[nodiscard]] static DurableExportResult export_session(
        Engine&, std::string_view session_sha256, std::string_view expected_tag,
        ContinuationCheckpointWriter&, const DurableLimits&);

    // Mandatory for live saves: serialize the final tag/epoch check and current-pointer
    // replace+sync with submission/finish epoch changes under the SAME per-session gate.
    // Never hold the Engine execution lock or wait on its worker while holding that gate.
    // Callback does only final pointer publication, not payload/manifest/hash work; Stale
    // never invokes it. False/exception maps to Refused/IoUnavailable, not acknowledged save.
    [[nodiscard]] static DurablePublicationResult publish_export(
        Engine&, std::string_view session_sha256, std::string_view expected_tag,
        DurableSessionEpoch expected_epoch, const std::function<bool()>& commit_callback);

    // Reader and fingerprint are verified before preparing any visible native/session state.
    // Caller retains Reader until this synchronous prepare returns; transaction owns the bytes.
    [[nodiscard]] static DurablePrepareResult prepare_import(
        Engine&, const DurableImportTarget&, const ContinuationCheckpointReader&,
        const RuntimeFingerprintV2&, const DurableCheckpointStats&, const DurableLimits&);

    // Convenience for non-Responses callers only. Responses MUST use prepare + K4 commit callback.
    [[nodiscard]] static DurableImportResult import_session(
        Engine&, const DurableImportTarget&, const ContinuationCheckpointReader&,
        const RuntimeFingerprintV2&, const DurableCheckpointStats&, const DurableLimits&);
};

// Native Program owns concrete move-only ExportLease and ImportReservation types and methods:
//   prepare_checkpoint_export(span<const CheckpointHandle>, limits) -> ExportLease
//   prepare_checkpoint_import(const DurableCheckpointSet&, const Reader&, limits) -> ImportReservation
//   commit_import(ImportReservation&&) -> vector<CheckpointHandle>
// They never accept HTTP session names. ExportLease pins the full reachable immutable set;
// ImportReservation reserves/fills Host-only state/KV and checkpoint slots, publishes nothing,
// and rolls back only after fences retire. Exact result mapping is K3 in CONTRACTS.md.

} // namespace ninfer::runtime
