#pragma once

#include "runtime/contract/continuation_checkpoint.h"

namespace ninfer::serve {

// The gate a refused save actually stopped at. The store keeps ProgramRejected as its coarse
// reason for accounting but records the first engine-side gate it replaced.
[[nodiscard]] inline runtime::SessionCheckpointSkipReason
effective_checkpoint_skip_reason(const runtime::SessionCheckpointSkipDetail& skip) noexcept {
    using Reason = runtime::SessionCheckpointSkipReason;
    if (skip.reason == Reason::ProgramRejected && skip.first_reason != Reason::None) {
        return skip.first_reason;
    }
    return skip.reason;
}

// A refusal that means "not yet" rather than "cannot": the save ran while a request's
// transaction held the continuation, or before the newest turn's continuation was catalogued.
// The same save succeeds once the engine quiesces, so an automatic save retries it instead of
// dropping the session's newest state.
[[nodiscard]] inline bool
checkpoint_refusal_is_transient(const runtime::SessionCheckpointSkipDetail& skip) noexcept {
    using Reason = runtime::SessionCheckpointSkipReason;
    using Export = runtime::ContinuationExportSkipReason;
    switch (effective_checkpoint_skip_reason(skip)) {
    case Reason::TransactionBusy:
    case Reason::TagMismatch:
    case Reason::CatalogIdentityDrift:
        return true;
    case Reason::ProgramRejected:
        return skip.export_detail.reason == Export::ContextTransactionBusy ||
               skip.export_detail.reason == Export::ExecutionBatchPending ||
               skip.export_detail.reason == Export::MaterializationPinned;
    default:
        return false;
    }
}

// Retries allowed per completed turn before an automatic save gives up on a transient refusal.
// Each retry waits for the engine to quiesce again, so this bounds background work, not latency.
inline constexpr unsigned kAutomaticCheckpointRetries = 6;

enum class ShutdownCheckpointOutcome { Saved, NothingToSave, Refused };

// How a graceful stop accounts one session. A session whose newest stored response is already on
// disk loses nothing, even if the engine has since evicted its continuation. A session whose
// newest response is not on disk and whose continuation cannot be exported - including one the
// engine no longer indexes - has lost that state.
[[nodiscard]] inline ShutdownCheckpointOutcome
classify_shutdown_checkpoint(bool newest_response_on_disk, bool saved,
                             const runtime::SessionCheckpointSkipDetail& skip) noexcept {
    using Reason = runtime::SessionCheckpointSkipReason;
    if (saved) { return ShutdownCheckpointOutcome::Saved; }
    if (newest_response_on_disk) { return ShutdownCheckpointOutcome::NothingToSave; }
    switch (effective_checkpoint_skip_reason(skip)) {
    case Reason::None:
    case Reason::StoreDisabled:
    case Reason::NoSessionRecords:
    case Reason::CacheDisabled:
        return ShutdownCheckpointOutcome::NothingToSave;
    default:
        return ShutdownCheckpointOutcome::Refused;
    }
}

} // namespace ninfer::serve
