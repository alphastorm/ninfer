#include "serve/automatic_checkpoint_queue.h"
#include "serve/session_checkpoint_store.h"
#include "runtime/engine/checkpoint_control.h"
#include "checkpoint_write_fault.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <future>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace ninfer::serve;
using Reason = ninfer::runtime::DurableReason;
using Outcome = ninfer::runtime::PressureCheckpointOutcome;
using Victim = ninfer::runtime::PressureCheckpointVictim;
using namespace std::chrono_literals;

const CheckpointRetryTiming kNoFallback{.fallback = 1h, .pressure_window = 1h};

int check(bool value, const char* message) {
    if (value) { return 0; }
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

int queue_coalescing() {
    std::promise<void> checkpoint_started;
    std::promise<void> release_checkpoint;
    std::shared_future<void> release_future = release_checkpoint.get_future().share();
    std::mutex saved_mutex;
    std::vector<std::string> saved;
    std::atomic<bool> started_signalled{false};
    ninfer::serve::AutomaticCheckpointQueue automatic_checkpoints(
        [&](std::string_view digest) {
            if (digest == "session-a" && !started_signalled.exchange(true)) {
                checkpoint_started.set_value();
            }
            release_future.wait();
            std::lock_guard lock(saved_mutex);
            saved.emplace_back(digest);
        },
        1);

    const auto enqueue_started = std::chrono::steady_clock::now();
    const auto first           = automatic_checkpoints.enqueue("session-a");
    const auto enqueue_elapsed = std::chrono::steady_clock::now() - enqueue_started;
    if (checkpoint_started.get_future().wait_for(std::chrono::seconds(1)) !=
            std::future_status::ready ||
        enqueue_elapsed >= std::chrono::milliseconds(250)) {
        std::cerr << "automatic checkpoint save blocked request completion\n";
        return 1;
    }

    // A turn that completes while its session's save is executing must re-enqueue
    // (the pre-fix behavior coalesced it away and lost the newest state); a session
    // that is queued but not yet executing still coalesces (review CR-20260830 GrokR2).
    const auto requeued  = automatic_checkpoints.enqueue("session-a");
    const auto coalesced = automatic_checkpoints.enqueue("session-a");
    const auto dropped   = automatic_checkpoints.enqueue("session-b");
    release_checkpoint.set_value();
    automatic_checkpoints.drain();
    const auto after_drain = automatic_checkpoints.enqueue("session-after-drain");
    automatic_checkpoints.drain();
    if (first != ninfer::serve::AutomaticCheckpointEnqueueResult::Enqueued ||
        requeued != ninfer::serve::AutomaticCheckpointEnqueueResult::Enqueued ||
        coalesced != ninfer::serve::AutomaticCheckpointEnqueueResult::Coalesced ||
        dropped != ninfer::serve::AutomaticCheckpointEnqueueResult::Dropped ||
        after_drain != ninfer::serve::AutomaticCheckpointEnqueueResult::Dropped ||
        saved != std::vector<std::string>{"session-a", "session-a"}) {
        std::cerr << "automatic checkpoint queue did not requeue racing saves\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}

StoredOpenAIResponse record(std::string id, const std::string& session, std::uint64_t order) {
    StoredOpenAIResponse value;
    value.id = std::move(id);
    value.session_key = session;
    value.client_session_sha256 = session;
    value.publication_order = order;
    value.response = {{"id", value.id}, {"object", "response"}, {"status", "completed"}};
    return value;
}

int pressure_callback_does_not_wait_for_restore_commit() {
    const std::string session(64, 'a');
    OpenAIResponsesStore responses(8, 1ULL << 20);
    responses.put(record("resp_endpoint", session, 1));
    const auto snapshot = *responses.snapshot_session(session);
    auto guard = responses.begin_restore(session);
    std::promise<void> commit_queued, worker_commit;
    auto commit_ready = worker_commit.get_future();
    std::atomic<unsigned> completed{0};
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view digest, std::optional<std::string_view> tag) {
            const auto state = responses.await_publication(digest, std::string(*tag), 0ms);
            return AutomaticCheckpointResult{std::string(*tag),
                state == ResponsePublicationState::Stored ? Reason::None : Reason::RequiredResponseMissing};
        }, [] { return CheckpointSaveProgress{}; }, [&] { ++completed; });
    auto http = std::async(std::launch::async, [&] {
        return responses.restore_session(snapshot, guard, [&] {
            commit_queued.set_value(); // HTTP owns R and has queued a worker commit
            commit_ready.wait();
            return true;
        });
    });
    commit_queued.get_future().wait();
    Victim victim{session, "resp_endpoint"};
    // Run the same production handler the Engine worker uses before its next command drain.
    coordinator.save_before_eviction(std::span(&victim, 1));
    const bool pending = victim.outcome == Outcome::Pending;
    worker_commit.set_value();
    const bool restored = http.get();
    coordinator.drain();
    return check(pending && restored && completed == 1,
                 "worker pressure blocked behind R while HTTP waited for worker commit");
}

struct ProgressFixture {
    std::mutex mutex;
    std::condition_variable cv;
    CheckpointSaveProgress progress;
    unsigned calls = 0, completed = 0, probes = 0;
    Reason refusal = Reason::TransactionBusy;
    bool publishing = false;
    bool busy = false;

    bool wait(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, predicate);
    }
    CheckpointSaveProgress observe() {
        std::lock_guard lock(mutex);
        ++probes;
        cv.notify_all();
        return progress;
    }
    AutomaticCheckpointResult save(std::optional<std::string_view> tag) {
        std::lock_guard lock(mutex);
        ++calls;
        return {tag ? std::string(*tag) : "resp_retry", refusal, publishing, busy};
    }
    void notify() {
        std::lock_guard lock(mutex);
        ++completed;
        cv.notify_all();
    }
};

