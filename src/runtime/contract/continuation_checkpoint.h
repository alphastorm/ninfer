#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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
    // True when a second batch may be submitted while an earlier completion is still
    // outstanding; a queue that serialises batches must be drained before the next submit.
    [[nodiscard]] virtual bool overlaps_batches() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<ContinuationCheckpointReadCompletion>
    submit(const std::filesystem::path& path,
           std::span<const ContinuationCheckpointReadRequest> requests) = 0;
};

class ContinuationCheckpointWriter {
public:
    virtual ~ContinuationCheckpointWriter()                   = default;
    virtual bool write_file(std::string_view path, std::uint64_t offset, std::uint64_t total_bytes,
                            std::span<const std::byte> bytes) = 0;
};

class ContinuationCheckpointReader {
public:
    virtual ~ContinuationCheckpointReader() = default;
    [[nodiscard]] virtual std::optional<std::uint64_t> file_size(std::string_view path) const = 0;
    virtual bool read_file(std::string_view path, std::uint64_t offset,
                           std::span<std::byte> destination) const                            = 0;
};

struct ContinuationCheckpointStats {
    std::uint32_t frontier_tokens = 0;
    std::uint32_t restored_tokens = 0;
    std::uint64_t payload_bytes   = 0;

    [[nodiscard]] friend constexpr bool operator==(ContinuationCheckpointStats,
                                                   ContinuationCheckpointStats) noexcept = default;
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

// Names the first failed target or store-local export gate. Outer engine gates retain the
// SessionCheckpointSkipReason vocabulary above; none of these diagnostics change admission.
enum class ContinuationExportSkipReason : std::uint8_t {
    None = 0,
    StagingBufferEmpty,
    ContinuationInvalid,
    ContextTransactionBusy,
    ExecutionBatchPending,
    MaterializationPinned,
    KvMissing,
    EndpointNotRetained,
    StateForkPending,
    StateSourceRetained,
    StateDestinationReserved,
    StateBindingSplit,
    SharedPrefixReferences,
    RewriteStateMismatch,
    EmptyFrontier,
    TextKvFrontierMismatch,
    SpeculativeKvPresenceMismatch,
    SpeculativeKvFrontierMismatch,
    BackendStateWithoutSpeculation,
    StateHandleInvalid,
    StateNotImmutable,
    StateCountOverflow,
    HostKvCapacityExceeded,
    MetadataWriteFailed,
    StateLayoutEmpty,
    StateStagingTooSmall,
    StateWriteFailed,
    HostKvArenaMissing,
    HostKvExtentsMissing,
    KvPayloadEmpty,
    KvStrideZero,
    KvPayloadOverflow,
    KvTemporaryPageUnavailable,
    KvReplicaMissing,
    TextKvWriteFailed,
    SpeculativeKvWriteFailed,
    PayloadOverflow,
    MetadataStagingTooSmall,
    SequenceLedgerOverflow,
    AnchorStateCountMismatch,
    PrefixDigestCountOverflow,
    PrefixTokenCountOverflow,
    PrefixVisionCountOverflow,
    PrefixRewriteCountOverflow,
    VisionTimestampCountOverflow,
    VisionTokenSpanCountOverflow,
    StateExportDestinationMissing,
    StateExportLayoutMismatch,
    StateHostBackingMissing,
    StateReplicaMissing,
    UnexpectedException,
    StoreInputInvalid,
    StoreStatsInvalid,
};

[[nodiscard]] constexpr std::string_view
continuation_export_skip_reason_name(ContinuationExportSkipReason reason) noexcept {
    switch (reason) {
    case ContinuationExportSkipReason::None:
        return "none";
    case ContinuationExportSkipReason::StagingBufferEmpty:
        return "staging buffer is zero";
    case ContinuationExportSkipReason::ContinuationInvalid:
        return "continuation handle invalid";
    case ContinuationExportSkipReason::ContextTransactionBusy:
        return "context transaction in progress";
    case ContinuationExportSkipReason::ExecutionBatchPending:
        return "execution batch pending";
    case ContinuationExportSkipReason::MaterializationPinned:
        return "continuation pinned by materialization";
    case ContinuationExportSkipReason::KvMissing:
        return "continuation has no KV";
    case ContinuationExportSkipReason::EndpointNotRetained:
        return "endpoint is not retained";
    case ContinuationExportSkipReason::StateForkPending:
        return "state fork pending";
    case ContinuationExportSkipReason::StateSourceRetained:
        return "state read source retained";
    case ContinuationExportSkipReason::StateDestinationReserved:
        return "state destination reserved";
    case ContinuationExportSkipReason::StateBindingSplit:
        return "state binding split";
    case ContinuationExportSkipReason::SharedPrefixReferences:
        return "active shared-prefix references remain";
    case ContinuationExportSkipReason::RewriteStateMismatch:
        return "rewrite state/metadata disagree";
    case ContinuationExportSkipReason::EmptyFrontier:
        return "empty execution frontier";
    case ContinuationExportSkipReason::TextKvFrontierMismatch:
        return "text KV frontier differs";
    case ContinuationExportSkipReason::SpeculativeKvPresenceMismatch:
        return "speculative KV presence differs";
    case ContinuationExportSkipReason::SpeculativeKvFrontierMismatch:
        return "speculative KV frontier differs";
    case ContinuationExportSkipReason::BackendStateWithoutSpeculation:
        return "backend state exists with speculation disabled";
    case ContinuationExportSkipReason::StateHandleInvalid:
        return "state handle invalid";
    case ContinuationExportSkipReason::StateNotImmutable:
        return "state image not immutable";
    case ContinuationExportSkipReason::StateCountOverflow:
        return "state count not representable";
    case ContinuationExportSkipReason::HostKvCapacityExceeded:
        return "checkpoint exceeds total host KV capacity";
    case ContinuationExportSkipReason::MetadataWriteFailed:
        return "metadata write refused";
    case ContinuationExportSkipReason::StateLayoutEmpty:
        return "state layout empty";
    case ContinuationExportSkipReason::StateStagingTooSmall:
        return "state image exceeds staging buffer";
    case ContinuationExportSkipReason::StateWriteFailed:
        return "state write refused";
    case ContinuationExportSkipReason::HostKvArenaMissing:
        return "host KV arena unavailable";
    case ContinuationExportSkipReason::HostKvExtentsMissing:
        return "host KV extent store unavailable";
    case ContinuationExportSkipReason::KvPayloadEmpty:
        return "empty KV payload";
    case ContinuationExportSkipReason::KvStrideZero:
        return "KV page stride is zero";
    case ContinuationExportSkipReason::KvPayloadOverflow:
        return "KV payload size overflows";
    case ContinuationExportSkipReason::KvTemporaryPageUnavailable:
        return "temporary host KV page unavailable";
    case ContinuationExportSkipReason::KvReplicaMissing:
        return "KV page has no exportable replica";
    case ContinuationExportSkipReason::TextKvWriteFailed:
        return "text KV write refused";
    case ContinuationExportSkipReason::SpeculativeKvWriteFailed:
        return "speculative KV write refused";
    case ContinuationExportSkipReason::PayloadOverflow:
        return "aggregate payload size overflows";
    case ContinuationExportSkipReason::MetadataStagingTooSmall:
        return "metadata exceeds staging limit";
    case ContinuationExportSkipReason::SequenceLedgerOverflow:
        return "sequence ledger count exceeds uint32";
    case ContinuationExportSkipReason::AnchorStateCountMismatch:
        return "anchor state count differs";
    case ContinuationExportSkipReason::PrefixDigestCountOverflow:
        return "prefix digest count exceeds uint32";
    case ContinuationExportSkipReason::PrefixTokenCountOverflow:
        return "prefix token count exceeds uint32";
    case ContinuationExportSkipReason::PrefixVisionCountOverflow:
        return "prefix vision item count exceeds uint32";
    case ContinuationExportSkipReason::PrefixRewriteCountOverflow:
        return "prefix rewrite frontier count exceeds uint32";
    case ContinuationExportSkipReason::VisionTimestampCountOverflow:
        return "vision timestamp count exceeds uint32";
    case ContinuationExportSkipReason::VisionTokenSpanCountOverflow:
        return "vision token span count exceeds uint32";
    case ContinuationExportSkipReason::StateExportDestinationMissing:
        return "state export destination is null";
    case ContinuationExportSkipReason::StateExportLayoutMismatch:
        return "state export layout differs";
    case ContinuationExportSkipReason::StateHostBackingMissing:
        return "host state replica has no backing pool";
    case ContinuationExportSkipReason::StateReplicaMissing:
        return "state image has no exportable replica";
    case ContinuationExportSkipReason::UnexpectedException:
        return "exception during export";
    case ContinuationExportSkipReason::StoreInputInvalid:
        return "checkpoint store input invalid";
    case ContinuationExportSkipReason::StoreStatsInvalid:
        return "returned checkpoint statistics invalid";
    }
    return "unknown";
}

enum class ContinuationExportStage : std::uint8_t {
    None = 0,
    Preconditions,
    StateInventory,
    HostKvCapacity,
    MetadataEncode,
    MetadataWrite,
    ComputeFence,
    StateStaging,
    StateCopy,
    StateSync,
    StateWrite,
    KvLookup,
    KvHostAllocation,
    KvCopy,
    KvSync,
    KvWrite,
    PayloadAccounting,
};

[[nodiscard]] constexpr std::string_view
continuation_export_stage_name(ContinuationExportStage reason) noexcept {
    switch (reason) {
    case ContinuationExportStage::None:
        return "none";
    case ContinuationExportStage::Preconditions:
        return "preconditions";
    case ContinuationExportStage::StateInventory:
        return "state inventory";
    case ContinuationExportStage::HostKvCapacity:
        return "host KV capacity check";
    case ContinuationExportStage::MetadataEncode:
        return "metadata encode";
    case ContinuationExportStage::MetadataWrite:
        return "metadata write";
    case ContinuationExportStage::ComputeFence:
        return "compute fence";
    case ContinuationExportStage::StateStaging:
        return "state staging allocation";
    case ContinuationExportStage::StateCopy:
        return "state copy";
    case ContinuationExportStage::StateSync:
        return "state synchronization";
    case ContinuationExportStage::StateWrite:
        return "state write";
    case ContinuationExportStage::KvLookup:
        return "KV page lookup";
    case ContinuationExportStage::KvHostAllocation:
        return "temporary host KV allocation";
    case ContinuationExportStage::KvCopy:
        return "KV page copy";
    case ContinuationExportStage::KvSync:
        return "KV synchronization";
    case ContinuationExportStage::KvWrite:
        return "KV page write";
    case ContinuationExportStage::PayloadAccounting:
        return "payload accounting";
    }
    return "unknown";
}

enum class ContinuationExportKind : std::uint8_t { None = 0, State, TextKv, SpeculativeKv };
enum class ContinuationExportException : std::uint8_t { None = 0, Standard, NonStandard };

// Fixed enums and scalar context only: reporting a refusal cannot allocate or throw.
struct ContinuationExportSkipDetail {
    ContinuationExportSkipReason reason = ContinuationExportSkipReason::None;
    ContinuationExportStage stage       = ContinuationExportStage::None;
    ContinuationExportKind kind         = ContinuationExportKind::None;
    ContinuationExportException exception = ContinuationExportException::None;
    std::optional<std::uint32_t> ordinal;
    std::optional<std::uint64_t> required_bytes;
    std::optional<std::uint64_t> capacity_bytes;
    std::optional<std::uint64_t> occupied_bytes;

