#include "serve/automatic_checkpoint_queue.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace ninfer::serve {

AutomaticCheckpointQueue::AutomaticCheckpointQueue(Save save, std::size_t max_pending)
    : save_(std::move(save)), max_pending_(max_pending) {
    if (!save_ || max_pending_ == 0) {
        throw std::invalid_argument("automatic checkpoint queue requires a callback and capacity");
    }
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

AutomaticCheckpointQueue::~AutomaticCheckpointQueue() { drain(); }

AutomaticCheckpointEnqueueResult
AutomaticCheckpointQueue::enqueue(std::string session_digest) noexcept {
    if (session_digest.empty()) { return AutomaticCheckpointEnqueueResult::Dropped; }
    try {
        std::lock_guard lock(mutex_);
        if (!accepting_) { return AutomaticCheckpointEnqueueResult::Dropped; }
        deferred_.erase(session_digest);
        // Dedup before the capacity gate: a session already queued (its save not
        // yet started) coalesces truthfully even when the queue is full, because
        // that queued save snapshots state at execution time and will include
        // this turn (review CR-20260830 GrokR2).
        if (pending_.contains(session_digest)) {
            return AutomaticCheckpointEnqueueResult::Coalesced;
        }
        if (queue_.size() + deferred_.size() >= max_pending_) {
            return AutomaticCheckpointEnqueueResult::Dropped;
        }
        const auto [pending, inserted] = pending_.insert(session_digest);
        if (!inserted) { return AutomaticCheckpointEnqueueResult::Coalesced; }
        try {
            queue_.push_back(std::move(session_digest));
        } catch (...) {
            pending_.erase(pending);
            throw;
        }
        cv_.notify_one();
        return AutomaticCheckpointEnqueueResult::Enqueued;
    } catch (...) { return AutomaticCheckpointEnqueueResult::Dropped; }
}

bool AutomaticCheckpointQueue::defer(std::string session, std::function<bool()> ready) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (!accepting_ || !ready || pending_.contains(session)) { return false; }
        if (!deferred_.contains(session) && deferred_.size() + queue_.size() >= max_pending_) {
            return false;
        }
        deferred_.insert_or_assign(std::move(session), std::move(ready));
        cv_.notify_one();
        return true;
    } catch (...) { return false; }
}

void AutomaticCheckpointQueue::drain() noexcept {
    {
        std::lock_guard lock(mutex_);
        accepting_ = false;
        deferred_.clear();
    }
    thread_.request_stop();
    cv_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        try {
            thread_.join();
        } catch (...) {}
    }
}

void AutomaticCheckpointQueue::run(std::stop_token stop) noexcept {
    for (;;) {
        std::string session;
        {
            std::unique_lock lock(mutex_);
            if (deferred_.empty()) {
                cv_.wait(lock, stop, [&] { return !queue_.empty() || !deferred_.empty(); });
            } else {
                cv_.wait_for(lock, stop, std::chrono::milliseconds(25),
                             [&] { return !queue_.empty(); });
            }
            if (queue_.empty() && !stop.stop_requested()) {
                for (auto at = deferred_.begin(); at != deferred_.end(); ++at) {
                    bool ready = false;
                    try { ready = at->second(); } catch (...) {}
                    if (ready) {
                        session = at->first;
                        deferred_.erase(at);
                        break;
                    }
                }
            }
            if (queue_.empty() && session.empty()) {
                if (stop.stop_requested()) { return; }
                continue;
            }
            if (session.empty()) {
                session = std::move(queue_.front());
                queue_.pop_front();
            }
            // Release the coalescing key before running the save: a turn that
            // completes while this save executes must re-enqueue rather than be
            // coalesced into a snapshot that predates it (review CR-20260830 GrokR2).
            pending_.erase(session);
        }
        try {
            save_(session);
        } catch (...) {}
        {
            std::lock_guard lock(mutex_);
            if (stop.stop_requested() && queue_.empty()) {
                cv_.notify_all();
                return;
            }
        }
    }
}

AutomaticCheckpointCoordinator::AutomaticCheckpointCoordinator(
    Save save, Progress progress, std::function<void()> notify,
    Abandoned abandoned, CheckpointRetryTiming timing)
    : save_(std::move(save)), progress_(std::move(progress)), notify_(std::move(notify)),
      abandoned_(std::move(abandoned)), timing_(timing),
      queue_([this](std::string_view session) { run(session); }, kMaxEntries) {}

AutomaticCheckpointCoordinator::~AutomaticCheckpointCoordinator() { drain(); }
void AutomaticCheckpointCoordinator::drain() noexcept {
    {
        std::lock_guard lock(mutex_);
        draining_ = true;
    }
    queue_.drain();
}