int automatic_retry_requires_matching_progress() {
    int failures = 0;
    for (const auto reason : {Reason::TransactionBusy, Reason::StaleRevision,
                              Reason::StalePublication, Reason::TagMismatch,
                              Reason::Capacity, Reason::IoUnavailable}) {
        ProgressFixture f;
        f.refusal = reason;
        AutomaticCheckpointCoordinator coordinator(
            [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
            [&] { return f.observe(); }, [&] { f.notify(); }, {}, kNoFallback);
        Victim victim{std::string(64, 'b'), "resp_retry"};
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(f.wait([&] { return f.completed == 1 && f.probes >= 3; }),
                          "retryable pressure refusal never completed/notified");
        {
            std::lock_guard lock(f.mutex);
            failures += check(f.calls == 1, "unchanged progress spun an automatic retry");
            // Storage progress cannot cure native capacity/races; native progress cannot cure I/O.
            if (reason == Reason::IoUnavailable) { ++f.progress.engine; }
            else { ++f.progress.storage; }
        }
        unsigned probes;
        { std::lock_guard lock(f.mutex); probes = f.probes; }
        failures += check(f.wait([&] { return f.probes >= probes + 2; }),
                          "deferred progress observation stopped");
        {
            std::lock_guard lock(f.mutex);
            failures += check(f.calls == 1, "unrelated progress retried the wrong refusal cause");
            f.refusal = Reason::None;
            if (reason == Reason::IoUnavailable) { ++f.progress.storage; }
            else { ++f.progress.engine; }
        }
        failures += check(f.wait([&] { return f.completed == 2; }),
                          "matching observed progress did not re-enqueue the refused save");
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(victim.outcome == Outcome::Settled && victim.reason == Reason::None,
                          "saved pressure task did not settle");
        coordinator.drain();
    }
    return failures;
}

// A persistent refusal ends the pressure save once its retry budget is spent. The reported
// reason permits release without claiming a durable copy, and stays stable across replanning.
int pressure_retry_exhaustion_settles_with_reason() {
    int failures = 0;
    for (const auto reason : {Reason::Capacity, Reason::IoUnavailable, Reason::StaleRevision,
                              Reason::StalePublication, Reason::TagMismatch}) {
        ProgressFixture f;
        f.refusal = reason;
        AutomaticCheckpointCoordinator coordinator(
            [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
            [&] { return f.observe(); }, [&] { f.notify(); });
        Victim victim{std::string(64, 'c'), "resp_retry"};
        coordinator.save_before_eviction(std::span(&victim, 1));
        for (unsigned count = 1; count <= kTransientCheckpointRetries + 1; ++count) {
            failures += check(f.wait([&] { return f.completed == count; }), "bounded retry did not finish");
            std::lock_guard lock(f.mutex);
            if (reason == Reason::IoUnavailable) { ++f.progress.storage; } else { ++f.progress.engine; }
        }
        failures += check(!f.wait([&] { return f.completed > kTransientCheckpointRetries + 1; }, 300ms),
                          "an exhausted retryable pressure save retried without a new turn");
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(victim.outcome == Outcome::Settled && victim.reason == reason,
                          "exhausted persistent pressure save stayed Pending");
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(victim.outcome == Outcome::Settled && victim.reason == reason,
                          "exhausted pressure answer disappeared on a second ask");
        { std::lock_guard lock(f.mutex); f.refusal = Reason::UnsupportedCodec; }
        const auto short_turn = coordinator.enqueue(victim.session_sha256, true);
        failures += check(short_turn == AutomaticCheckpointEnqueueResult::Dropped,
                          "short turn rearmed an already settled pressure answer");
        // Let the predecessor's unwanted rearm finish so the remaining assertions are independent.
        if (short_turn != AutomaticCheckpointEnqueueResult::Dropped) {
            failures += check(f.wait([&] { return f.completed >= kTransientCheckpointRetries + 2; }),
                              "unexpected short-turn rearm did not finish");
        }
        unsigned completed;
        { std::lock_guard lock(f.mutex); completed = f.completed; }
        failures += check(coordinator.enqueue(victim.session_sha256) !=
                              AutomaticCheckpointEnqueueResult::Dropped,
                          "new completed turn did not queue checkpoint work");
        failures += check(f.wait([&] { return f.completed >= completed + 1; }),
                          "new completed turn did not restart checkpoint work");
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(f.wait([&] { return f.completed >= completed + 2; }),
                          "terminal pressure refusal never completed");
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(victim.outcome == Outcome::Settled && victim.reason == Reason::UnsupportedCodec,
                          "reported nonretryable pressure refusal did not settle with its reason");
        coordinator.drain();
    }
    return failures;
}

int persistent_refusal_retries_without_progress() {
    int failures = 0;
    for (const bool automatic : {false, true}) {
        ProgressFixture f;
        f.refusal = automatic ? Reason::IoUnavailable : Reason::Capacity;
        AutomaticCheckpointCoordinator coordinator(
            [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
            [&] { return f.observe(); }, [&] { f.notify(); });
        Victim victim{std::string(64, '1'), "resp_retry"};
        if (automatic) {
            failures += check(coordinator.enqueue(victim.session_sha256) !=
                                  AutomaticCheckpointEnqueueResult::Dropped,
                              "automatic I/O fixture did not queue");
            failures += check(f.wait([&] { return f.completed >= 1; }),
                              "automatic I/O refusal did not finish");
        }
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(victim.outcome == Outcome::Pending, "retry fixture was not Pending");
        failures += check(f.wait([&] { return f.completed >= 2; }, 3s),
                          automatic ? "automatic I/O deferral never fired without storage progress"
                                    : "pressure capacity deferral never fired without engine progress");
        coordinator.drain();
    }
    return failures;
}

int settled_entries_do_not_fill_the_table() {
    ProgressFixture f;
    f.refusal = Reason::UnsupportedCodec;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
        [&] { return f.observe(); }, [&] { f.notify(); });
    const auto session = [](unsigned index) {
        constexpr std::string_view digits = "0123456789abcdef";
        return std::string(62, 'a') + digits[index / 16] + digits[index % 16];
    };
    for (unsigned index = 0; index < 64; ++index) {
        Victim victim{session(index), "resp_retry"};
        coordinator.save_before_eviction(std::span(&victim, 1));
    }
    int failures = check(f.wait([&] { return f.completed >= 64; }),
                         "settled-table fixture did not finish its first 64 saves");
    for (unsigned index = 0; index < 64; ++index) {
        Victim settled{session(index), "resp_retry"};
        coordinator.save_before_eviction(std::span(&settled, 1));
        failures += check(settled.outcome == Outcome::Settled,
                          "settled-table fixture could not read its completed answers");
    }
    Victim victim{session(64), "resp_retry"};
    coordinator.save_before_eviction(std::span(&victim, 1));
    failures += check(f.wait([&] { return f.completed >= 65; }),
                      "retained settled entries prevented a new pressure attempt");
    failures += check(coordinator.enqueue(session(65)) != AutomaticCheckpointEnqueueResult::Dropped,
                      "retained settled entries prevented a new automatic save");
    coordinator.drain();
    return failures;
}

int settled_answer_survives_a_pending_peer() {
    ProgressFixture f;
    const std::string x(64, '2'), y(64, '3');
    unsigned x_calls = 0;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view session, std::optional<std::string_view> tag) {
            std::lock_guard lock(f.mutex);
            ++f.calls;
            if (session == x) { ++x_calls; }
            return AutomaticCheckpointResult{std::string(*tag),
                session == x ? Reason::UnsupportedCodec : f.refusal};
        }, [&] { return f.observe(); }, [&] { f.notify(); });
    std::array<Victim, 2> victims{{{x, "resp_x"}, {y, "resp_y"}}};
    coordinator.save_before_eviction(victims);
    int failures = check(f.wait([&] { return f.completed >= 2; }),
                         "peer fixture did not finish both attempts");
    coordinator.save_before_eviction(victims);
    failures += check(victims[0].outcome == Outcome::Settled && victims[1].outcome == Outcome::Pending,
                      "terminal peer did not settle beside a busy peer");
    coordinator.save_before_eviction(victims);
    failures += check(victims[0].outcome == Outcome::Settled,
                      "terminal answer disappeared while its peer remained Pending");
    {
        std::lock_guard lock(f.mutex);
        failures += check(x_calls == 1, "settled peer was saved again");
        f.refusal = Reason::None;
        ++f.progress.engine;
    }
    failures += check(f.wait([&] { return f.completed >= 3; }), "released busy peer was not retried");
    coordinator.save_before_eviction(victims);
    failures += check(victims[0].outcome == Outcome::Settled && victims[1].outcome == Outcome::Settled,
                      "peer settlements never coincided");
    coordinator.drain();
    return failures;
}

int saved_answer_survives_a_pending_peer() {
    ProgressFixture f;
    const std::string x(64, '2'), y(64, '3');
    bool peer_entered = false, release_peer = false;
    unsigned x_calls = 0;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view session, std::optional<std::string_view> tag) {
            std::unique_lock lock(f.mutex);
            ++f.calls;
            if (session == x) { ++x_calls; }
            else {
                peer_entered = true;
                f.cv.notify_all();
                f.cv.wait_for(lock, 2s, [&] { return release_peer; });
            }
            return AutomaticCheckpointResult{std::string(*tag), Reason::None};
        }, [&] { return f.observe(); }, [&] { f.notify(); }, {}, kNoFallback);
    std::array<Victim, 2> victims{{{x, "resp_x"}, {y, "resp_y"}}};
    coordinator.save_before_eviction(victims);
    int failures = check(f.wait([&] { return f.completed >= 1 && peer_entered; }),
                         "saved-peer fixture did not hold its second save");
    for (unsigned ask = 0; ask < 2; ++ask) {
        coordinator.save_before_eviction(victims);
        failures += check(victims[0].outcome == Outcome::Settled &&
                              victims[0].reason == Reason::None &&
                              victims[1].outcome == Outcome::Pending,
                          "saved answer disappeared while another victim remained Pending");
    }
    {
        std::lock_guard lock(f.mutex);
        release_peer = true;
        f.cv.notify_all();
    }
    failures += check(f.wait([&] { return f.completed >= 2; }), "saved peer did not finish");
    coordinator.save_before_eviction(victims);
    failures += check(victims[0].outcome == Outcome::Settled &&
                          victims[1].outcome == Outcome::Settled,
                      "saved peer answers never aligned in one plan");
    {
        std::lock_guard lock(f.mutex);
        failures += check(x_calls == 1, "saved peer was unnecessarily saved again");
    }
    coordinator.save_before_eviction(victims);
    failures += check(victims[0].outcome == Outcome::Pending &&
                          victims[1].outcome == Outcome::Pending,
                      "fully answered saved plan did not release its cached proofs");
    coordinator.drain();
    return failures;
}

