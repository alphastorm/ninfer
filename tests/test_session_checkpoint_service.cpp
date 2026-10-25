#include "serve/openai_responses_store.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace ninfer::serve;
using State = ninfer::runtime::DurableImportState;
using Reason = ninfer::runtime::DurableReason;
using Result = ninfer::runtime::DurableImportResult;

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

StoredOpenAIResponse record(std::string id, const std::string& session, std::uint64_t order) {
    StoredOpenAIResponse response;
    response.id = std::move(id);
    response.session_key = "http:" + session;
    response.client_session_sha256 = session;
    response.publication_order = order;
    response.response = {{"id", response.id}, {"object", "response"}, {"status", "completed"}};
    return response;
}

OpenAIResponseStoreSnapshot disk_snapshot(const std::string& session) {
    auto first = record("resp_first", session, 1);
    auto second = record("resp_endpoint", session, 2);
    second.previous_response_id = first.id;
    second.preserve_thinking = false;
    return {.client_session_sha256 = session, .latest_response_id = second.id,
             .records = {std::move(first), std::move(second)}};
}

int native_refusal_matrix() {
    const std::string session(64, 'a');
    const std::array refusals = {
        Result{.state = State::CapacityRefused, .reason = Reason::Capacity},
        Result{.state = State::Corrupt, .reason = Reason::InvalidIndex},
        Result{.state = State::Corrupt, .reason = Reason::PayloadSizeMismatch},
        Result{.state = State::Corrupt, .reason = Reason::PayloadDigestMismatch},
        Result{.state = State::IdentityMismatch, .reason = Reason::FingerprintMismatch},
        Result{.state = State::IdentityMismatch, .reason = Reason::StalePublication},
        Result{.state = State::PolicyDisabled, .reason = Reason::CacheDisabled},
        Result{.state = State::Unavailable, .reason = Reason::IoUnavailable}};
    int failures = 0;
    for (const auto& native : refusals) {
        OpenAIResponsesStore responses(8, 1ULL << 20);
        responses.put(record("resp_endpoint", session, 10));
        const auto original_bytes = responses.bytes();
        auto guard = responses.begin_restore(session);
        bool called = false;
        const auto result = commit_openai_response_restore(responses, disk_snapshot(session), guard,
            [&] { called = true; return native; });
        failures += check(called && result.state == native.state && result.reason == native.reason,
                           "native refusal lost its exact state/reason");
        failures += check(responses.size() == 1 && responses.bytes() == original_bytes &&
                               responses.latest_response_id(session) == "resp_endpoint" &&
                               !responses.get_for_session("resp_first", session) &&
                               responses.get_for_session("resp_endpoint", session),
                           "precommit native refusal changed live Responses state");
    }
    return failures;
}

int store_refusal_matrix() {
    const std::string session(64, 'b'), foreign(64, 'c');
    int failures = 0;
    const auto exercise = [&](OpenAIResponsesStore& store, const OpenAIResponseRestoreGuard& guard,
                               State expected_state, Reason expected_reason) {
        bool called = false;
        const auto result = commit_openai_response_restore(store, disk_snapshot(session), guard, [&] {
            called = true;
            return Result{.state = State::Imported, .reason = Reason::None};
        });
        return check(!called && result.state == expected_state && result.reason == expected_reason,
                      "store admission/race refusal invoked native commit or lost its reason");
    };
    OpenAIResponsesStore capacity(2, 1ULL << 20);
    capacity.put(record("resp_unrelated", foreign, 1));
    auto capacity_guard = capacity.begin_restore(session);
    failures += exercise(capacity, capacity_guard, State::CapacityRefused, Reason::Capacity);
    failures += check(capacity.size() == 1 && capacity.get_for_session("resp_unrelated", foreign),
                       "capacity refusal evicted another session");

    OpenAIResponsesStore revision(8, 1ULL << 20);
    revision.put(record("resp_deleted", session, 1));
    auto revision_guard = revision.begin_restore(session);
    (void)revision.erase_for_session("resp_deleted", session);
    failures += exercise(revision, revision_guard, State::IdentityMismatch, Reason::StaleRevision);
    failures += check(revision.size() == 0, "stale revision resurrected a deleted record");

    OpenAIResponsesStore busy(8, 1ULL << 20);
    OpenAIResponsePublication in_flight(busy, "resp_in_flight", session);
    auto busy_guard = busy.begin_restore(session);
    failures += exercise(busy, busy_guard, State::IdentityMismatch, Reason::TransactionBusy);

    OpenAIResponsesStore collision(8, 1ULL << 20);
    collision.put(record("resp_first", foreign, 1));
    auto collision_guard = collision.begin_restore(session);
    failures += exercise(collision, collision_guard, State::IdentityMismatch, Reason::ForeignSessionCollision);
    failures += check(collision.get_for_session("resp_first", foreign) &&
                           !collision.get_for_session("resp_first", session),
                       "foreign collision crossed the session capability boundary");
    return failures;
}

