#pragma once

#include "serve/checkpoint_policy.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <unordered_map>

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
    // Waiting is not a retry. The coordinator's predicate chooses cause-appropriate progress or
    // a wall-clock fallback. Poll every 25 ms; shutdown drops deferred work, not inventing progress.
    [[nodiscard]] bool defer(std::string session_digest, std::function<bool()> ready) noexcept;
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
    std::unordered_map<std::string, std::function<bool()>> deferred_;
    bool accepting_ = true;
    std::jthread thread_;
};

struct CheckpointSaveProgress {
    std::uint64_t engine = 0;
    std::uint64_t publication = 0;
    std::uint64_t storage = 0;
};

enum class PressureAbandonLimit : std::uint8_t { Budget, Window, DeferralRefused };

[[nodiscard]] constexpr std::string_view pressure_abandon_limit_name(PressureAbandonLimit limit) noexcept {
    switch (limit) {
    case PressureAbandonLimit::Budget: return "budget";
    case PressureAbandonLimit::Window: return "window";
    case PressureAbandonLimit::DeferralRefused: return "deferral_refused";
    }
    return "deferral_refused";
}

struct PressureAbandonment {
    runtime::DurableReason reason = runtime::DurableReason::None;
    PressureAbandonLimit limit = PressureAbandonLimit::Budget;
    std::uint32_t attempts = 0;
    std::uint64_t elapsed_ms = 0;
    std::uint64_t total = 0;
};

struct AutomaticCheckpointResult {
    std::string tag;
    runtime::DurableReason reason = runtime::DurableReason::None;
    bool awaiting_publication = false;
    // Native capacity behind an in-flight pin; the public reason and receipts stay capacity.
    bool busy = false;
};

// The production pressure callback only consults bounded bookkeeping and queues work. In
// particular, it never acquires Responses/catalog locks or waits for an Engine command.
// Persistent pressure refusals settle in bounded time, after their abandonment receipt.
// Saved answers survive partial plans; stopping reports retained non-None answers as Pending
// for synchronous final save rather than authorizing release.
class AutomaticCheckpointCoordinator final : public runtime::PressureCheckpointHandler {
public:
    using Save = std::function<AutomaticCheckpointResult(
        std::string_view, std::optional<std::string_view>)>;
    using Progress = std::function<CheckpointSaveProgress()>;
    using Abandoned = std::function<void(const PressureAbandonment&)>;
    AutomaticCheckpointCoordinator(Save save, Progress progress, std::function<void()> notify,
                                   Abandoned abandoned = {}, CheckpointRetryTiming timing = {});
    ~AutomaticCheckpointCoordinator();
    [[nodiscard]] AutomaticCheckpointEnqueueResult enqueue(
        std::string session, bool only_if_pending = false) noexcept;
    void save_before_eviction(std::span<runtime::PressureCheckpointVictim> victims) noexcept override;
    void drain() noexcept;

private:
    struct Entry {
        std::string tag;
        std::uint64_t revision = 0;
        runtime::DurableReason reason = runtime::DurableReason::None;
        bool completed = false;
        bool settled = false;
        bool pressure = false;
        std::uint32_t attempts = 0;
        std::chrono::steady_clock::time_point pressed_at{};
        std::optional<std::chrono::steady_clock::time_point> window_from;
    };
    static constexpr std::size_t kMaxEntries = 64;
    [[nodiscard]] bool make_room_locked() noexcept;
    void run(std::string_view session) noexcept;
    Save save_;
    Progress progress_;
    std::function<void()> notify_;
    Abandoned abandoned_;
    const CheckpointRetryTiming timing_;
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    TransientRetryBudget retries_;
    std::uint64_t next_revision_ = 1;
    std::uint64_t abandoned_total_ = 0;
    bool draining_ = false;
    AutomaticCheckpointQueue queue_;
};

} // namespace ninfer::serve