int busy_and_publication_reset_pressure_window() {
    int failures = 0;
    for (unsigned wait_kind = 0; wait_kind < 3; ++wait_kind) {
        ProgressFixture f;
        f.refusal = Reason::Capacity;
        unsigned abandonments = 0;
        AutomaticCheckpointCoordinator coordinator(
            [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
            [&] { return f.observe(); }, [&] { f.notify(); },
            [&](const PressureAbandonment&) {
                std::lock_guard lock(f.mutex);
                ++abandonments;
            }, {.fallback = 1h, .pressure_window = 100ms});
        Victim victim{std::string(64, '4'), "resp_retry"};
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(f.wait([&] { return f.completed >= 1; }),
                          "window-reset fixture did not refuse persistently");
        {
            std::lock_guard lock(f.mutex);
            f.refusal = wait_kind == 1 ? Reason::Capacity : Reason::TransactionBusy;
            f.busy = wait_kind == 1;
            f.publishing = wait_kind == 2;
            ++f.progress.engine;
        }
        failures += check(f.wait([&] { return f.completed >= 2; }),
                          "window-reset fixture did not enter its busy wait");
        failures += check(!f.wait([&] { return f.completed > 2; }, 250ms),
                          "busy wait unexpectedly retried without progress");
        {
            std::lock_guard lock(f.mutex);
            f.refusal = Reason::Capacity;
            f.busy = f.publishing = false;
            ++f.progress.engine;
            ++f.progress.publication;
        }
        failures += check(f.wait([&] { return f.completed >= 3; }),
                          "persistent refusal did not resume after the busy wait");
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(victim.outcome == Outcome::Pending && victim.reason == Reason::Capacity,
                          "busy or publication time spent the persistent pressure window");
        {
            std::lock_guard lock(f.mutex);
            failures += check(abandonments == 0,
                              "resumed persistent refusal emitted a premature abandonment");
            f.refusal = Reason::None;
            ++f.progress.engine;
        }
        if (victim.outcome == Outcome::Pending) {
            failures += check(f.wait([&] { return f.completed >= 4; }),
                              "reset pressure window did not permit recovery");
        }
        coordinator.drain();
    }
    return failures;
}

int inflight_automatic_entry_is_promoted() {
    ProgressFixture f;
    f.refusal = Reason::IoUnavailable;
    bool entered = false, release = false;
    std::vector<PressureAbandonment> abandonments;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) {
            {
                std::unique_lock lock(f.mutex);
                if (f.calls == 0) {
                    entered = true;
                    f.cv.notify_all();
                    f.cv.wait_for(lock, 2s, [&] { return release; });
                }
            }
            return f.save(tag);
        }, [&] { return f.observe(); }, [&] { f.notify(); },
        [&](const PressureAbandonment& value) {
            std::lock_guard lock(f.mutex);
            abandonments.push_back(value);
            f.cv.notify_all();
        }, {.fallback = 50ms, .pressure_window = 150ms});
    Victim victim{std::string(64, '5'), "resp_retry"};
    int failures = check(coordinator.enqueue(victim.session_sha256) ==
                             AutomaticCheckpointEnqueueResult::Enqueued,
                         "in-flight promotion fixture did not queue");
    failures += check(f.wait([&] { return entered; }), "automatic first save did not start");
    coordinator.save_before_eviction(std::span(&victim, 1));
    failures += check(victim.outcome == Outcome::Pending, "in-flight automatic save was not Pending");
    {
        std::lock_guard lock(f.mutex);
        release = true;
        f.cv.notify_all();
    }
    failures += check(f.wait([&] {
        return !abandonments.empty() && f.completed >= abandonments[0].attempts;
    }), "in-flight automatic save did not acquire the pressure bound");
    coordinator.save_before_eviction(std::span(&victim, 1));
    {
        std::lock_guard lock(f.mutex);
        failures += check(abandonments.size() == 1 &&
                              abandonments[0].limit == PressureAbandonLimit::Window &&
                              abandonments[0].attempts == f.calls &&
                              f.calls < 1 + kTransientCheckpointRetries &&
                              victim.outcome == Outcome::Settled &&
                              victim.reason == Reason::IoUnavailable,
                          "empty-tag promotion ran the automatic budget before pressure settlement");
    }
    coordinator.drain();
    return failures;
}

int fresh_unread_settlements_survive_table_pressure() {
    const auto session = [](unsigned index) {
        constexpr std::string_view digits = "0123456789abcdef";
        return std::string(62, 'a') + digits[index / 16] + digits[index % 16];
    };
    ProgressFixture f;
    const std::string abandoned_session = session(63);
    unsigned abandonments = 0;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view digest, std::optional<std::string_view> tag) {
            std::lock_guard lock(f.mutex);
            ++f.calls;
            ++f.progress.engine;
            return AutomaticCheckpointResult{tag ? std::string(*tag) : "resp_retry",
                digest == abandoned_session ? Reason::Capacity : Reason::UnsupportedCodec};
        }, [&] { return f.observe(); }, [&] { f.notify(); },
        [&](const PressureAbandonment&) {
            std::lock_guard lock(f.mutex);
            ++abandonments;
        }, kNoFallback);
    for (unsigned index = 0; index < 64; ++index) {
        Victim victim{session(index), "resp_retry"};
        coordinator.save_before_eviction(std::span(&victim, 1));
    }
    constexpr unsigned initial_completions = 64 + kTransientCheckpointRetries;
    int failures = check(f.wait([&] { return f.completed >= initial_completions; }),
                         "unread-table fixture did not settle");
    Victim fresh{session(64), "resp_retry"};
    coordinator.save_before_eviction(std::span(&fresh, 1));
    failures += check(coordinator.enqueue(session(65)) == AutomaticCheckpointEnqueueResult::Dropped,
                      "full table purged fresh unread settlements for an automatic save");
    failures += check(!f.wait([&] { return f.completed > initial_completions; }, 100ms),
                      "full table purged fresh unread settlements for a pressure save");
    Victim abandoned{session(63), "resp_retry"};
    coordinator.save_before_eviction(std::span(&abandoned, 1));
    failures += check(abandoned.outcome == Outcome::Settled && abandoned.reason == Reason::Capacity,
                      "table pressure lost an unread abandonment answer");
    failures += check(coordinator.enqueue(session(65)) != AutomaticCheckpointEnqueueResult::Dropped,
                      "a read settlement did not make room for new work");
    coordinator.drain();
    {
        std::lock_guard lock(f.mutex);
        failures += check(abandonments == 1, "table pressure duplicated an abandonment receipt");
    }

    ProgressFixture aged;
    aged.refusal = Reason::UnsupportedCodec;
    AutomaticCheckpointCoordinator aged_coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) { return aged.save(tag); },
        [&] { return aged.observe(); }, [&] { aged.notify(); },
        {}, {.fallback = 1h, .pressure_window = 50ms});
    for (unsigned index = 0; index < 64; ++index) {
        Victim victim{session(index), "resp_retry"};
        aged_coordinator.save_before_eviction(std::span(&victim, 1));
    }
    failures += check(aged.wait([&] { return aged.completed >= 64; }),
                      "aged-table fixture did not settle");
    failures += check(!aged.wait([&] { return aged.completed > 64; }, 150ms),
                      "settled entries unexpectedly restarted while aging");
    failures += check(aged_coordinator.enqueue(session(64)) != AutomaticCheckpointEnqueueResult::Dropped,
                      "unread settlement older than twice the window did not make room");
    failures += check(aged.wait([&] { return aged.completed >= 65; }),
                      "work admitted after settlement aging did not run");
    aged_coordinator.drain();
    return failures;
}