    static void record(ContinuationExportSkipDetail* detail,
                       ContinuationExportSkipReason reason) noexcept {
        if (detail != nullptr && detail->reason == ContinuationExportSkipReason::None) {
            detail->reason = reason;
        }
    }
};

struct SessionCheckpointSkipDetail {
    SessionCheckpointSkipReason reason = SessionCheckpointSkipReason::None;
    // The store keeps ProgramRejected for legacy shutdown accounting, but must not erase
    // the engine gate that arrived first. The formatter reports both classifications.
    SessionCheckpointSkipReason first_reason = SessionCheckpointSkipReason::None;
    ContinuationExportSkipDetail export_detail;
    // The checkpoint tag the refused save attempted (response-id correlation for logs).
    std::string attempted_tag;
    // TagMismatch only: the tag the catalogued continuation actually carries.
    std::string catalogued_tag;

    // Diagnostics must never turn a clean refusal into an exception: on allocation
    // failure the detail stays empty and the refusal path proceeds (alphastorm/ninfer#31).
    static void assign_tag(std::string& field, std::string_view tag) noexcept {
        try {
            field.assign(tag);
        } catch (...) { field.clear(); }
    }
};

// Names the first failed gate inside a target's continuation import. The resource manager only
// sees "the program refused", which cannot distinguish a capacity bound - host KV extents,
// logical pages, state slots, continuation slots - from a malformed payload. Reported by value
// through an out-parameter so diagnostics never allocate on the refusal path.
enum class ContinuationImportSkipReason : std::uint8_t {
    None = 0,
    NotReady,
    MetadataUnreadable,
    MetadataInvalid,
    SpeculativeMismatch,
    StateStagingTooSmall,
    StatePayloadInvalid,
    StateImportFailed,
    KvAddressSpaceExhausted,
    KvLogicalPagesExhausted,
    KvHostCapacityExhausted,
    KvPayloadInvalid,
    KvAddressRestoreFailed,
    SequenceAssemblyFailed,
    ContinuationSlotsExhausted,
    CommitFailed,
};

[[nodiscard]] constexpr std::string_view
continuation_import_skip_reason_name(ContinuationImportSkipReason reason) noexcept {
    switch (reason) {
    case ContinuationImportSkipReason::None:
        return "none";
    case ContinuationImportSkipReason::NotReady:
        return "import preconditions unmet";
    case ContinuationImportSkipReason::MetadataUnreadable:
        return "continuation metadata unreadable";
    case ContinuationImportSkipReason::MetadataInvalid:
        return "continuation metadata invalid";
    case ContinuationImportSkipReason::SpeculativeMismatch:
        return "speculative backend does not match the checkpoint";
    case ContinuationImportSkipReason::StateStagingTooSmall:
        return "state image exceeds the staging buffer";
    case ContinuationImportSkipReason::StatePayloadInvalid:
        return "state image payload invalid";
    case ContinuationImportSkipReason::StateImportFailed:
        return "state image slots exhausted";
    case ContinuationImportSkipReason::KvAddressSpaceExhausted:
        return "KV address spaces exhausted";
    case ContinuationImportSkipReason::KvLogicalPagesExhausted:
        return "logical KV pages exhausted";
    case ContinuationImportSkipReason::KvHostCapacityExhausted:
        return "host KV pool capacity exhausted";
    case ContinuationImportSkipReason::KvPayloadInvalid:
        return "KV payload invalid";
    case ContinuationImportSkipReason::KvAddressRestoreFailed:
        return "KV address restore failed";
    case ContinuationImportSkipReason::SequenceAssemblyFailed:
        return "sequence assembly failed";
    case ContinuationImportSkipReason::ContinuationSlotsExhausted:
        return "continuation slots exhausted";
    case ContinuationImportSkipReason::CommitFailed:
        return "import commit failed";
    }
    return "unknown";
}

// True when the gate names a pool shared across live continuations rather than a property of the
// checkpoint being imported. Only these are worth retrying after dropping another session: the
// rest fail identically no matter how much room is freed.
[[nodiscard]] constexpr bool
contended_import_capacity(ContinuationImportSkipReason reason) noexcept {
    switch (reason) {
    case ContinuationImportSkipReason::StateImportFailed:
    case ContinuationImportSkipReason::KvAddressSpaceExhausted:
    case ContinuationImportSkipReason::KvLogicalPagesExhausted:
    case ContinuationImportSkipReason::KvHostCapacityExhausted:
    case ContinuationImportSkipReason::ContinuationSlotsExhausted:
        return true;
    default:
        return false;
    }
}

// Names the first failed gate when a verified checkpoint is declined at restore. Export has had
// named gates since alphastorm/ninfer#31; restore returned a bare nullopt, so a decline read as
// "the engine did not accept the checkpointed continuation" with no way to tell a capacity bound
// from a drifted binding without attaching a debugger (alphastorm/omp-ninfer#40).
enum class SessionRestoreSkipReason : std::uint8_t {
    None = 0,
    CacheDisabled,
    EmptyTag,
    PublicationOrderInvalid,
    TransactionBusy,
    LiveSessionDrift,
    LiveTagMismatch,
    LiveStatsInvalid,
    CatalogFull,
    ProgramRejected,
    StatsMismatch,
    SummaryInvalid,
    PublicationFailed,
};

[[nodiscard]] constexpr std::string_view
session_restore_skip_reason_name(SessionRestoreSkipReason reason) noexcept {
    switch (reason) {
    case SessionRestoreSkipReason::None:
        return "none";
    case SessionRestoreSkipReason::CacheDisabled:
        return "engine context cache disabled";
    case SessionRestoreSkipReason::EmptyTag:
        return "empty checkpoint tag";
    case SessionRestoreSkipReason::PublicationOrderInvalid:
        return "invalid publication order";
    case SessionRestoreSkipReason::TransactionBusy:
        return "resource transaction in progress";
    case SessionRestoreSkipReason::LiveSessionDrift:
        return "live session binding drifted";
    case SessionRestoreSkipReason::LiveTagMismatch:
        return "live session carries a different checkpoint tag";
    case SessionRestoreSkipReason::LiveStatsInvalid:
        return "live session frontier does not match the checkpoint";
    case SessionRestoreSkipReason::CatalogFull:
        return "no vacant continuation catalog slot";
    case SessionRestoreSkipReason::ProgramRejected:
        return "program refused continuation import";
    case SessionRestoreSkipReason::StatsMismatch:
        return "restored continuation statistics differ from the checkpoint";
    case SessionRestoreSkipReason::SummaryInvalid:
        return "restored continuation summary is invalid";
    case SessionRestoreSkipReason::PublicationFailed:
        return "session publication failed";
    }
    return "unknown";
}

// A restore that needs room may drop a *different* live continuation instead of refusing, but
// only when dropping it loses nothing: that session's own checkpoint must already be current on
// disk, so its next request restores it. Only the serve layer owns the checkpoint store, so it
// answers per candidate. Absent oracle means nothing is reclaimable, which is the old behaviour.
class ReclaimableSessionOracle {
public:
    ReclaimableSessionOracle()                                           = default;
    virtual ~ReclaimableSessionOracle()                                  = default;
    ReclaimableSessionOracle(const ReclaimableSessionOracle&)            = delete;
    ReclaimableSessionOracle& operator=(const ReclaimableSessionOracle&) = delete;

