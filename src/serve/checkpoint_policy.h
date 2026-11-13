#pragma once

#include "runtime/contract/continuation_checkpoint.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ninfer::serve {

// K7 is authoritative for retry policy. Legacy engine/Program diagnostics are log
// detail only: a coarse ProgramRejected must not erase or reinterpret the actual outcome.
[[nodiscard]] inline bool
checkpoint_refusal_is_transient(const runtime::SessionCheckpointSkipDetail& skip) noexcept {
    using Reason = runtime::DurableReason;
    switch (skip.durable_reason) {
    case Reason::TransactionBusy:
    case Reason::TagMismatch:
    case Reason::StalePublication:
    case Reason::StaleRevision:
        return true;
    default:
        return false;
    }
}

// Retries per completed turn, waiting for matching progress or kCheckpointRetryFallback.
// A spent budget drops an automatic save until the next turn and settles a pressure save with
// its reason, permitting release without a durable copy. Busy pressure refusals (a transaction,
// a response still publishing, or capacity behind a pinned image) spend none: their blocker ends
// by itself.
inline constexpr unsigned kTransientCheckpointRetries = 6;

// Busy pressure saves are at least this far apart. A transaction, publication or State image
// pinned by another export can outlast many Engine progress ticks; cap attempts at ten per second.
inline constexpr std::chrono::milliseconds kBusyPressureRetrySpacing{100};

// Every deferral except an HTTP-publication wait fires by this deadline without matching progress.
inline constexpr std::chrono::milliseconds kCheckpointRetryFallback{1000};
// From the first persistent pressure refusal; keep well inside the smallest shipped pending timeout.
inline constexpr std::chrono::milliseconds kPressureRetryWindow{10000};

struct CheckpointRetryTiming {
    std::chrono::milliseconds fallback = kCheckpointRetryFallback;
    std::chrono::milliseconds pressure_window = kPressureRetryWindow;
};

[[nodiscard]] inline bool capacity_refusal_is_busy(
    runtime::DurableReason reason, runtime::DurableHostShortage shortage) noexcept {
    return reason == runtime::DurableReason::Capacity && shortage == runtime::DurableHostShortage::Busy;
}

// Whether a completed turn is worth an automatic checkpoint. A session below the configured minimum
// is cheaper to prefill again than to write out after every turn, so automatic saves skip it; a
// graceful stop and save-before-eviction still save it, which keeps its responses resumable across
// a restart. A session that already has a generation on disk keeps it current at any size: after a
// compaction its new lineage starts below the minimum, and a stale generation restores a prefix
// the next request no longer shares.
[[nodiscard]] inline bool checkpoint_worth_saving(std::uint64_t frontier_tokens,
                                                  std::uint64_t minimum_tokens,
                                                  bool generation_on_disk) noexcept {
    return frontier_tokens >= minimum_tokens || generation_on_disk;
}

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
    using Reason = runtime::DurableReason;
    if (saved) { return ShutdownCheckpointOutcome::Saved; }
    if (newest_response_on_disk) { return ShutdownCheckpointOutcome::NothingToSave; }
    switch (skip.durable_reason) {
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