int abandonment_receipt_precedes_settlement() {
    int failures = 0;
    for (const bool replace_revision : {false, true}) {
        ProgressFixture f;
        f.refusal = Reason::Capacity;
        bool receipt_entered = false, release_receipt = false;
        bool replacement_entered = false, release_replacement = false;
        unsigned receipts = 0;
        AutomaticCheckpointCoordinator coordinator(
            [&](std::string_view, std::optional<std::string_view> tag) {
                {
                    std::unique_lock lock(f.mutex);
                    if (replace_revision && f.calls >= 1 + kTransientCheckpointRetries) {
                        replacement_entered = true;
                        f.cv.notify_all();
                        f.cv.wait_for(lock, 2s, [&] { return release_replacement; });
                    }
                }
                return f.save(tag);
            }, [&] { return f.observe(); }, [&] { f.notify(); },
            [&](const PressureAbandonment&) {
                std::unique_lock lock(f.mutex);
                ++receipts;
                receipt_entered = true;
                f.cv.notify_all();
                f.cv.wait_for(lock, 2s, [&] { return release_receipt; });
            }, {.fallback = 1ms, .pressure_window = 1h});
        Victim victim{std::string(64, '6'), "resp_retry"};
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(f.wait([&] { return receipt_entered; }),
                          "receipt-order fixture did not reach abandonment");
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(victim.outcome == Outcome::Pending && victim.reason == Reason::Capacity,
                          "engine observed settlement before the abandonment receipt completed");
        if (replace_revision) {
            failures += check(coordinator.enqueue(victim.session_sha256) !=
                                  AutomaticCheckpointEnqueueResult::Dropped,
                              "receipt callback prevented a new revision");
        }
        {
            std::lock_guard lock(f.mutex);
            release_receipt = true;
            f.cv.notify_all();
        }
        failures += check(f.wait([&] {
            return f.completed >= 1 + kTransientCheckpointRetries &&
                   (!replace_revision || replacement_entered);
        }), "receipt completion did not release its task");
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(victim.outcome == (replace_revision ? Outcome::Pending : Outcome::Settled),
                          "receipt completion settled the wrong entry revision");
        if (replace_revision) {
            {
                std::lock_guard lock(f.mutex);
                f.refusal = Reason::None;
                release_replacement = true;
                f.cv.notify_all();
            }
            failures += check(f.wait([&] { return f.completed >= 2 + kTransientCheckpointRetries; }),
                              "replacement save did not finish");
            coordinator.save_before_eviction(std::span(&victim, 1));
            failures += check(victim.outcome == Outcome::Settled && victim.reason == Reason::None,
                              "obsolete abandonment replaced the new revision's answer");
        }
        coordinator.drain();
        {
            std::lock_guard lock(f.mutex);
            failures += check(receipts == 1, "revision change emitted a second abandonment receipt");
        }
    }
    return failures;
}

int drain_hides_retained_refusal_answers() {
    int failures = 0;
    for (const auto reason : {Reason::UnsupportedCodec, Reason::Capacity}) {
        ProgressFixture f;
        f.refusal = reason;
        AutomaticCheckpointCoordinator coordinator(
            [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
            [&] { return f.observe(); }, [&] { f.notify(); },
            {}, {.fallback = 1ms, .pressure_window = 1h});
        Victim victim{std::string(64, '7'), "resp_retry"};
        coordinator.save_before_eviction(std::span(&victim, 1));
        const unsigned attempts = reason == Reason::Capacity ? 1 + kTransientCheckpointRetries : 1;
        failures += check(f.wait([&] { return f.completed >= attempts; }),
                          "retained-drain fixture did not settle");
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(victim.outcome == Outcome::Settled && victim.reason == reason,
                          "retained-drain fixture did not expose its pre-stop answer");
        coordinator.drain();
        for (unsigned ask = 0; ask < 2; ++ask) {
            coordinator.save_before_eviction(std::span(&victim, 1));
            failures += check(victim.outcome == Outcome::Pending && victim.reason == reason,
                              "drain exposed a retained non-None settlement before final save");
        }
        {
            std::lock_guard lock(f.mutex);
            failures += check(f.calls == attempts, "drain erased or restarted a retained answer");
        }
    }
    return failures;
}

// A busy transaction is not an inability to save: a pressure save refused that way waits for each
// observed progress change without spending the retry budget, so more busy refusals than the
// budget allows still end in a settled save, and nothing retries without progress.
int pressure_transaction_busy_waits_without_budget() {
    ProgressFixture f;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
        [&] { return f.observe(); }, [&] { f.notify(); }, {}, kNoFallback);
    Victim victim{std::string(64, 'f'), "resp_retry"};
    coordinator.save_before_eviction(std::span(&victim, 1));
    constexpr unsigned refusals = 2 * kTransientCheckpointRetries + 2;
    int failures = 0;
    unsigned reached = 0;
    for (unsigned count = 1; count <= refusals; ++count) {
        if (!f.wait([&] { return f.completed == count; })) { break; }
        reached = count;
        failures += check(!f.wait([&] { return f.calls > count; }, 50ms),
                          "a busy pressure save retried without observed progress");
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(victim.outcome == Outcome::Pending && victim.reason == Reason::TransactionBusy,
                          "a busy pressure save was not Pending with its reason");
        std::lock_guard lock(f.mutex);
        if (count == refusals) { f.refusal = Reason::None; }
        // Engine and publication progress each end a busy wait.
        if (count % 2) { ++f.progress.engine; } else { ++f.progress.publication; }
    }
    failures += check(reached == refusals,
                      ("busy pressure save stopped retrying after " + std::to_string(reached) +
                       " refusals").c_str());
    failures += check(f.wait([&] { return f.completed == refusals + 1; }),
                      "the save after the busy refusals never ran");
    coordinator.save_before_eviction(std::span(&victim, 1));
    failures += check(victim.outcome == Outcome::Settled && victim.reason == Reason::None,
                      "a pressure save refused busy past the retry budget did not settle once saved");
    coordinator.drain();
    return failures;
}

// Automatic saves keep the bounded budget for busy refusals: a dropped automatic save holds no
// latch, and the session's next completed turn re-arms it.
int automatic_transaction_busy_keeps_budget() {
    ProgressFixture f;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
        [&] { return f.observe(); }, [&] { f.notify(); });
    int failures = check(coordinator.enqueue(std::string(64, 'g')) == AutomaticCheckpointEnqueueResult::Enqueued,
                         "automatic save was not enqueued");
    for (unsigned count = 1; count <= kTransientCheckpointRetries + 1; ++count) {
        failures += check(f.wait([&] { return f.completed == count; }), "bounded automatic retry did not finish");
        std::lock_guard lock(f.mutex);
        ++f.progress.engine;
    }
    failures += check(!f.wait([&] { return f.completed > kTransientCheckpointRetries + 1; }, 300ms),
                      "an automatic busy save retried past its budget");
    coordinator.drain();
    return failures;
}

