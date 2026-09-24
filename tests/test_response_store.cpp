#include "serve/response_store.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <iostream>
#include <optional>
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

StoredResponse record(std::string id, ResponseContext context) {
    StoredResponse value;
    value.id          = std::move(id);
    value.session_key = "session-" + value.id;
    value.response =
        nlohmann::json{{"id", value.id}, {"object", "response"}, {"status", "completed"}};
    value.input_items.push_back(nlohmann::json{{"id", "msg_" + value.id}, {"type", "message"}});
    value.context = std::move(context);
    return value;
}

int test_context_dag() {
    const ResponseContext first =
        append_response_context({}, {text_turn(ninfer::ChatRole::User, "one"),
                                     text_turn(ninfer::ChatRole::Assistant, "a")});
    const ResponseContext second =
        append_response_context(first, {text_turn(ninfer::ChatRole::User, "two"),
                                        text_turn(ninfer::ChatRole::Assistant, "b")});
    const std::vector<ChatTurn> flattened = flatten_response_context(second);
    int failures                          = 0;
    failures += check(flattened.size() == 4, "context chain flattened all turns");
    failures += check(flattened[0].content[0].text == "one" && flattened[3].content[0].text == "b",
                      "context chain preserves chronological order");
    failures += check(second->parent.get() == first.get(), "context nodes share their parent");
    return failures;
}

int test_lru_and_delete() {
    ResponseStore store(2, 1ULL << 20);
    const ResponseContext root =
        append_response_context({}, {text_turn(ninfer::ChatRole::User, "root")});
    store.put(record("resp_1", root));
    const ResponseContext child =
        append_response_context(root, {text_turn(ninfer::ChatRole::Assistant, "child")});
    store.put(record("resp_2", child));
    (void)store.get_for_session("resp_1", std::nullopt); // resp_2 becomes least-recently used.
    store.put(record("resp_3",
                     append_response_context(root, {text_turn(ninfer::ChatRole::User, "fork")})));

    int failures = 0;
    failures += check(store.get_for_session("resp_1", std::nullopt) != nullptr,
                      "get refreshes LRU recency");
    failures += check(store.get_for_session("resp_2", std::nullopt) == nullptr,
                      "least-recent response evicted");
    failures +=
        check(store.get_for_session("resp_3", std::nullopt) != nullptr, "new response retained");
    failures += check(store.size() == 2 && store.bytes() != 0, "store reports bounded usage");
    failures += check(store.erase_for_session("resp_1", std::nullopt), "stored response deleted");
    failures += check(!store.erase_for_session("resp_1", std::nullopt) &&
                          store.get_for_session("resp_1", std::nullopt) == nullptr,
                      "deleted response is no longer addressable");
    // The child/fork context owns a shared parent even when the parent's public
    // response entry is deleted.
    const std::shared_ptr<const StoredResponse> fork =
        store.get_for_session("resp_3", std::nullopt);
    failures += check(fork && flatten_response_context(fork->context).size() == 2,
                      "descendant context survives parent response deletion");
    return failures;
}

int test_session_continuation_and_dag_deletion() {
    const std::string session_a(64, 'a');
    const std::string session_b(64, 'b');
    int failures = 0;
    ResponseStore store(8, 1ULL << 20);

    const ResponseContext first_context =
        append_response_context({}, {text_turn(ninfer::ChatRole::User, "first")});
    StoredResponse first        = record("resp_first", first_context);
    first.client_session_sha256 = session_a;
    store.put(std::move(first));

    const auto continuation_parent = store.get_for_session("resp_first", session_a);
    failures += check(continuation_parent != nullptr,
                      "same-session continuation could not read its parent");
    if (!continuation_parent) { return failures; }
    failures += check(!store.get_for_session("resp_first", session_b),
                      "cross-session continuation read another session's parent");
    failures += check(!store.get_for_session("resp_first", std::nullopt),
                      "session omission read a session-scoped continuation parent");

    const ResponseContext branch_one_context = append_response_context(
        continuation_parent->context, {text_turn(ninfer::ChatRole::User, "branch one")});
    const ResponseContext branch_two_context = append_response_context(
        continuation_parent->context, {text_turn(ninfer::ChatRole::User, "branch two")});
    StoredResponse branch_one        = record("resp_branch_one", branch_one_context);
    branch_one.client_session_sha256 = session_a;
    StoredResponse branch_two        = record("resp_branch_two", branch_two_context);
    branch_two.client_session_sha256 = session_a;
    store.put(std::move(branch_one));
    store.put(std::move(branch_two));

    failures += check(!store.erase_for_session("resp_first", session_b),
                      "cross-session delete removed another session's parent");
    failures += check(!store.erase_for_session("resp_first", std::nullopt),
                      "session omission deleted a session-scoped parent");
    failures += check(store.get_for_session("resp_first", session_a) != nullptr,
                      "rejected deletes mutated the scoped parent");
    failures += check(store.erase_for_session("resp_first", session_a),
                      "same-session parent deletion failed");
    failures += check(!store.get_for_session("resp_first", session_a),
                      "deleted parent remained available for continuation");
    failures += check(store.get_for_session("resp_branch_one", session_a) != nullptr &&
                          store.get_for_session("resp_branch_two", session_a) != nullptr,
                      "parent deletion removed stored branches");
    failures += check(!store.get_for_session("resp_branch_one", session_b),
                      "cross-session continuation read a surviving branch");
    return failures;
}

