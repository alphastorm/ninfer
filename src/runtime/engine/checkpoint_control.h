#pragma once

#include "runtime/contract/continuation_checkpoint.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace ninfer::runtime {

// This gate is independent of Program execution. A disk publisher may hold it only for
// current-pointer replacement and sync; it must never wait for a worker operation here.
class CheckpointControl {
public:
    struct Session {
        std::mutex mutex;
        DurableSessionEpoch epoch;
        std::string tag;
    };

    [[nodiscard]] std::shared_ptr<Session> session(std::string_view digest) {
        std::lock_guard lock(mutex_);
        if (const auto entry = sessions_.find(digest); entry != sessions_.end()) {
            return entry->second;
        }
        auto state = std::make_shared<Session>();
        sessions_.emplace(std::string(digest), state);
        return state;
    }

    [[nodiscard]] DurableSessionEpoch epoch(std::string_view digest) {
        const auto state = session(digest);
        std::lock_guard lock(state->mutex);
        return state->epoch;
    }

    [[nodiscard]] DurablePublicationResult publish(
        std::string_view digest, std::string_view tag, DurableSessionEpoch expected,
        const std::function<bool()>& commit) {
        if (tag.empty()) { return {DurableExportState::Refused, DurableReason::EmptyTag}; }
        const auto state = session(digest);
        std::lock_guard lock(state->mutex);
        if (state->epoch != expected || state->tag != tag) {
            return {DurableExportState::Stale, DurableReason::StalePublication};
        }
        try {
            if (commit()) { return {DurableExportState::Exported, DurableReason::None}; }
        } catch (...) {}
        return {DurableExportState::Refused, DurableReason::IoUnavailable};
    }

    template <class Commit>
    [[nodiscard]] bool commit_import(const std::shared_ptr<Session>& state,
                                     DurableSessionEpoch expected, std::string& tag,
                                     std::uint64_t fresh_order, Commit&& commit) {
        std::lock_guard lock(state->mutex);
        if (state->epoch != expected) { return false; }
        // All capacity and strings are prepared before entering this final publication.
        // The native/policy commit callback has no fallible work after native ownership.
        std::forward<Commit>(commit)();
        state->tag.swap(tag);
        state->epoch = {fresh_order};
        return true;
    }

    void set_handler(std::shared_ptr<PressureCheckpointHandler> value) noexcept {
        handler_.store(std::move(value));
    }
    void set_oracle(std::shared_ptr<const ReclaimableSessionOracle> value) noexcept {
        oracle_.store(std::move(value));
    }
    [[nodiscard]] std::shared_ptr<const ReclaimableSessionOracle> oracle() const noexcept {
        return oracle_.load();
    }
    // Snapshot ownership before calling out: removing a provider from inside its callback is
    // legal and cannot destroy that callback's receiver while it is on the stack.
    [[nodiscard]] bool settle(std::span<PressureCheckpointVictim> victims) const noexcept {
        const auto handler = handler_.load();
        if (!handler || victims.empty()) { return true; }
        handler->save_before_eviction(victims);
        for (const auto& victim : victims) {
            if (victim.outcome == PressureCheckpointOutcome::Pending) { return false; }
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Session>, std::less<>> sessions_;
    std::atomic<std::shared_ptr<PressureCheckpointHandler>> handler_;
    std::atomic<std::shared_ptr<const ReclaimableSessionOracle>> oracle_;
};

struct PreparedEngineCheckpoint {
    DurablePrepareState state = DurablePrepareState::PolicyDisabled;
    DurableReason reason = DurableReason::CacheDisabled;
    std::function<DurableImportResult()> commit;
    std::function<void()> abort;
};

} // namespace ninfer::runtime