// Engine progress can move every turn while a busy cause lasts (the session's own request, the
// store's single save slot). A busy pressure save keeps retrying without a budget, but no faster
// than the policy's busy spacing (100 ms, kBusyPressureRetrySpacing).
int pressure_busy_retries_are_spaced() {
    ProgressFixture f;
    std::vector<std::chrono::steady_clock::time_point> attempts;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) {
            {
                std::lock_guard lock(f.mutex);
                attempts.push_back(std::chrono::steady_clock::now());
            }
            return f.save(tag);
        },
        [&] { return f.observe(); }, [&] { f.notify(); });
    std::atomic<bool> ticking{true};
    std::thread clock([&] {
        while (ticking.load()) {
            {
                std::lock_guard lock(f.mutex);
                ++f.progress.engine;
            }
            std::this_thread::sleep_for(1ms);
        }
    });
    Victim victim{std::string(64, 'h'), "resp_retry"};
    coordinator.save_before_eviction(std::span(&victim, 1));
    std::this_thread::sleep_for(1050ms);
    ticking.store(false);
    clock.join();
    coordinator.drain();
    std::chrono::nanoseconds spacing = std::chrono::hours(1);
    for (std::size_t index = 1; index < attempts.size(); ++index) {
        spacing = std::min<std::chrono::nanoseconds>(spacing, attempts[index] - attempts[index - 1]);
    }
    return check(attempts.size() >= 5 && attempts.size() <= 12 &&
                     spacing >= 90ms,
                 ("busy pressure retries under per-millisecond progress: attempts=" +
                  std::to_string(attempts.size()) + " minimum spacing ms=" +
                  std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(spacing).count()))
                     .c_str());
}

int partially_reclaiming_refusal_does_not_self_wake() {
    ProgressFixture f;
    f.refusal = Reason::Capacity;
    ninfer::runtime::CheckpointControl engine;
    const auto initial_progress = engine.observed_progress();
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) {
            // EngineCore's matching regression proves these notifications follow real
            // partial Host reclaim. Exercise the coordinator against the real clock here.
            engine.notify_progress(false);
            return f.save(tag);
        },
        [&] { auto progress = f.observe(); progress.engine = engine.observed_progress(); return progress; },
        [&] { engine.notify_progress(false); f.notify(); }, {}, kNoFallback);
    Victim victim{std::string(64, 'd'), "resp_retry"};
    coordinator.save_before_eviction({&victim, 1});
    int failures = check(f.wait([&] { return f.completed == 1 && f.probes >= 4; }),
                         "partially reclaiming Capacity attempt did not defer");
    {
        std::lock_guard lock(f.mutex);
        failures += check(f.calls == 1 && engine.observed_progress() == initial_progress,
                          "save-owned cleanup woke its own Capacity retry");
        f.refusal = Reason::None;
    }
    engine.notify_progress();
    failures += check(f.wait([&] { return f.completed == 2; }),
                      "independent Engine progress did not wake deferred Capacity");
    coordinator.drain();
    coordinator.save_before_eviction({&victim, 1});
    failures += check(victim.outcome == Outcome::Settled && victim.reason == Reason::None,
                      "retained settled pressure answer disappeared at drain");
    Victim fresh{std::string(64, 'e'), "new-pressure"};
    coordinator.save_before_eviction({&fresh, 1});
    failures += check(fresh.outcome == Outcome::Pending,
                      "new pressure after drain fabricated a settled save");
    return failures;
}

int publication_abandonment_completes_pressure() {
    ProgressFixture f;
    f.publishing = true;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
        [&] { return f.observe(); }, [&] { f.notify(); });
    Victim victim{std::string(64, 'a'), "resp_retry"};
    int failures = check(coordinator.enqueue("short-unpressured", true) ==
                             AutomaticCheckpointEnqueueResult::Dropped,
                         "unpressured short turn ignored the automatic threshold");
    coordinator.save_before_eviction(std::span(&victim, 1));
    failures += check(f.wait([&] { return f.completed == 1 && f.probes >= 3; }),
                      "publishing task did not complete into an explicit waiting state");
    unsigned probes;
    {
        std::lock_guard lock(f.mutex);
        probes = f.probes;
        f.progress.engine += kTransientCheckpointRetries + 10;
    }
    failures += check(f.wait([&] { return f.probes >= probes + 2; }),
                      "publication progress observer stopped");
    {
        std::lock_guard lock(f.mutex);
        failures += check(f.calls == 1, "native progress spent a publication wait's retry budget");
        f.publishing = false;
        f.refusal = Reason::RequiredResponseMissing;
        ++f.progress.publication;
    }
    failures += check(f.wait([&] { return f.completed == 2; }),
                      "publication abandonment left the pressure latch pending");
    coordinator.save_before_eviction(std::span(&victim, 1));
    failures += check(victim.outcome == Outcome::Settled && victim.reason == Reason::RequiredResponseMissing,
                      "abandoned publication did not settle with its reported reason");
    coordinator.drain();
    return failures;
}

int pressure_window_settles_slow_refusals() {
    ProgressFixture f;
    f.refusal = Reason::Capacity;
    std::vector<PressureAbandonment> abandonments;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
        [&] { return f.observe(); }, [&] { f.notify(); },
        [&](const PressureAbandonment& value) {
            std::lock_guard lock(f.mutex);
            abandonments.push_back(value);
            f.cv.notify_all();
        }, {.fallback = 100ms, .pressure_window = 250ms});
    Victim victim{std::string(64, '4'), "resp_window"};
    coordinator.save_before_eviction(std::span(&victim, 1));
    int failures = check(f.wait([&] {
        return !abandonments.empty() && f.completed >= abandonments[0].attempts;
    }),
                         "pressure window did not settle without progress");
    coordinator.save_before_eviction(std::span(&victim, 1));
    {
        std::lock_guard lock(f.mutex);
        failures += check(abandonments.size() == 1 &&
            abandonments[0].limit == PressureAbandonLimit::Window &&
            abandonments[0].reason == Reason::Capacity &&
            abandonments[0].attempts >= 3 && abandonments[0].attempts <= 5 &&
            abandonments[0].attempts == f.calls && abandonments[0].elapsed_ms >= 250 &&
            victim.outcome == Outcome::Settled && victim.reason == Reason::Capacity,
            "pressure window did not preserve its last reason and exact attempt count");
    }
    coordinator.drain();
    return failures;
}

int pressure_abandonment_receipts() {
    ProgressFixture f;
    f.refusal = Reason::Capacity;
    std::vector<PressureAbandonment> abandonments;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
        [&] { return f.observe(); }, [&] { f.notify(); },
        [&](const PressureAbandonment& value) {
            std::lock_guard lock(f.mutex);
            abandonments.push_back(value);
            f.cv.notify_all();
        }, {.fallback = 10ms, .pressure_window = 1h});
    int failures = 0;
    for (unsigned index = 0; index < 2; ++index) {
        Victim victim{std::string(64, static_cast<char>('5' + index)), "resp_budget"};
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(f.wait([&] {
            return abandonments.size() >= index + 1 &&
                   f.completed >= (index + 1) * (1 + kTransientCheckpointRetries);
        }),
                          "budget exhaustion did not publish an abandonment");
        std::lock_guard lock(f.mutex);
        failures += check(abandonments.size() == index + 1 &&
            abandonments.back().reason == Reason::Capacity &&
            abandonments.back().limit == PressureAbandonLimit::Budget &&
            abandonments.back().attempts == 1 + kTransientCheckpointRetries &&
            abandonments.back().total == index + 1 && abandonments.back().elapsed_ms > 0,
            "abandonment lost its budget, attempt count, elapsed time or cumulative count");
    }
    for (const auto reason : {Reason::UnsupportedCodec, Reason::None}) {
        unsigned completed;
        {
            std::lock_guard lock(f.mutex);
            f.refusal = reason;
            completed = f.completed;
        }
        Victim victim{std::string(64, reason == Reason::None ? '8' : '7'), "resp_terminal"};
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(f.wait([&] { return f.completed > completed; }),
                          "terminal pressure outcome never finished");
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(victim.outcome == Outcome::Settled && victim.reason == reason,
                          "terminal pressure outcome did not settle");
    }
    coordinator.drain();
    failures += check(abandonments.size() == 2, "success or terminal refusal emitted an abandonment");
    return failures;
}