// A save that must checkpoint a finished turn waits on its reply's publication rather than a fixed
// guess: a reply stored well after the engine finished still reads as stored and wakes the waiter,
// an abandoned one reads as absent at once, and a newer stored reply supersedes an older one
// (council CR-20260924-ninfer-durable-evict-r1, daybreak-blue R2).
int test_publication_is_awaited_until_stored_or_abandoned() {
    using namespace std::chrono_literals;
    const std::string session(64, 'c');
    int failures = 0;
    ResponseStore store(8, 1ULL << 20);
    const auto session_record = [&](std::string id) {
        StoredResponse value = record(
            std::move(id), append_response_context({}, {text_turn(ninfer::ChatRole::User, "go")}));
        value.client_session_sha256 = session;
        return value;
    };

    ResponsePublication slow(store, "resp_slow");
    failures += check(store.await_publication(session, "resp_slow", 0ms) ==
                          ResponsePublicationState::Publishing,
                      "a reply its handler has yet to store did not read as publishing");
    std::thread handler([&] {
        std::this_thread::sleep_for(200ms);
        store.put(session_record("resp_slow"));
    });
    const auto started                   = std::chrono::steady_clock::now();
    const ResponsePublicationState state = store.await_publication(session, "resp_slow", 30s);
    const auto waited                    = std::chrono::steady_clock::now() - started;
    handler.join();
    failures += check(state == ResponsePublicationState::Stored,
                      "a reply stored after the waiter started was not seen as stored");
    failures += check(waited < 10s, "storing the reply did not wake the waiting save");
    slow.end();

    { ResponsePublication abandoned(store, "resp_abandoned"); }
    failures += check(store.await_publication(session, "resp_abandoned", 30s) ==
                          ResponsePublicationState::Absent,
                      "a reply its handler abandoned did not read as absent");

    ResponsePublication moved(store, "resp_moved");
    ResponsePublication owner = std::move(moved);
    moved.end();
    failures += check(store.await_publication(session, "resp_moved", 0ms) ==
                          ResponsePublicationState::Publishing,
                      "ending a moved-from publication ended its new owner's");
    owner.end();
    failures += check(store.await_publication(session, "resp_moved", 0ms) ==
                          ResponsePublicationState::Absent,
                      "the owner of a moved publication could not end it");

    store.put(session_record("resp_newer"));
    failures += check(store.await_publication(session, "resp_slow", 0ms) ==
                          ResponsePublicationState::Absent,
                      "a reply superseded by a newer stored one still read as the newest");
    return failures;
}

int test_oversized_record() {
    ResponseStore store(4, 256);
    StoredResponse large = record(
        "resp_large",
        append_response_context({}, {text_turn(ninfer::ChatRole::User, std::string(1024, 'x'))}));
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

int test_restore_preserves_unrelated_session_on_capacity_refusal() {
    const std::string resident_session(64, 'a');
    const std::string target_session(64, 'b');
    ResponseStore store(2, 1ULL << 20);

    StoredResponse resident =
        record("resp_resident",
               append_response_context({}, {text_turn(ninfer::ChatRole::User, "resident")}));
    resident.client_session_sha256 = resident_session;
    store.put(std::move(resident));

    StoredResponse first =
        record("resp_target_first",
               append_response_context({}, {text_turn(ninfer::ChatRole::User, "first")}));
    first.client_session_sha256 = target_session;
    StoredResponse second =
        record("resp_target_second",
               append_response_context({}, {text_turn(ninfer::ChatRole::User, "second")}));
    second.client_session_sha256 = target_session;
    second.previous_response_id  = first.id;
    ResponseStoreSnapshot target{.client_session_sha256 = target_session,
                                 .latest_response_id    = second.id,
                                 .records               = {std::move(first), std::move(second)}};

    bool external_committed = false;
    const bool restored     = store.restore_session(std::move(target), [&] {
        external_committed = true;
        return true;
    });
    int failures            = 0;
    failures += check(!restored && !external_committed,
                      "capacity refusal committed the external engine restore");
    failures += check(store.get_for_session("resp_resident", resident_session) != nullptr,
                      "target restore evicted an unrelated resident session");
    failures += check(store.get_for_session("resp_target_first", target_session) == nullptr &&
                          store.get_for_session("resp_target_second", target_session) == nullptr,
                      "refused target restore published a partial lineage");
    return failures;
}

} // namespace

int main() {
    int failures = 0;
    failures += test_context_dag();
    failures += test_lru_and_delete();
    failures += test_session_continuation_and_dag_deletion();
    failures += test_oversized_record();
    failures += test_restore_preserves_unrelated_session_on_capacity_refusal();
    failures += test_publication_is_awaited_until_stored_or_abandoned();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
