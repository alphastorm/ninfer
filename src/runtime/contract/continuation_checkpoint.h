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

struct SessionCheckpointSkipDetail {
    SessionCheckpointSkipReason reason = SessionCheckpointSkipReason::None;
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

struct SessionRestoreSkipDetail {
    SessionRestoreSkipReason reason = SessionRestoreSkipReason::None;
    // ProgramRejected only: the gate the target's import path reported, when it named one.
    ContinuationImportSkipReason import_reason = ContinuationImportSkipReason::None;
};

} // namespace ninfer::runtime