int pinned_busy_capacity_waits_without_budget() {
    ProgressFixture f;
    f.refusal = Reason::Capacity;
    f.busy = true;
    std::atomic<unsigned> abandoned{0};
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
        [&] { return f.observe(); }, [&] { f.notify(); },
        [&](const PressureAbandonment&) { ++abandoned; }, kNoFallback);
    Victim victim{std::string(64, '9'), "resp_pinned"};
    coordinator.save_before_eviction(std::span(&victim, 1));
    constexpr unsigned refusals = 2 * kTransientCheckpointRetries + 2;
    int failures = 0;
    unsigned reached = 0;
    for (unsigned count = 1; count <= refusals; ++count) {
        if (!f.wait([&] { return f.completed >= count; })) { break; }
        reached = count;
        coordinator.save_before_eviction(std::span(&victim, 1));
        failures += check(victim.outcome == Outcome::Pending && victim.reason == Reason::Capacity,
                          "pinned capacity settled or lost its public reason");
        std::lock_guard lock(f.mutex);
        if (count == refusals) { f.refusal = Reason::None; }
        ++f.progress.engine;
    }
    failures += check(reached == refusals, "pinned capacity spent the persistent retry budget");
    failures += check(f.wait([&] { return f.completed >= refusals + 1; }),
                      "pinned capacity did not save once the pin retired");
    coordinator.save_before_eviction(std::span(&victim, 1));
    failures += check(victim.outcome == Outcome::Settled && victim.reason == Reason::None && abandoned == 0,
                      "pinned capacity was abandoned instead of saved");
    coordinator.drain();

    ProgressFixture automatic;
    automatic.refusal = Reason::Capacity;
    automatic.busy = true;
    AutomaticCheckpointCoordinator automatic_coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) { return automatic.save(tag); },
        [&] { return automatic.observe(); }, [&] { automatic.notify(); },
        [&](const PressureAbandonment&) { ++abandoned; }, kNoFallback);
    failures += check(automatic_coordinator.enqueue(std::string(64, 'a')) !=
                          AutomaticCheckpointEnqueueResult::Dropped,
                      "automatic pinned-capacity fixture was not queued");
    for (unsigned count = 1; count <= 1 + kTransientCheckpointRetries; ++count) {
        failures += check(automatic.wait([&] { return automatic.completed >= count; }),
                          "automatic pinned-capacity retry did not finish");
        std::lock_guard lock(automatic.mutex);
        ++automatic.progress.engine;
    }
    failures += check(!automatic.wait([&] { return automatic.calls > 1 + kTransientCheckpointRetries; }, 300ms),
                      "automatic pinned capacity escaped its retry budget");
    automatic_coordinator.drain();
    failures += check(abandoned == 0, "automatic pinned capacity emitted a pressure abandonment");
    return failures;
}

int drain_keeps_inflight_pressure_pending() {
    ProgressFixture f;
    f.refusal = Reason::Capacity;
    std::promise<void> entered, release;
    auto started = entered.get_future();
    const auto released = release.get_future().share();
    std::atomic<unsigned> abandoned{0};
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) {
            entered.set_value();
            released.wait();
            return f.save(tag);
        }, [&] { return f.observe(); }, [&] { f.notify(); },
        [&](const PressureAbandonment&) { ++abandoned; });
    Victim victim{std::string(64, 'b'), "resp_draining"};
    coordinator.save_before_eviction(std::span(&victim, 1));
    int failures = check(started.wait_for(2s) == std::future_status::ready,
                         "drain fixture did not enter the save");
    auto drained = std::async(std::launch::async, [&] { coordinator.drain(); });
    failures += check(drained.wait_for(50ms) == std::future_status::timeout,
                      "drain did not wait for its in-flight attempt");
    release.set_value();
    drained.get();
    coordinator.save_before_eviction(std::span(&victim, 1));
    failures += check(victim.outcome == Outcome::Pending && victim.reason == Reason::Capacity && abandoned == 0,
                      "drain abandoned the pressure victim before synchronous final save");
    return failures;
}

int promoted_automatic_entry_settles() {
    ProgressFixture f;
    f.refusal = Reason::IoUnavailable;
    std::vector<PressureAbandonment> abandonments;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
        [&] { return f.observe(); }, [&] { f.notify(); },
        [&](const PressureAbandonment& value) {
            std::lock_guard lock(f.mutex);
            abandonments.push_back(value);
            f.cv.notify_all();
        }, {.fallback = 50ms, .pressure_window = 400ms});
    Victim victim{std::string(64, 'c'), "resp_retry"};
    int failures = check(coordinator.enqueue(victim.session_sha256) != AutomaticCheckpointEnqueueResult::Dropped,
                         "promotion fixture was not queued");
    failures += check(f.wait([&] { return f.completed >= 1; }), "automatic I/O attempt did not finish");
    coordinator.save_before_eviction(std::span(&victim, 1));
    failures += check(victim.outcome == Outcome::Pending, "automatic entry did not promote as Pending");
    failures += check(f.wait([&] {
        return !abandonments.empty() && f.completed >= abandonments[0].attempts;
    }), "promoted automatic save stayed Pending");
    coordinator.save_before_eviction(std::span(&victim, 1));
    unsigned calls;
    {
        std::lock_guard lock(f.mutex);
        calls = f.calls;
        failures += check(abandonments.size() == 1 && abandonments[0].attempts == calls &&
            abandonments[0].reason == Reason::IoUnavailable &&
            victim.outcome == Outcome::Settled && victim.reason == Reason::IoUnavailable,
            "promoted automatic save lost its attempts or refusal");
    }
    failures += check(!f.wait([&] { return f.calls > calls; }, 300ms),
                      "settled promoted entry restarted without a new turn");
    coordinator.drain();
    return failures;
}

int capacity_busy_mapping() {
    using Shortage = ninfer::runtime::DurableHostShortage;
    int failures = check(capacity_refusal_is_busy(Reason::Capacity, Shortage::Busy),
                         "pinned native capacity was not classified busy");
    for (const auto shortage : {Shortage::None, Shortage::HostCapacity,
                                Shortage::HostContiguity, Shortage::NoHostArena}) {
        failures += check(!capacity_refusal_is_busy(Reason::Capacity, shortage),
                          "persistent capacity was classified busy");
    }
    failures += check(!capacity_refusal_is_busy(Reason::TransactionBusy, Shortage::Busy),
                      "capacity mapping reinterpreted a different reason");
    return failures;
}

class FileReadQueue final : public ninfer::runtime::ContinuationCheckpointReadQueue {
    class Completion final : public ninfer::runtime::ContinuationCheckpointReadCompletion {
    public:
        Completion(std::filesystem::path path,
                   std::span<const ninfer::runtime::ContinuationCheckpointReadRequest> requests)
            : path_(std::move(path)), requests_(requests.begin(), requests.end()) {}
        void wait() override {
            if (done_) { return; }
            done_ = true;
            std::ifstream input(path_, std::ios::binary);
            for (const auto& request : requests_) {
                input.seekg(static_cast<std::streamoff>(request.file_offset));
                input.read(reinterpret_cast<char*>(request.destination.data()),
                           static_cast<std::streamsize>(request.destination.size()));
                if (!input || input.gcount() != static_cast<std::streamsize>(request.destination.size())) {
                    throw std::runtime_error("checkpoint fixture read failed");
                }
            }
        }
    private:
        std::filesystem::path path_;
        std::vector<ninfer::runtime::ContinuationCheckpointReadRequest> requests_;
        bool done_ = false;
    };
public:
    bool available() const noexcept override { return true; }
    bool overlaps_batches() const noexcept override { return false; }
    std::string_view backend_name() const noexcept override { return "host-file-test"; }
    std::string_view unavailable_reason() const noexcept override { return {}; }
    std::unique_ptr<ninfer::runtime::ContinuationCheckpointReadCompletion> submit(
        const std::filesystem::path& path,
        std::span<const ninfer::runtime::ContinuationCheckpointReadRequest> requests) override {
        return std::make_unique<Completion>(path, requests);
    }
};

