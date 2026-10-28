#pragma once

#include "runtime/contract/continuation_checkpoint.h"

#include <atomic>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ninfer::runtime {

// This gate is independent of Program execution. A disk publisher may hold it only for
// current-pointer replacement and sync; it must never wait for a worker operation here.
// A session's gate lives while a request, an in-flight operation or a captured epoch token holds
// it, or while a native owner still names the session. It is retired only after all of those
// end, so a later lookup never splits that session's serialization across two gates.
class CheckpointControl {
public:
    // Smallest table that is swept. A sweep runs once the table has doubled since the previous
    // one, which keeps retirement amortized O(1) per created gate.
    static constexpr std::size_t kGateSweepFloor = 64;

    struct Session {
        std::mutex mutex;
        DurableSessionEpoch epoch;
        std::string tag;
        std::uint64_t committed_order = 0;

        // Submission invalidates captured work, but is not an acknowledged endpoint.
        void submitted(std::uint64_t order) noexcept {
            epoch.value = std::max(epoch.value + 1, order);
        }
        void stored(std::string value, std::uint64_t order) {
            if (order <= committed_order) { return; }
            tag = std::move(value);
            committed_order = order;
            ++epoch.value;
        }
        // No submission, acknowledgement or import has reached this gate: it holds nothing an
        // absent session lacks, and no native owner can name its session.
        [[nodiscard]] bool pristine() const noexcept {
            return epoch.value == 0 && tag.empty() && committed_order == 0;
        }
    };

    [[nodiscard]] std::shared_ptr<Session> session(std::string_view digest) {
        std::lock_guard lock(mutex_);
        if (const auto entry = sessions_.find(digest); entry != sessions_.end()) {
            return entry->second;
        }
        // Any thread may retire idle pristine gates, such as those of missing restore lookups.
        if (sessions_.size() >= pristine_sweep_at_) {
            sweep_locked(pristine_sweep_at_, [](std::string_view, const Session& state) {
                return !state.pristine();
            });
        }
        auto state = std::make_shared<Session>();
        sessions_.emplace(std::string(digest), state);
        return state;
    }

    // The returned token retains this gate until its last copy is destroyed.
    [[nodiscard]] DurableSessionEpoch epoch(std::string_view digest) {
        auto state = session(digest);
        std::lock_guard lock(state->mutex);
        return {state->epoch.value, state};
    }

    // Worker-only, once per worker turn. Once the table has doubled since its previous sweep,
    // retires every gate that nothing holds and whose session `owned` reports no native owner
    // still names. Retained committed tags and epochs matter only to such owners.
    template <class Owned>
    void retire_unowned(const Owned& owned) {
        std::lock_guard lock(mutex_);
        if (sessions_.size() < owned_sweep_at_) { return; }
        sweep_locked(owned_sweep_at_, [&](std::string_view digest, const Session&) {
            return owned(digest);
        });
    }

    [[nodiscard]] std::size_t gate_count() const {
        std::lock_guard lock(mutex_);
        return sessions_.size();
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
        state->committed_order = fresh_order;
        state->submitted(fresh_order);
        return true;
    }

