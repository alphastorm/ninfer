#include "serve/openai_responses_store.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ninfer::serve;

int check(bool condition, const std::string& message) {
    if (condition) { return 0; }
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

ChatTurn text_turn(ninfer::ChatRole role, std::string text) {
    ChatTurn turn;
    turn.role = role;
    ContentPart part;
    part.kind     = ContentKind::Text;
    part.type_raw = "input_text";
    part.text     = std::move(text);
    turn.content.push_back(std::move(part));
    return turn;
}

StoredOpenAIResponse record(std::string id, OpenAIResponseContext context) {
    StoredOpenAIResponse value;
    value.id          = std::move(id);
    value.session_key = "session-" + value.id;
    value.response =
        nlohmann::json{{"id", value.id}, {"object", "response"}, {"status", "completed"}};
    value.input_items.push_back(nlohmann::json{{"id", "msg_" + value.id}, {"type", "message"}});
    value.context = std::move(context);
    return value;
}

int test_context_dag() {
    const OpenAIResponseContext first =
        append_openai_response_context({}, {text_turn(ninfer::ChatRole::User, "one"),
                                            text_turn(ninfer::ChatRole::Assistant, "a")});
    const OpenAIResponseContext second =
        append_openai_response_context(first, {text_turn(ninfer::ChatRole::User, "two"),
                                               text_turn(ninfer::ChatRole::Assistant, "b")});
    const std::vector<ChatTurn> flattened = flatten_openai_response_context(second);
    int failures                          = 0;
    failures += check(flattened.size() == 4, "context chain flattened all turns");
    failures += check(flattened[0].content[0].text == "one" && flattened[3].content[0].text == "b",
                      "context chain preserves chronological order");
    failures += check(second->parent.get() == first.get(), "context nodes share their parent");
    return failures;
}

int test_lru_and_delete() {
    OpenAIResponsesStore store(2, 1ULL << 20);
    const OpenAIResponseContext root =
        append_openai_response_context({}, {text_turn(ninfer::ChatRole::User, "root")});
    store.put(record("resp_1", root));
    const OpenAIResponseContext child =
        append_openai_response_context(root, {text_turn(ninfer::ChatRole::Assistant, "child")});
    store.put(record("resp_2", child));
    (void)store.get_for_session("resp_1", std::nullopt); // resp_2 becomes least recently used.
    store.put(record("resp_3", append_openai_response_context(
                                   root, {text_turn(ninfer::ChatRole::User, "fork")})));

    int failures = 0;
    failures += check(store.get_for_session("resp_1", std::nullopt) != nullptr, "get refreshes LRU recency");
    failures += check(store.get_for_session("resp_2", std::nullopt) == nullptr, "least-recent response evicted");
    failures += check(store.get_for_session("resp_3", std::nullopt) != nullptr, "new response retained");
    failures += check(store.size() == 2 && store.bytes() != 0, "store reports bounded usage");
    failures += check(store.erase_for_session("resp_1", std::nullopt), "stored response deleted");
    failures += check(!store.erase_for_session("resp_1", std::nullopt) &&
                          store.get_for_session("resp_1", std::nullopt) == nullptr,
                      "deleted response is no longer addressable");
    // The child/fork context owns a shared parent even when the parent's public
    // response entry is deleted.
    const auto fork = store.get_for_session("resp_3", std::nullopt);
    failures += check(fork && flatten_openai_response_context(fork->context).size() == 2,
                      "descendant context survives parent response deletion");
    return failures;
}

int test_oversized_record() {
    OpenAIResponsesStore store(4, 256);
    StoredOpenAIResponse large =
        record("resp_large", append_openai_response_context(
                                 {}, {text_turn(ninfer::ChatRole::User, std::string(1024, 'x'))}));
    std::string code;
    try {
        store.put(std::move(large));
    } catch (const ApiException& exception) { code = exception.error().code; }
    int failures = 0;
    failures += check(code == "response_store_capacity_exceeded",
                      "oversized response fails deterministically");
    failures += check(store.size() == 0, "oversized insertion does not mutate store");
    return failures;
}

StoredOpenAIResponse scoped_record(std::string id, const std::string& session,
                                   std::uint64_t order = 0) {
    auto value = record(std::move(id), append_openai_response_context(
        {}, {text_turn(ninfer::ChatRole::User, "go")}));
    value.client_session_sha256 = session;
    value.publication_order = order;
    return value;
}

int test_isolation_and_submission_order() {
    const std::string a(64, 'a'), b(64, 'b');
    OpenAIResponsesStore store(8, 1ULL << 20);
    store.put(scoped_record("resp_later_submission", a, 20));
    store.put(scoped_record("resp_slow_handler", a, 10));
    int failures = 0;
    failures += check(store.latest_response_id(a) == "resp_later_submission",
                      "handler completion order displaced a newer Engine submission");
    failures += check(!store.get_for_session("resp_later_submission", b) &&
                          !store.get_for_session("resp_later_submission", std::nullopt) &&
                          !store.get_for_session("missing", b),
                      "cross-session or omitted capability read private state");
    failures += check(!store.erase_for_session("resp_later_submission", b) &&
                          !store.erase_for_session("resp_later_submission", std::nullopt) &&
                          store.get_for_session("resp_later_submission", a),
                      "cross-session deletion changed the target record");
    const auto snapshot = store.snapshot_session(a);
    failures += check(snapshot && snapshot->records.size() == 2 &&
                          snapshot->records[0].id == "resp_slow_handler" &&
                          snapshot->records[0].publication_order == 1 &&
                          snapshot->records[1].publication_order == 2 &&
                          snapshot->latest_response_id == "resp_later_submission",
                      "snapshot did not preserve relative Engine publication order");
    return failures;
}

int test_publication_guard() {
    using namespace std::chrono_literals;
    const std::string session(64, 'c'), foreign(64, 'd');
    OpenAIResponsesStore store(8, 1ULL << 20);
    OpenAIResponsePublication slow(store, "resp_slow", session);
    int failures = 0;
    failures += check(store.await_publication(session, "resp_slow", 0ms) ==
                          ResponsePublicationState::Publishing &&
                          store.await_publication(foreign, "resp_slow", 0ms) ==
                          ResponsePublicationState::Absent,
                      "publication scope was absent or leaked to another session");
    std::thread handler([&] {
        std::this_thread::sleep_for(20ms);
        store.put(scoped_record("resp_slow", session, 1));
    });
    const auto state = store.await_publication(session, "resp_slow", 5s);
    handler.join();
    failures += check(state == ResponsePublicationState::Stored, "stored reply did not wake save");
    slow.end();
    {
        OpenAIResponsePublication abandoned(store, "resp_abandoned", session);
        OpenAIResponsePublication owner = std::move(abandoned);
        abandoned.end();
        failures += check(store.await_publication(session, "resp_abandoned", 0ms) ==
                              ResponsePublicationState::Publishing,
                          "moved-from guard ended another owner's publication");
    }
    failures += check(store.await_publication(session, "resp_abandoned", 5s) ==
                          ResponsePublicationState::Absent,
                      "abandoned reply remained publishing");
    store.put(scoped_record("resp_newer", session, 2));
    failures += check(store.await_publication(session, "resp_slow", 0ms) ==
                          ResponsePublicationState::Absent,
                      "superseded reply still reported the saved endpoint");
    return failures;
}

int test_atomic_restore_and_revision_guard() {
    const std::string a(64, 'a'), b(64, 'b');
    OpenAIResponsesStore source(8, 1ULL << 20);
    auto first = scoped_record("resp_first", b, 100);
    first.preserve_thinking = std::nullopt;
    auto second = scoped_record("resp_second", b, 200);
    second.previous_response_id = first.id;
    second.preserve_thinking = false;
    source.put(std::move(first));
    source.put(std::move(second));
    const auto snapshot = *source.snapshot_session(b);
    int failures = 0;
    OpenAIResponsesStore store(8, 1ULL << 20);
    store.put(scoped_record("resp_unrelated", a));
    bool engine_committed = false;
    auto guard = store.begin_restore(b); // captured before disk loading
    failures += check(!store.restore_session(snapshot, guard, [] { return false; }) &&
                          !store.get_for_session("resp_first", b),
                      "refused Engine import exposed a partial DAG");
    failures += check(!store.restore_session(snapshot, guard, []() -> bool {
                          throw std::runtime_error("import aborted");
                      }) && !store.latest_response_id(b),
                      "throwing Engine import exposed a partial DAG");
    failures += check(store.restore_session(snapshot, guard, [&] {
                          engine_committed = true;
                          return true;
                      }) && engine_committed && store.latest_response_id(b) == "resp_second" &&
                          store.get_for_session("resp_unrelated", a),
                      "Engine/Responses restore did not atomically replace only its own session");
    failures += check(!store.get_for_session("resp_first", b)->preserve_thinking &&
                          store.get_for_session("resp_second", b)->preserve_thinking == false,
                      "optional thinking null and explicit false collapsed on restore");
    store.put(scoped_record("resp_after_restart", b, 1));
    failures += check(store.latest_response_id(b) == "resp_after_restart",
                      "persisted order superseded a fresh runtime epoch");

    auto stale = store.begin_restore(b);
    failures += check(store.erase_for_session("resp_after_restart", b), "delete before load failed");
    engine_committed = false;
    ninfer::runtime::DurableReason refusal = ninfer::runtime::DurableReason::None;
    failures += check(!store.restore_session(snapshot, stale, [&] {
                          engine_committed = true; return true;
                      }, &refusal) && !engine_committed &&
                          refusal == ninfer::runtime::DurableReason::StaleRevision,
                      "stale disk load resurrected explicitly deleted lineage");
    OpenAIResponsesStore deleted(8, 1ULL << 20);
    deleted.put(scoped_record("resp_last", b));
    auto last_guard = deleted.begin_restore(b);
    (void)deleted.erase_for_session("resp_last", b);
    failures += check(!deleted.restore_session(snapshot, last_guard, [] { return true; }),
                      "deletion of the last record forgot the pinned revision");

    auto pending_guard = store.begin_restore(b);
    OpenAIResponsePublication pending(store, "resp_pending", b);
    failures += check(!store.restore_session(snapshot, pending_guard, [] { return true; }),
                      "disk restore replaced a publishing live response");
    auto in_flight_guard = store.begin_restore(b);
    failures += check(!store.restore_session(snapshot, in_flight_guard, [] { return true; }, &refusal) &&
                          refusal == ninfer::runtime::DurableReason::TransactionBusy,
                      "in-flight publication was not classified separately from stale loading");
    pending.end();
    failures += check(!store.restore_session(snapshot, pending_guard, [] { return true; }),
                      "abandoned new publication did not invalidate an earlier load");

    OpenAIResponsesStore capacity(2, 1ULL << 20);
    capacity.put(scoped_record("resp_resident", a));
    auto capacity_guard = capacity.begin_restore(b);
    failures += check(!capacity.restore_session(snapshot, capacity_guard, [] { return true; }, &refusal) &&
                          refusal == ninfer::runtime::DurableReason::Capacity &&
                          capacity.get_for_session("resp_resident", a) &&
                          !capacity.get_for_session("resp_first", b),
                      "restore capacity refusal evicted unrelated state or published a prefix");
    OpenAIResponsesStore collision(8, 1ULL << 20);
    collision.put(scoped_record("resp_first", a));
    auto collision_guard = collision.begin_restore(b);
    failures += check(!collision.restore_session(snapshot, collision_guard, [] { return true; }, &refusal) &&
                          refusal == ninfer::runtime::DurableReason::ForeignSessionCollision &&
                          collision.get_for_session("resp_first", a),
                      "foreign response-ID collision overwrote another capability");
    auto malformed = snapshot;
    malformed.latest_response_id = "missing";
    auto fresh = store.begin_restore(b);
    failures += check(!store.restore_session(malformed, fresh, [] { return true; }),
                      "malformed endpoint was published");
    malformed = snapshot;
    malformed.records[1].publication_order = malformed.records[0].publication_order;
    failures += check(!store.restore_session(malformed, fresh, [] { return true; }),
                      "ambiguous publication ordering was accepted");
    OpenAIResponsesStore rejected_epoch(8, 1ULL << 20);
    rejected_epoch.put(scoped_record("resp_second", b, 10));
    auto epoch_guard = rejected_epoch.begin_restore(b);
    bool commit_attempted = false;
    failures += check(!rejected_epoch.restore_session(snapshot, epoch_guard, [&] {
                          commit_attempted = true; // Engine's captured epoch has gone stale.
                          return false;
                      }, &refusal) && commit_attempted &&
                          refusal == ninfer::runtime::DurableReason::None &&
                          rejected_epoch.latest_response_id(b) == "resp_second" &&
                          !rejected_epoch.get_for_session("resp_first", b),
                      "Engine epoch refusal replaced newer live Responses or lost its K3 reason");
    return failures;
}

int test_restore_guard_captured_after_newer_turn() {
    const std::string session(64, 'e');
    OpenAIResponsesStore store(3, 1ULL << 20);
    store.put(scoped_record("resp_disk_1", session, 1));
    store.put(scoped_record("resp_disk_2", session, 2));
    store.put(scoped_record("resp_disk_3", session, 3));
    const auto disk = *store.snapshot_session(session);
    store.put(scoped_record("resp_new_unsaved", session, 4)); // real LRU miss for disk_1
    auto guard = store.begin_restore(session); // no mutation after capture
    bool engine_called = false;
    ninfer::runtime::DurableReason refusal{};
    const bool restored = store.restore_session(disk, guard, [&] {
        engine_called = true;
        return true; // captured Engine epoch is current too
    }, &refusal);
    return check(!restored && !engine_called &&
                     refusal == ninfer::runtime::DurableReason::StaleRevision &&
                     store.latest_response_id(session) == "resp_new_unsaved" &&
                     store.get_for_session("resp_new_unsaved", session) &&
                     !store.get_for_session("resp_disk_1", session),
                 "RAM-miss restore rolled back a newer turn that preceded guard capture");
}

} // namespace

int main() {
    int failures = 0;
    failures += test_context_dag();
    failures += test_lru_and_delete();
    failures += test_oversized_record();
    failures += test_isolation_and_submission_order();
    failures += test_publication_guard();
    failures += test_atomic_restore_and_revision_guard();
    failures += test_restore_guard_captured_after_newer_turn();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