int durable_record_delete_preserves_survivors() {
    const auto root = std::filesystem::temp_directory_path() /
        ("ninfer-delete-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{root};
    const std::string session(64, 'd');
    nlohmann::json manifest;
    std::ifstream(std::filesystem::path(NINFER_SOURCE_DIR) / "tests/fixtures/durable/manifest-text.json")
        >> manifest;
    const auto fingerprint = manifest.at("runtime_fingerprint");
    SessionCheckpointStoreOptions options{.root = root, .origin_mac_key = std::string(32, 'k'),
                                          .require_origin_auth = true,
                                          .read_queue = std::make_shared<FileReadQueue>()};
    SessionCheckpointStore disk(options);
    OpenAIResponsesStore responses(8, 1ULL << 20);
    responses.put(record("resp_parent", session, 1));
    auto child = record("resp_child", session, 2);
    child.previous_response_id = "resp_parent";
    responses.put(std::move(child));
    const auto save = [&](const OpenAIResponseStoreSnapshot& snapshot) {
        return disk.save(snapshot, fingerprint,
            [](ninfer::runtime::ContinuationCheckpointWriter& writer) {
                const std::array<std::byte, 1> payload{std::byte{7}};
                const bool written = writer.write_file("engine/index.cbor", 0, 0, {}, 1) &&
                    writer.write_file("engine/state/0.bin", 0, payload.size(), payload, 1);
                return ninfer::runtime::DurableExportResult{
                    .state = written ? ninfer::runtime::DurableExportState::Exported
                                     : ninfer::runtime::DurableExportState::Refused,
                    .stats = {.frontier_tokens = 1, .restored_tokens = 1,
                              .payload_bytes = 1, .points = 1, .states = 1}};
            }, [](ninfer::runtime::DurableSessionEpoch, const std::function<bool()>& commit) {
                return ninfer::runtime::DurablePublicationResult{
                    .state = commit() ? ninfer::runtime::DurableExportState::Exported
                                      : ninfer::runtime::DurableExportState::Refused};
            });
    };
    int failures = check(save(*responses.snapshot_session(session)).has_value(), "initial save failed");
    Reason refusal{};
    bool invoked = false;
    failures += check(!responses.erase_for_session_durably("resp_child", session,
        [&](const auto& survivors) { invoked = survivors && survivors->replay_only; return false; }, &refusal) &&
        invoked && refusal == Reason::None && responses.size() == 2,
        "failed replay-only leaf publication removed the live endpoint");
    failures += check(!responses.erase_for_session_durably("resp_parent", session,
        [](const auto&) { return false; }) && responses.size() == 2,
        "failed durable replacement removed RAM response");
    {
        const auto old = disk.load(session, fingerprint, "resp_parent");
        failures += check(old.state == SessionCheckpointLoadState::Available &&
                              old.checkpoint->responses.records.size() == 2,
                          "failed deletion lost prior durable authority");
    }
    const auto prior = *responses.snapshot_session(session);
#if defined(_WIN32) || defined(__linux__)
    bool capacity_threw = false, capacity_erased = false;
    {
        checkpoint_write_fault::Scope fault(ENOSPC);
        try {
            capacity_erased = responses.erase_for_session_durably("resp_child", session,
                [&](const auto& survivors) {
                    ninfer::runtime::SessionCheckpointSkipDetail skip;
                    const auto saved = disk.save_responses(*survivors, fingerprint, &skip);
                    refusal = skip.durable_reason;
                    return saved.has_value();
                }, &refusal, &prior);
        } catch (...) { capacity_threw = true; }
    }
    failures += check(checkpoint_write_fault::fired != 0 && !capacity_threw && !capacity_erased &&
                          refusal == Reason::Capacity && responses.size() == 2 &&
                          responses.get_for_session("resp_parent", session) &&
                          responses.get_for_session("resp_child", session),
                      "disk-full DELETE lost capacity reason or changed live survivors");
    {
        const auto unchanged = disk.load(session, fingerprint, "resp_child");
        failures += check(unchanged.state == SessionCheckpointLoadState::Available &&
                              unchanged.checkpoint && unchanged.checkpoint->responses.records.size() == 2,
                          "disk-full DELETE discarded prior durable survivors");
    }
#endif
    failures += check(responses.erase_for_session_durably("resp_parent", session,
        [&](const auto& survivors) { return survivors && save(*survivors).has_value(); }, &refusal, &prior),
        "ancestor deletion did not publish its survivor generation");
    // Reopen immediately at the DELETE acknowledgement boundary: no final/automatic save.
    {
        SessionCheckpointStore restarted(options);
        const auto loaded = restarted.load(session, fingerprint, "resp_child");
        failures += check(loaded.state == SessionCheckpointLoadState::Available &&
            loaded.checkpoint->responses.records.size() == 1 &&
            loaded.checkpoint->responses.latest_response_id == "resp_child" &&
            restarted.load(session, fingerprint, "resp_parent").state != SessionCheckpointLoadState::Available,
            "DELETE acknowledgement lost undeleted lineage or resurrected the parent after restart");
    }
    failures += check(responses.erase_for_session_durably("resp_child", session,
        [&](const auto& survivors) {
            return !survivors && disk.erase(session) != SessionCheckpointEraseResult::Conflict;
        }) && responses.size() == 0, "last-record deletion failed");
    failures += check(SessionCheckpointStore(options).load(session, fingerprint).state ==
                          SessionCheckpointLoadState::Missing,
                      "last-record deletion resurrected a durable record");

    // The shipped API also deletes the leaf with 200. Its parent survives by authenticated
    // replay, never by falsely attaching the child's native state to the parent's ID.
    for (const auto& value : prior.records) { responses.put(value); }
    failures += check(save(prior).has_value(), "leaf fixture initial save failed");
    failures += check(responses.erase_for_session_durably("resp_child", session,
        [&](const auto& survivors) {
            return survivors && survivors->replay_only &&
                disk.save_responses(*survivors, fingerprint).has_value();
        }, &refusal, &prior), "leaf deletion was not acknowledged with a durable survivor");
    {
        SessionCheckpointStore restarted(options);
        auto loaded = restarted.load(session, fingerprint, "resp_parent");
        failures += check(loaded.state == SessionCheckpointLoadState::Available && loaded.checkpoint &&
            loaded.checkpoint->responses.replay_only && !loaded.checkpoint->engine &&
            loaded.checkpoint->responses.records.size() == 1 &&
            !restarted.covers(session, fingerprint, "resp_parent") &&
            restarted.load(session, fingerprint, "resp_child").state != SessionCheckpointLoadState::Available,
            "leaf DELETE restart lost its parent or exposed a deleted native endpoint");
        if (loaded.checkpoint) {
            OpenAIResponsesStore hydrated(8, 1ULL << 20);
            auto guard = hydrated.begin_restore(session);
            failures += check(hydrated.restore_session(std::move(loaded.checkpoint->responses), guard,
                [] { return true; }) && hydrated.snapshot_session(session)->replay_only &&
                hydrated.get_for_session("resp_parent", session),
                "replay-only survivor did not hydrate for parent transcript replay");
            hydrated.put(record("resp_new_turn", session, 3));
            failures += check(!hydrated.snapshot_session(session)->replay_only,
                              "new completed turn did not replace replay-only endpoint identity");
        }
    }
    return failures;
}

int durable_delete_retains_disk_only_records() {
    const std::string session(64, 'e'), foreign(64, 'f');
    OpenAIResponsesStore responses(3, 1ULL << 20);
    responses.put(record("resp_archived", session, 1));
    responses.put(record("resp_parent", session, 2));
    responses.put(record("resp_child", session, 3));
    const auto disk = *responses.snapshot_session(session);
    responses.put(record("resp_unrelated", foreign, 4)); // evicts archived only in RAM
    std::optional<OpenAIResponseStoreSnapshot> published;
    Reason refusal{};
    int failures = check(responses.erase_for_session_durably("resp_parent", session,
        [&](auto& survivors) {
            // A busy/absent native endpoint may fall back after snapshot preparation.
            survivors->replay_only = true;
            published = survivors;
            return true;
        }, &refusal, &disk) &&
        published && published->records.size() == 2 && published->records.front().id == "resp_archived" &&
        published->latest_response_id == "resp_child" && responses.snapshot_session(session)->replay_only,
        "deletion discarded an undeleted disk-only record");
    bool called = false;
    failures += check(responses.erase_for_session_durably("resp_child", session,
        [&](const auto& survivors) {
            called = survivors && survivors->replay_only && survivors->records.size() == 1 &&
                survivors->latest_response_id == "resp_archived";
            return called;
        }, &refusal, &*published) &&
        called && !responses.get_for_session("resp_child", session),
        "last RAM record deletion failed to retain disk-only replay survivors");
    return failures;
}

// The store's single save slot. A pressure save refused as busy because a competing save holds
// the slot through more than six progress ticks keeps waiting, and settles once that save ends.
int pressure_save_waits_out_competing_store_save() {
    const auto root = std::filesystem::temp_directory_path() /
        ("ninfer-busy-slot-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{root};
    nlohmann::json manifest;
    std::ifstream(std::filesystem::path(NINFER_SOURCE_DIR) / "tests/fixtures/durable/manifest-text.json")
        >> manifest;
    const auto fingerprint = manifest.at("runtime_fingerprint");
    SessionCheckpointStoreOptions options{.root = root, .origin_mac_key = std::string(32, 'k'),
                                          .require_origin_auth = true,
                                          .read_queue = std::make_shared<FileReadQueue>()};
    SessionCheckpointStore disk(options);
    OpenAIResponsesStore responses(8, 1ULL << 20);
    const std::string competitor(64, 'a'), victim(64, 'b');
    responses.put(record("resp_competitor", competitor, 1));
    responses.put(record("resp_victim", victim, 2));
    const auto exporter = [](ninfer::runtime::ContinuationCheckpointWriter& writer) {
        const std::array<std::byte, 1> payload{std::byte{7}};
        const bool written = writer.write_file("engine/index.cbor", 0, 0, {}, 1) &&
            writer.write_file("engine/state/0.bin", 0, payload.size(), payload, 1);
        return ninfer::runtime::DurableExportResult{
            .state = written ? ninfer::runtime::DurableExportState::Exported
                             : ninfer::runtime::DurableExportState::Refused,
            .stats = {.frontier_tokens = 1, .restored_tokens = 1,
                      .payload_bytes = 1, .points = 1, .states = 1}};
    };
    const auto publish = [](ninfer::runtime::DurableSessionEpoch, const std::function<bool()>& commit) {
        return ninfer::runtime::DurablePublicationResult{
            .state = commit() ? ninfer::runtime::DurableExportState::Exported
                              : ninfer::runtime::DurableExportState::Refused};
    };
    // The competing save holds the slot inside its exporter until released.
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    const std::shared_future<void> released = release.get_future().share();
    auto competing = std::async(std::launch::async, [&] {
        return disk.save(*responses.snapshot_session(competitor), fingerprint,
            [&](ninfer::runtime::ContinuationCheckpointWriter& writer) {
                entered.set_value();
                released.wait();
                return exporter(writer);
            }, publish);
    });
    struct Release {
        std::promise<void>& promise;
        bool done = false;
        void now() { if (!std::exchange(done, true)) { promise.set_value(); } }
        ~Release() { now(); }
    } guard{release};
    entered_future.wait();
    ProgressFixture f;
    unsigned busy = 0;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view session, std::optional<std::string_view> tag) {
            ninfer::runtime::SessionCheckpointSkipDetail skip;
            const auto saved = disk.save(*responses.snapshot_session(session), fingerprint, exporter,
                                         publish, &skip);
            std::lock_guard lock(f.mutex);
            ++f.calls;
            busy += !saved && skip.durable_reason == Reason::TransactionBusy ? 1U : 0U;
            return AutomaticCheckpointResult{std::string(*tag), saved ? Reason::None : skip.durable_reason};
        },
        [&] { return f.observe(); }, [&] { f.notify(); }, {}, kNoFallback);
    Victim pressure{victim, "resp_victim"};
    coordinator.save_before_eviction(std::span(&pressure, 1));
    constexpr unsigned refusals = kTransientCheckpointRetries + 4;
    unsigned reached = 0;
    for (unsigned count = 1; count <= refusals; ++count) {
        if (!f.wait([&] { return f.completed == count; })) { break; }
        reached = count;
        if (count != refusals) {
            std::lock_guard lock(f.mutex);
            ++f.progress.engine;
        }
    }
    guard.now();
    int failures = check(competing.get().has_value(), "competing save did not complete");
    { std::lock_guard lock(f.mutex); ++f.progress.engine; }
    failures += check(f.wait([&] { return f.completed == refusals + 1; }),
                      "the pressure save did not run after the competing save ended");
    coordinator.save_before_eviction(std::span(&pressure, 1));
    {
        std::lock_guard lock(f.mutex);
        failures += check(reached == refusals && busy == refusals && f.calls == refusals + 1 &&
                              pressure.outcome == Outcome::Settled && pressure.reason == Reason::None,
                          ("pressure save behind the store's save slot: busy refusals=" +
                           std::to_string(busy) + " attempts=" + std::to_string(f.calls) +
                           " settled=" + std::to_string(pressure.outcome == Outcome::Settled))
                              .c_str());
    }
    coordinator.drain();
    return failures;
}
} // namespace