    void set_handler(std::shared_ptr<PressureCheckpointHandler> value) noexcept {
        handler_.store(std::move(value));
        notify_progress();
    }
    void set_oracle(std::shared_ptr<const ReclaimableSessionOracle> value) noexcept {
        oracle_.store(std::move(value));
        notify_progress();
    }
    [[nodiscard]] std::shared_ptr<const ReclaimableSessionOracle> oracle() const noexcept {
        return oracle_.load();
    }
    // Snapshot ownership before calling out: removing a provider from inside its callback is
    // legal and cannot destroy that callback's receiver while it is on the stack.
    void notify_progress(bool observed = true) noexcept {
        progress_.fetch_add(1);
        if (observed) { observed_progress_.fetch_add(1); }
    }
    [[nodiscard]] std::uint64_t observed_progress() const noexcept {
        return observed_progress_.load();
    }
    // Installed once by Engine. Invocation and pin retirement are worker-affine.
    void set_validator(std::function<DurableReason(const ContinuationCheckpointReader&)> validate) {
        validate_ = std::move(validate);
    }
    void retire_release_pins() { release_pins_.clear(); }
    // The pending snapshot is worker-owned. Only progress_ is touched by provider threads.
    void begin_turn() noexcept {
        if (pending_progress_ && *pending_progress_ != progress_.load()) {
            pending_progress_.reset();
        }
    }
    [[nodiscard]] bool pending() const noexcept { return pending_progress_.has_value(); }
    [[nodiscard]] bool settle(std::span<PressureCheckpointVictim> victims) noexcept {
        begin_turn();
        if (pending()) { return false; }
        const auto progress = progress_.load();
        const auto handler = handler_.load();
        if (!handler || victims.empty()) { return true; }
        handler->save_before_eviction(victims);
        for (auto& victim : victims) {
            if (victim.outcome == PressureCheckpointOutcome::Settled &&
                victim.reason == DurableReason::None && validate_) {
                victim.reason = prove_recoverable(victim);
                if (victim.reason != DurableReason::None) {
                    victim.outcome = PressureCheckpointOutcome::Pending;
                }
            }
            if (victim.outcome == PressureCheckpointOutcome::Pending) {
                // Preserve this attempt's classification even when its callback itself signals
                // progress. The next attempt/worker turn may retry; this one must not fail as
                // native capacity impossibility or consume a different destructive plan.
                pending_progress_ = progress;
                return false;
            }
        }
        pending_progress_.reset();
        return true;
    }

private:
    DurableReason prove_recoverable(const PressureCheckpointVictim& victim) noexcept {
        try {
            const auto provider = oracle();
            if (!provider) { return DurableReason::SessionNotIndexed; }
            auto pin = provider->pin_recoverable(victim.session_sha256, victim.checkpoint_tag);
            if (!pin) { return DurableReason::SessionNotIndexed; }
            auto verification = pin->fresh_reader();
            if (!verification) { return DurableReason::IoUnavailable; }
            const auto reason = validate_(*verification);
            if (reason != DurableReason::None) { return reason; }
            auto current = provider->pin_recoverable(victim.session_sha256, victim.checkpoint_tag);
            if (!current) { return DurableReason::StalePublication; }
            const auto verified_index = pin->file_descriptor("engine/index.cbor");
            const auto current_index = current->file_descriptor("engine/index.cbor");
            if (!verified_index || !current_index ||
                verified_index->bytes != current_index->bytes ||
                verified_index->sha256 != current_index->sha256) {
                return DurableReason::StalePublication;
            }
            release_pins_.push_back(std::move(pin));
            release_pins_.push_back(std::move(current));
            return DurableReason::None;
        } catch (...) { return DurableReason::IoUnavailable; }
    }
    // A gate whose table entry is its only owner has no holder that could lock it, recheck its
    // epoch or publish under it. The lock only orders this read after the last holder's writes.
    template <class Retain>
    void sweep_locked(std::size_t& sweep_at, const Retain& retain) {
        std::erase_if(sessions_, [&](const auto& entry) {
            if (entry.second.use_count() != 1) { return false; }
            std::lock_guard gate(entry.second->mutex);
            return !retain(std::string_view(entry.first), *entry.second);
        });
        sweep_at = std::max(kGateSweepFloor, 2 * sessions_.size());
    }
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Session>, std::less<>> sessions_;
    std::size_t pristine_sweep_at_ = kGateSweepFloor;
    std::size_t owned_sweep_at_ = kGateSweepFloor;
    std::atomic<std::shared_ptr<PressureCheckpointHandler>> handler_;
    std::atomic<std::shared_ptr<const ReclaimableSessionOracle>> oracle_;
    std::atomic<std::uint64_t> progress_{1};
    std::atomic<std::uint64_t> observed_progress_{1};
    std::optional<std::uint64_t> pending_progress_;
    std::function<DurableReason(const ContinuationCheckpointReader&)> validate_;
    std::vector<std::shared_ptr<const ContinuationCheckpointReader>> release_pins_;
};

struct PreparedEngineCheckpoint {
    DurablePrepareState state = DurablePrepareState::PolicyDisabled;
    DurableReason reason = DurableReason::CacheDisabled;
    std::function<DurableImportResult()> commit;
    std::function<void()> abort;
};

} // namespace ninfer::runtime
