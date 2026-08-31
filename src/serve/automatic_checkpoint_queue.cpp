#include "serve/automatic_checkpoint_queue.h"

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
        // Dedup before the capacity gate: a session already queued (its save not
        // yet started) coalesces truthfully even when the queue is full, because
        // that queued save snapshots state at execution time and will include
        // this turn (review CR-20260830 GrokR2).
        if (pending_.contains(session_digest)) {
            return AutomaticCheckpointEnqueueResult::Coalesced;
        }
        if (queue_.size() >= max_pending_) {
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

void AutomaticCheckpointQueue::drain() noexcept {
    {
        std::lock_guard lock(mutex_);
        accepting_ = false;
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
            cv_.wait(lock, stop, [&] { return !queue_.empty(); });
            if (queue_.empty()) {
                if (stop.stop_requested()) { return; }
                continue;
            }
            session = std::move(queue_.front());
            queue_.pop_front();
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

} // namespace ninfer::serve