bool AutomaticCheckpointCoordinator::make_room_locked() noexcept {
    const auto now = std::chrono::steady_clock::now();
    for (auto at = entries_.begin(); at != entries_.end();) {
        const auto& entry = at->second;
        // Keep fresh settlements until the Engine reads them. Unread answers may age out
        // after twice the pressure window when their original admission no longer asks.
        if (entry.completed && entry.settled &&
            (entry.answer_read || now - entry.settled_at > 2 * timing_.pressure_window)) {
            at = entries_.erase(at);
        }
        else { ++at; }
    }
    return entries_.size() < kMaxEntries;
}

AutomaticCheckpointEnqueueResult AutomaticCheckpointCoordinator::enqueue(
    std::string session, bool only_if_pending) noexcept {
    try {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(session);
        if (only_if_pending && (found == entries_.end() ||
                                (found->second.completed && found->second.settled))) {
            return AutomaticCheckpointEnqueueResult::Dropped;
        }
        if (found == entries_.end() && entries_.size() >= kMaxEntries && !make_room_locked()) {
            return AutomaticCheckpointEnqueueResult::Dropped;
        }
        retries_.settle(session);
        entries_[session] = {.revision = next_revision_++};
        const auto result = queue_.enqueue(session);
        if (result == AutomaticCheckpointEnqueueResult::Dropped) { entries_.erase(session); }
        return result;
    } catch (...) { return AutomaticCheckpointEnqueueResult::Dropped; }
}

void AutomaticCheckpointCoordinator::save_before_eviction(
    std::span<runtime::PressureCheckpointVictim> victims) noexcept {
    for (auto& victim : victims) {
        victim.outcome = runtime::PressureCheckpointOutcome::Pending;
        victim.reason = runtime::DurableReason::None;
    }
    try {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        for (auto& victim : victims) {
            try {
                auto found = entries_.find(victim.session_sha256);
                if (found != entries_.end() &&
                    (found->second.tag == victim.checkpoint_tag || found->second.tag.empty())) {
                    auto& entry = found->second;
                    if (!entry.pressure) {
                        entry.pressure = true;
                        entry.pressed_at = now;
                        entry.window_from.reset();
                    }
                    // An automatic first run may still be queued or in flight with no tag.
                    // Promotion applies now; its result is not yet an answer to this victim.
                    if (entry.tag.empty()) { continue; }
                    if (entry.completed) {
                        victim.reason = entry.reason;
                        if (entry.settled &&
                            (!draining_ || entry.reason == runtime::DurableReason::None)) {
                            victim.outcome = runtime::PressureCheckpointOutcome::Settled;
                            entry.answer_read = true;
                        }
                    }
                    continue;
                }
                if (found == entries_.end() && entries_.size() >= kMaxEntries && !make_room_locked()) {
                    continue;
                }
                retries_.settle(victim.session_sha256);
                entries_[victim.session_sha256] = {
                    .tag = victim.checkpoint_tag, .revision = next_revision_++, .pressure = true,
                    .pressed_at = now};
                if (queue_.enqueue(victim.session_sha256) == AutomaticCheckpointEnqueueResult::Dropped) {
                    entries_.erase(victim.session_sha256);
                }
            } catch (...) {}
        }
        // A Pending peer makes the Engine discard the whole plan. Consume saved answers
        // only after answering every victim, so the next plan can read all of them together.
        // Non-None answers retain their turn/tag lifetime and the bounded table pruning rule.
        if (std::all_of(victims.begin(), victims.end(), [](const auto& victim) {
                return victim.outcome == runtime::PressureCheckpointOutcome::Settled;
            })) {
            for (const auto& victim : victims) {
                if (victim.reason != runtime::DurableReason::None) { continue; }
                const auto found = entries_.find(victim.session_sha256);
                if (found != entries_.end() && found->second.tag == victim.checkpoint_tag &&
                    found->second.settled && found->second.reason == runtime::DurableReason::None) {
                    entries_.erase(found);
                }
            }
        }
    } catch (...) {}
}

