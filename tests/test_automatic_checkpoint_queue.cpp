#include "serve/automatic_checkpoint_queue.h"
#include "serve/session_checkpoint_store.h"

#include <atomic>
#include <array>
#include <chrono>
#include <future>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace ninfer::serve;
using Reason = ninfer::runtime::DurableReason;
using Outcome = ninfer::runtime::PressureCheckpointOutcome;
using Victim = ninfer::runtime::PressureCheckpointVictim;
using namespace std::chrono_literals;

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

    bool wait(const std::function<bool()>& predicate) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 2s, predicate);
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
        return {tag ? std::string(*tag) : "resp_retry", refusal};
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
            [&] { return f.observe(); }, [&] { f.notify(); });
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

int automatic_retry_budget_and_terminal_refusal() {
    ProgressFixture f;
    AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view, std::optional<std::string_view> tag) { return f.save(tag); },
        [&] { return f.observe(); }, [&] { f.notify(); });
    Victim victim{std::string(64, 'c'), "resp_retry"};
    coordinator.save_before_eviction(std::span(&victim, 1));
    int failures = 0;
    for (unsigned count = 1; count <= kTransientCheckpointRetries + 1; ++count) {
        failures += check(f.wait([&] { return f.completed == count; }), "bounded retry did not finish");
        std::lock_guard lock(f.mutex);
        ++f.progress.engine;
    }
    coordinator.save_before_eviction(std::span(&victim, 1));
    failures += check(victim.outcome == Outcome::Pending && victim.reason == Reason::TransactionBusy,
                      "retry exhaustion permitted destructive release");
    // A new completed turn resets the bounded attempt state.
    { std::lock_guard lock(f.mutex); f.refusal = Reason::UnsupportedCodec; }
    (void)coordinator.enqueue(victim.session_sha256);
    failures += check(f.wait([&] { return f.completed == kTransientCheckpointRetries + 2; }),
                      "new completed turn did not restart checkpoint work");
    coordinator.save_before_eviction(std::span(&victim, 1));
    failures += check(f.wait([&] { return f.completed == kTransientCheckpointRetries + 3; }),
                      "terminal pressure refusal never completed");
    coordinator.save_before_eviction(std::span(&victim, 1));
    failures += check(victim.outcome == Outcome::Settled && victim.reason == Reason::UnsupportedCodec,
                      "reported nonretryable pressure refusal did not settle with its reason");
    coordinator.drain();
    return failures;
}

int durable_record_delete_preserves_survivors() {
    const auto root = std::filesystem::temp_directory_path() /
        ("ninfer-delete-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{root};
    const std::string session(64, 'd');
    nlohmann::json manifest;
    std::ifstream(std::filesystem::path(__FILE__).parent_path() / "fixtures/durable/manifest-text.json")
        >> manifest;
    const auto fingerprint = manifest.at("runtime_fingerprint");
    SessionCheckpointStoreOptions options{.root = root, .origin_mac_key = std::string(32, 'k'),
                                          .require_origin_auth = true};
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
                    .stats = {.frontier_tokens = 1, .payload_bytes = 1, .points = 1, .states = 1}};
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
        [&](const auto& survivors) { published = survivors; return true; }, &refusal, &disk) &&
        published && published->records.size() == 2 && published->records.front().id == "resp_archived" &&
        published->latest_response_id == "resp_child",
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
} // namespace

int main() {
    int failures = queue_coalescing();
    failures += pressure_callback_does_not_wait_for_restore_commit();
    failures += automatic_retry_requires_matching_progress();
    failures += automatic_retry_budget_and_terminal_refusal();
    failures += durable_record_delete_preserves_survivors();
    failures += durable_delete_retains_disk_only_records();
    if (!failures) { std::cout << "automatic checkpoint remediation tests passed\n"; }
    return failures ? 1 : 0;
}
