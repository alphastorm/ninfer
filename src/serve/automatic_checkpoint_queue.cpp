#include "serve/automatic_checkpoint_queue.h"

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
    Save save, Progress progress, std::function<void()> notify)
    : save_(std::move(save)), progress_(std::move(progress)), notify_(std::move(notify)),
      queue_([this](std::string_view session) { run(session); }) {}

AutomaticCheckpointCoordinator::~AutomaticCheckpointCoordinator() { drain(); }
void AutomaticCheckpointCoordinator::drain() noexcept { queue_.drain(); }

AutomaticCheckpointEnqueueResult AutomaticCheckpointCoordinator::enqueue(
    std::string session, bool only_if_pending) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (only_if_pending && !entries_.contains(session)) { return AutomaticCheckpointEnqueueResult::Dropped; }
        if (!entries_.contains(session) && entries_.size() >= 64) {
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
        try {
            std::lock_guard lock(mutex_);
            auto found = entries_.find(victim.session_sha256);
            if (found != entries_.end() && found->second.tag == victim.checkpoint_tag) {
                if (found->second.completed) {
                    victim.reason = found->second.reason;
                    if (found->second.settled) {
                        victim.outcome = runtime::PressureCheckpointOutcome::Settled;
                        entries_.erase(found);
                    }
                }
                continue;
            }
            if (found != entries_.end() && found->second.tag.empty()) { continue; }
            if (found == entries_.end() && entries_.size() >= 64) { continue; }
            retries_.settle(victim.session_sha256);
            entries_[victim.session_sha256] = {
                .tag = victim.checkpoint_tag, .revision = next_revision_++, .pressure = true};
            if (queue_.enqueue(victim.session_sha256) == AutomaticCheckpointEnqueueResult::Dropped) {
                entries_.erase(victim.session_sha256);
            }
        } catch (...) {}
    }
}

void AutomaticCheckpointCoordinator::run(std::string_view session) noexcept {
    std::string key;
    Entry attempt;
    CheckpointSaveProgress before;
    AutomaticCheckpointResult result;
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
            if (entry.tag.empty()) { entry.tag = std::move(result.tag); }
            entry.completed = true;
            entry.reason = result.reason;
            runtime::SessionCheckpointSkipDetail skip;
            skip.durable_reason = result.reason;
            const bool transient = checkpoint_refusal_is_transient(skip);
            const bool retryable = transient || result.reason == runtime::DurableReason::IoUnavailable ||
                                   result.reason == runtime::DurableReason::Capacity;
            entry.settled = !retryable;
            bool deferred = false;
            if (retryable && (result.awaiting_publication || retries_.retry(key))) {
                const auto reason = result.reason;
                deferred = queue_.defer(key, [this, before, reason, publication = result.awaiting_publication] {
                    const auto now = progress_();
                    if (publication) { return now.publication != before.publication; }
                    if (reason == runtime::DurableReason::IoUnavailable) {
                        return now.storage != before.storage;
                    }
                    if (reason == runtime::DurableReason::Capacity) { return now.engine != before.engine; }
                    return now.engine != before.engine || now.publication != before.publication;
                });
                if (!deferred) { retries_.settle(key); }
            } else {
                retries_.settle(key);
            }
            // Ordinary completed saves need no pressure cache. A subsequent pressure request
            // can inspect coverage on the checkpoint thread. Exhausted retryable pressure
            // failures remain Pending, never become permission to discard resident state.
            if (!entry.pressure && !deferred) { entries_.erase(found); }
        }
    } catch (...) {}
    // Even a retryable refusal completes this task and releases the Engine Pending latch.
    // This notification is deliberately excluded from the observed Engine progress clock.
    try { notify_(); } catch (...) {}
}

} // namespace ninfer::serve