void AutomaticCheckpointCoordinator::run(std::string_view session) noexcept {
    std::string key;
    Entry attempt;
    CheckpointSaveProgress before;
    AutomaticCheckpointResult result;
    std::optional<PressureAbandonment> abandonment;
    try {
        key.assign(session);
        {
            std::lock_guard lock(mutex_);
            const auto found = entries_.find(key);
            if (found == entries_.end()) { return; }
            attempt = found->second;
        }
        before = progress_();
        result = save_(session, attempt.tag.empty() ? std::nullopt
                         : std::optional<std::string_view>(attempt.tag));
    } catch (...) {
        result.tag = attempt.tag;
        result.reason = runtime::DurableReason::IoUnavailable;
    }
    try {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(key);
        if (found != entries_.end() && found->second.revision == attempt.revision) {
            auto& entry = found->second;
            const auto now = std::chrono::steady_clock::now();
            if (entry.tag.empty()) { entry.tag = std::move(result.tag); }
            entry.completed = true;
            entry.reason = result.reason;
            ++entry.attempts;
            runtime::SessionCheckpointSkipDetail skip;
            skip.durable_reason = result.reason;
            const bool transient = checkpoint_refusal_is_transient(skip);
            const bool retryable = transient || result.reason == runtime::DurableReason::IoUnavailable ||
                                   result.reason == runtime::DurableReason::Capacity;
            entry.settled = !retryable;
            if (entry.settled) {
                entry.settled_at = now;
                entry.answer_read = false;
            }
            // Publication waits stay progress-only. Busy pressure refusals have no budget or
            // window, reset any persistent window, and keep their spacing. Other retryable
            // pressure refusals use the unchanged budget and a consecutive-persistent window.
            const bool publication = result.awaiting_publication;
            const bool busy = entry.pressure &&
                (result.reason == runtime::DurableReason::TransactionBusy ||
                 (result.reason == runtime::DurableReason::Capacity && result.busy));
            const bool persistent = retryable && !publication && !busy;
            if (entry.pressure && (busy || publication)) { entry.window_from.reset(); }
            if (entry.pressure && persistent && !entry.window_from) { entry.window_from = now; }
            const bool window_spent = entry.pressure && persistent &&
                now - *entry.window_from >= timing_.pressure_window;
            std::optional<PressureAbandonLimit> limit;
            bool deferred = false;
            if (retryable) {
                if (window_spent) {
                    limit = PressureAbandonLimit::Window;
                } else if (!publication && !busy && !retries_.retry(key)) {
                    limit = PressureAbandonLimit::Budget;
                } else {
                    const auto reason = result.reason;
                    const auto not_before = busy ? now + kBusyPressureRetrySpacing
                                                 : std::chrono::steady_clock::time_point{};
                    auto fire_at = publication ? std::chrono::steady_clock::time_point::max()
                                               : now + timing_.fallback;
                    if (entry.pressure && persistent) {
                        fire_at = std::min(fire_at, *entry.window_from + timing_.pressure_window);
                    }
                    std::function<bool()> ready;
                    try {
                        ready = [this, before, reason, not_before, fire_at, publication] {
                            const auto tick = std::chrono::steady_clock::now();
                            if (tick < not_before) { return false; }
                            if (tick >= fire_at) { return true; }
                            const auto progress = progress_();
                            if (publication) { return progress.publication != before.publication; }
                            if (reason == runtime::DurableReason::IoUnavailable) {
                                return progress.storage != before.storage;
                            }
                            if (reason == runtime::DurableReason::Capacity) {
                                return progress.engine != before.engine;
                            }
                            return progress.engine != before.engine || progress.publication != before.publication;
                        };
                    } catch (...) {}
                    deferred = queue_.defer(key, std::move(ready));
                    if (!deferred) { limit = PressureAbandonLimit::DeferralRefused; }
                }
            }
            if (!deferred) { retries_.settle(key); }
            if (entry.pressure && limit && !draining_) {
                abandonment = PressureAbandonment{result.reason, *limit, entry.attempts,
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - entry.pressed_at).count()), ++abandoned_total_};
            }
            // Ordinary saves need no pressure cache. Pressure answers retain their turn/tag:
            // None survives Pending peers; full-table pruning reclaims read or aged settlements.
            // During stop, retained non-None answers report Pending for synchronous final save.
            if (!entry.pressure && !deferred) { entries_.erase(found); }
        }
    } catch (...) {}
    // The best-effort receipt precedes any Engine-visible abandonment, not just its wakeup.
    // No lock is held across the callback; a new revision supersedes this answer without
    // another receipt, and stop keeps it Pending for synchronous final save.
    if (abandonment) {
        if (abandoned_) {
            try { abandoned_(*abandonment); } catch (...) {}
        }
        try {
            std::lock_guard lock(mutex_);
            const auto found = entries_.find(key);
            if (found != entries_.end() && found->second.revision == attempt.revision && !draining_) {
                found->second.settled = true;
                found->second.settled_at = std::chrono::steady_clock::now();
                found->second.answer_read = false;
            }
        } catch (...) {}
    }
    // Even a retryable refusal completes this task and releases the Engine Pending latch.
    // This notification is deliberately excluded from the observed Engine progress clock.
    try { notify_(); } catch (...) {}
}

} // namespace ninfer::serve
