#include "serve/response_store.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <iostream>
#include <string>
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
    (void)store.get("resp_1"); // resp_2 becomes the least-recently used entry.
    store.put(record("resp_3",
                     append_response_context(root, {text_turn(ninfer::ChatRole::User, "fork")})));

    int failures = 0;
    failures += check(store.get("resp_1") != nullptr, "get refreshes LRU recency");
    failures += check(store.get("resp_2") == nullptr, "least-recent response evicted");
    failures += check(store.get("resp_3") != nullptr, "new response retained");
    failures += check(store.size() == 2 && store.bytes() != 0, "store reports bounded usage");
    failures += check(store.erase("resp_1"), "stored response deleted");
    failures += check(!store.erase("resp_1") && store.get("resp_1") == nullptr,
                      "deleted response is no longer addressable");
    // The child/fork context owns a shared parent even when the parent's public
    // response entry is deleted.
    const std::shared_ptr<const StoredResponse> fork = store.get("resp_3");
    failures += check(fork && flatten_response_context(fork->context).size() == 2,
                      "descendant context survives parent response deletion");
    return failures;
}

int test_session_lineage_isolation() {
    const std::string session_a(64, 'a');
    const std::string session_b(64, 'b');
    ResponseStore store(8, 1ULL << 20);

    const ResponseContext first_context =
        append_response_context({}, {text_turn(ninfer::ChatRole::User, "first")});
    StoredResponse first = record("resp_first", first_context);
    first.session_key = "http:" + session_a;
    first.client_session_sha256 = session_a;
    store.put(std::move(first));

    const std::shared_ptr<const StoredResponse> continued =
        store.get_for_session("resp_first", session_a);
    int failures = 0;
    failures += check(continued && continued->session_key == "http:" + session_a,
                      "first turn did not retain its stable Engine lineage");
    failures += check(!store.get_for_session("resp_first", session_b) &&
                          !store.get_for_session("resp_first", std::nullopt),
                      "another or absent session accessed the first turn");

    StoredResponse branch_one = record(
        "resp_branch_one",
        append_response_context(continued->context,
                                {text_turn(ninfer::ChatRole::Assistant, "one")}));
    branch_one.session_key = continued->session_key;
    branch_one.client_session_sha256 = session_a;
    store.put(std::move(branch_one));
    StoredResponse branch_two = record(
        "resp_branch_two",
        append_response_context(continued->context,
                                {text_turn(ninfer::ChatRole::Assistant, "two")}));
    branch_two.session_key = continued->session_key;
    branch_two.client_session_sha256 = session_a;
    store.put(std::move(branch_two));

    failures += check(!store.erase_for_session("resp_first", session_b),
                      "another session deleted the first turn");
    failures += check(store.erase_for_session("resp_first", session_a),
                      "owning session could not delete the first turn");
    const std::shared_ptr<const StoredResponse> surviving_one =
        store.get_for_session("resp_branch_one", session_a);
    const std::shared_ptr<const StoredResponse> surviving_two =
        store.get_for_session("resp_branch_two", session_a);
    failures += check(surviving_one && surviving_two &&
                          surviving_one->context->parent.get() == surviving_two->context->parent.get() &&
                          flatten_response_context(surviving_one->context).size() == 2,
                      "forks lost their shared context after parent deletion");
    failures += check(!store.get_for_session("resp_branch_one", session_b),
                      "branch isolation was not preserved");
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

} // namespace

int main() {
    int failures = 0;
    failures += test_context_dag();
    failures += test_lru_and_delete();
    failures += test_session_lineage_isolation();
    failures += test_oversized_record();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
