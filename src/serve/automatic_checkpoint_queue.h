#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

namespace ninfer::serve {

enum class AutomaticCheckpointEnqueueResult {
    Enqueued,
    Coalesced,
    Dropped,
};

// One bounded best-effort worker for automatic checkpoint saves. Explicit POST and shutdown saves
// remain synchronous acceptance boundaries; request completion only enqueues here.
class AutomaticCheckpointQueue {
public:
    using Save = std::function<void(std::string_view)>;

    explicit AutomaticCheckpointQueue(Save save, std::size_t max_pending = 64);
    ~AutomaticCheckpointQueue();

    AutomaticCheckpointQueue(const AutomaticCheckpointQueue&)            = delete;
    AutomaticCheckpointQueue& operator=(const AutomaticCheckpointQueue&) = delete;
    AutomaticCheckpointQueue(AutomaticCheckpointQueue&&)                 = delete;
    AutomaticCheckpointQueue& operator=(AutomaticCheckpointQueue&&)      = delete;

    [[nodiscard]] AutomaticCheckpointEnqueueResult enqueue(std::string session_digest) noexcept;
    void drain() noexcept;

private:
    void run(std::stop_token stop) noexcept;

    Save save_;
    const std::size_t max_pending_;
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<std::string> queue_;
    // Sessions queued but not yet running. A running save has released its key, so a turn that
    // completes meanwhile - or a retry the save itself requests - queues a fresh save.
    std::unordered_set<std::string> pending_;
    bool accepting_ = true;
    std::jthread thread_;
};

} // namespace ninfer::serve
