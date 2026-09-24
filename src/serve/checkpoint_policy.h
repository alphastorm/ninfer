#pragma once

#include "runtime/contract/continuation_checkpoint.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

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

// Retries a save gets per completed turn before a transient refusal counts as final: an automatic
// save requeues, and save-before-evict keeps its victim resident for another admission pass.
// Each retry waits for the engine to quiesce again, so this bounds work, not latency.
inline constexpr unsigned kTransientCheckpointRetries = 6;

// Transient-refusal retries of automatic saves, per session. A session holds an entry only while
// a retry of its save is queued - every final outcome settles it - so session churn cannot grow
// the table past what the automatic queue itself holds.
class TransientRetryBudget {
public:
    // Counts one transient refusal of the session's save. False once every retry is spent, which
    // settles the session.
    [[nodiscard]] bool retry(std::string_view session_sha256) {
        std::lock_guard lock(mutex_);
        auto found = attempts_.find(session_sha256);
        if (found == attempts_.end()) {
            found = attempts_.emplace(std::string(session_sha256), 0U).first;
        }
        if (found->second >= kTransientCheckpointRetries) {
            attempts_.erase(found);
            return false;
        }
        ++found->second;
        return true;
    }

    // The session's save reached a final outcome, or a new turn restarts its retries.
    void settle(std::string_view session_sha256) noexcept {
        std::lock_guard lock(mutex_);
        if (const auto found = attempts_.find(session_sha256); found != attempts_.end()) {
            attempts_.erase(found);
        }
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return attempts_.size();
    }

private:
    struct Hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view text) const noexcept {
            return std::hash<std::string_view>{}(text);
        }
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, unsigned, Hash, std::equal_to<>> attempts_;
};

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