    // True when session_sha256's stored checkpoint already covers checkpoint_tag. Must answer
    // false - never throw - when the store cannot prove it: an unprovable candidate is kept.
    [[nodiscard]] virtual bool recoverable(std::string_view session_sha256,
                                           std::string_view checkpoint_tag) const noexcept = 0;
};

struct SessionRestoreSkipDetail {
    SessionRestoreSkipReason reason = SessionRestoreSkipReason::None;
    // ProgramRejected only: the gate the target's import path reported, when it named one.
    ContinuationImportSkipReason import_reason = ContinuationImportSkipReason::None;
    // Checkpoint-backed continuations dropped to make room before this outcome. Non-zero on
    // success means the restore only fit because reclaim ran.
    std::uint32_t reclaimed = 0;
    // Reclaimable candidates the oracle declined to confirm, i.e. capacity held by sessions
    // whose checkpoints are not current. Separates "pool too small" from "nothing safe to drop".
    std::uint32_t reclaim_declined = 0;
};

// What save-before-evict found for one victim.
enum class PressureCheckpointOutcome : std::uint8_t {
    // Dropping the continuation loses nothing more: the checkpoint covers the turn, the turn is
    // not the session's newest stored response and never will be, or its save was refused for a
    // reason retrying cannot fix (the handler reports that refusal).
    Settled,
    // The turn may still become savable: its response is still being stored, or its save hit a
    // gate that clears once the engine quiesces. The engine keeps the continuation and asks again
    // when it next plans an admission that drops it.
    Pending,
};

// A live session that admission is about to drop from engine memory to make room for another
// request, named the way the checkpoint store names it. The handler fills in the outcome.
struct PressureCheckpointVictim {
    std::string session_sha256;
    std::string checkpoint_tag;
    PressureCheckpointOutcome outcome = PressureCheckpointOutcome::Pending;
};

// Admission can make room by evicting another session's continuation, or by dropping checkpoints
// that its export needs. When that session's newest turn is not on disk yet, the drop loses the
// turn for good (alphastorm/omp-ninfer#45). With a handler installed, the engine first hands such
// sessions to it - on the engine worker, outside the execution lock, so the handler saves through
// the ordinary checkpoint path, which takes that lock itself - and replans afterwards.
class PressureCheckpointHandler {
public:
    PressureCheckpointHandler()                                            = default;
    virtual ~PressureCheckpointHandler()                                   = default;
    PressureCheckpointHandler(const PressureCheckpointHandler&)            = delete;
    PressureCheckpointHandler& operator=(const PressureCheckpointHandler&) = delete;

    // Answers every victim through its outcome. The engine drops only Settled victims; a plan that
    // would drop a Pending one is abandoned, so the waiting request waits - bounded by its own
    // deadline - rather than trading a turn that can still be saved for cache.
    virtual void save_before_eviction(std::span<PressureCheckpointVictim> victims) noexcept = 0;
};

} // namespace ninfer::runtime