int main() {
    const auto run = [](const char* name, int failures) {
        std::cout << (failures ? "FAIL " : "PASS ") << name << '\n';
        return failures;
    };
    int failures = run("queue_coalescing", queue_coalescing());
    failures += run("pressure_callback_does_not_wait_for_restore_commit", pressure_callback_does_not_wait_for_restore_commit());
    failures += run("automatic_retry_requires_matching_progress", automatic_retry_requires_matching_progress());
    failures += run("pressure_retry_exhaustion_settles_with_reason", pressure_retry_exhaustion_settles_with_reason());
    failures += run("persistent_refusal_retries_without_progress", persistent_refusal_retries_without_progress());
    failures += run("settled_entries_do_not_fill_the_table", settled_entries_do_not_fill_the_table());
    failures += run("settled_answer_survives_a_pending_peer", settled_answer_survives_a_pending_peer());
    failures += run("saved_answer_survives_a_pending_peer", saved_answer_survives_a_pending_peer());
    failures += run("busy_and_publication_reset_pressure_window", busy_and_publication_reset_pressure_window());
    failures += run("inflight_automatic_entry_is_promoted", inflight_automatic_entry_is_promoted());
    failures += run("fresh_unread_settlements_survive_table_pressure", fresh_unread_settlements_survive_table_pressure());
    failures += run("abandonment_receipt_precedes_settlement", abandonment_receipt_precedes_settlement());
    failures += run("drain_hides_retained_refusal_answers", drain_hides_retained_refusal_answers());
    failures += run("pressure_transaction_busy_waits_without_budget", pressure_transaction_busy_waits_without_budget());
    failures += run("automatic_transaction_busy_keeps_budget", automatic_transaction_busy_keeps_budget());
    failures += run("pressure_busy_retries_are_spaced", pressure_busy_retries_are_spaced());
    failures += run("partially_reclaiming_refusal_does_not_self_wake", partially_reclaiming_refusal_does_not_self_wake());
    failures += run("publication_abandonment_completes_pressure", publication_abandonment_completes_pressure());
    failures += run("pressure_window_settles_slow_refusals", pressure_window_settles_slow_refusals());
    failures += run("pressure_abandonment_receipts", pressure_abandonment_receipts());
    failures += run("pinned_busy_capacity_waits_without_budget", pinned_busy_capacity_waits_without_budget());
    failures += run("drain_keeps_inflight_pressure_pending", drain_keeps_inflight_pressure_pending());
    failures += run("promoted_automatic_entry_settles", promoted_automatic_entry_settles());
    failures += run("capacity_busy_mapping", capacity_busy_mapping());
    failures += run("durable_record_delete_preserves_survivors", durable_record_delete_preserves_survivors());
    failures += run("durable_delete_retains_disk_only_records", durable_delete_retains_disk_only_records());
    failures += run("pressure_save_waits_out_competing_store_save", pressure_save_waits_out_competing_store_save());
    if (!failures) { std::cout << "automatic checkpoint remediation tests passed\n"; }
    return failures ? 1 : 0;
}