int successful_commit_and_namespace() {
    const std::string session(64, 'd'), foreign(64, 'e');
    OpenAIResponsesStore responses(8, 1ULL << 20);
    responses.put(record("resp_endpoint", session, 99));
    responses.put(record("resp_foreign", foreign, 1));
    auto guard = responses.begin_restore(session);
    bool committed = false;
    const Result native{.state = State::Imported, .reason = Reason::None,
        .stats = {.frontier_tokens = 128, .restored_tokens = 128, .payload_bytes = 4096,
                   .points = 2, .states = 2, .pages = 2}};
    const auto result = commit_openai_response_restore(responses, disk_snapshot(session), guard, [&] {
        committed = true;
        return native;
    });
    int failures = check(committed && result.state == State::Imported && result.reason == Reason::None &&
                              result.stats.restored_tokens == 128 && responses.size() == 3 &&
                              responses.latest_response_id(session) == "resp_endpoint" &&
                              responses.get_for_session("resp_endpoint", session) &&
                              responses.get_for_session("resp_foreign", foreign),
                          "successful native commit did not publish exactly the prepared replacement");
    failures += check(!responses.get_for_session("resp_endpoint", foreign) &&
                           !responses.get_for_session("resp_endpoint", std::nullopt) &&
                           !responses.erase_for_session("resp_endpoint", foreign) &&
                           !responses.erase_for_session("resp_endpoint", std::nullopt),
                       "scoped retrieve/delete exposed private restored state");
    failures += check(!responses.get_for_session("resp_first", session)->preserve_thinking &&
                           responses.get_for_session("resp_endpoint", session)->preserve_thinking == false,
                       "restore collapsed null and false thinking policies");
    responses.put(record("resp_fresh", session, 1));
    failures += check(responses.latest_response_id(session) == "resp_fresh",
                       "persisted order outranked a fresh runtime publication");
    failures += check(responses.erase_for_session("resp_endpoint", session) &&
                           !responses.get_for_session("resp_endpoint", session),
                       "authorized delete did not remove the restored response");
    return failures;
}

int publication_completion_wakes_off_lock() {
    const std::string session(64, 'f');
    OpenAIResponsesStore responses(4, 1ULL << 20);
    unsigned ended = 0;
    {
        OpenAIResponsePublication first(responses, "resp_abandoned", session, [&] {
            // Re-entering the store would deadlock if notification ran under its mutex.
            (void)responses.latest_response_id(session);
            ++ended;
        });
        auto moved = std::move(first);
        first.end();
        moved.end();
        moved.end();
    }
    return check(ended == 1, "publication completion did not notify exactly once off-lock");
}
} // namespace

int main() {
    int failures = 0;
    failures += native_refusal_matrix();
    failures += store_refusal_matrix();
    failures += successful_commit_and_namespace();
    failures += publication_completion_wakes_off_lock();
    if (failures == 0) { std::cout << "K4 host matrix passed\n"; }
    return failures == 0 ? 0 : 1;
}
