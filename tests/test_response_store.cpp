#include "serve/response_store.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace ninfer::serve;

int check(bool condition, const std::string& message) {
    if (condition) { return 0; }
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

ChatTurn text_turn(std::string role, std::string text) {
    ChatTurn turn;
    turn.role = std::move(role);
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
        append_response_context({}, {text_turn("user", "one"), text_turn("assistant", "a")});
    const ResponseContext second =
        append_response_context(first, {text_turn("user", "two"), text_turn("assistant", "b")});
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
    const ResponseContext root = append_response_context({}, {text_turn("user", "root")});
    store.put(record("resp_1", root));
    const ResponseContext child = append_response_context(root, {text_turn("assistant", "child")});
    store.put(record("resp_2", child));
    (void)store.get_for_session("resp_1", std::nullopt); // resp_2 becomes least-recently used.
    store.put(record("resp_3", append_response_context(root, {text_turn("user", "fork")})));

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

int test_session_binding() {
    ResponseStore store(6, 1ULL << 20);
    StoredResponse scoped = record("resp_scoped", {});
    scoped.client_session_sha256 = std::string(64, 'a');
    store.put(std::move(scoped));
    StoredResponse scoped_latest = record("resp_scoped_latest", {});
    scoped_latest.client_session_sha256 = std::string(64, 'a');
    store.put(std::move(scoped_latest));
    StoredResponse other = record("resp_other", {});
    other.client_session_sha256 = std::string(64, 'b');
    store.put(std::move(other));
    store.put(record("resp_unscoped", {}));

    int failures = 0;
    failures += check(store.get_for_session("resp_scoped", std::string(64, 'a')) != nullptr,
                      "matching session could not continue a stored response");
    failures += check(store.get_for_session("resp_scoped", std::string(64, 'b')) == nullptr &&
                          store.get_for_session("resp_scoped", std::nullopt) == nullptr,
                      "stored response crossed its session boundary");
    failures += check(store.get_for_session("resp_unscoped", std::nullopt) != nullptr,
                      "unscoped response did not preserve unscoped continuation");
    failures += check(store.latest_response_id_for_session(std::string(64, 'a')) ==
                          std::optional<std::string>("resp_scoped_latest") &&
                          store.latest_response_id_for_session(std::string(64, 'b')) ==
                              std::optional<std::string>("resp_other"),
                      "latest response lookup crossed session insertion order");
    failures += check(
        store.erase_for_session("resp_scoped_latest", std::string(64, 'a')) &&
            store.latest_response_id_for_session(std::string(64, 'a')) ==
                std::optional<std::string>("resp_scoped") &&
            !store.latest_response_id_for_session(std::string(64, 'c')),
        "latest response lookup did not follow deletion without crossing sessions");
    return failures;
}
int test_oversized_record() {
    ResponseStore store(4, 256);
    StoredResponse large = record(
        "resp_large", append_response_context({}, {text_turn("user", std::string(1024, 'x'))}));
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
    failures += test_session_binding();
    failures += test_oversized_record();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
