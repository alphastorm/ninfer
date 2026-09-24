#pragma once

// Bounded storage for OpenAI Responses objects and their previous_response_id context DAG.

#include "serve/request.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ninfer::serve {

struct ResponseContextNode {
    std::shared_ptr<const ResponseContextNode> parent;
    std::vector<ChatTurn> turns;
    std::size_t owned_bytes      = 0;
    std::size_t cumulative_bytes = 0;
    std::size_t cumulative_turns = 0;
};

using ResponseContext = std::shared_ptr<const ResponseContextNode>;

ResponseContext append_response_context(ResponseContext parent, std::vector<ChatTurn> turns);
std::vector<ChatTurn> flatten_response_context(const ResponseContext& context);

struct StoredResponse {
    std::string id;
    std::string session_key;
    std::optional<std::string> client_session_sha256;
    std::optional<std::string> previous_response_id;
    nlohmann::json response;
    std::vector<nlohmann::json> input_items;
    ResponseContext context;
    bool preserve_thinking = false;
};

struct ResponseStoreSnapshot {
    std::string client_session_sha256;
    std::string latest_response_id;
    std::vector<StoredResponse> records;
};

// Where one response's record stands, for a save that must checkpoint it.
enum class ResponsePublicationState : std::uint8_t {
    // It is the session's newest stored response.
    Stored,
    // Its handler has not stored it yet and still may.
    Publishing,
    // It was never stored and no longer will be, or a newer response of the session was stored.
    Absent,
};

class ResponseStore;

// Held by the handler of a response it will store, from before the engine can catalogue the turn
// until the record is stored or the handler gives up. The engine can finish a turn well before its
// handler stores the reply - a streamed reply is stored only after its last delta reaches the
// client - so a save that must checkpoint the turn waits on this instead of guessing how long the
// store may trail the engine (alphastorm/omp-ninfer#45).
class ResponsePublication {
public:
    ResponsePublication() noexcept = default;
    ResponsePublication(ResponseStore& store, std::string response_id);
    ResponsePublication(ResponsePublication&& other) noexcept;
    ResponsePublication& operator=(ResponsePublication&& other) noexcept;
    ResponsePublication(const ResponsePublication&)            = delete;
    ResponsePublication& operator=(const ResponsePublication&) = delete;
    ~ResponsePublication();

    // Idempotent. ResponseStore::put() of the same id ends the publication as it stores it.
    void end() noexcept;

private:
    ResponseStore* store_ = nullptr;
    std::string response_id_;
};

class ResponseStore {
public:
    ResponseStore(std::size_t max_records, std::size_t max_bytes);

    // A different or absent session is deliberately indistinguishable from a missing response.
    // Returned immutable records remain valid if another request evicts or deletes their public
    // store entry. All public reads and deletes stay in one authenticated session namespace.
    std::shared_ptr<const StoredResponse>
    get_for_session(const std::string& id, const std::optional<std::string>& session_sha256);
    void put(StoredResponse response);
    bool erase_for_session(const std::string& id, const std::optional<std::string>& session_sha256);

    // Snapshots are insertion ordered, parent-before-child for a normal Responses lineage. Restore
    // publishes the complete session while holding the store lock; malformed snapshots are rejected
    // without exposing a partial lineage.
    [[nodiscard]] std::optional<ResponseStoreSnapshot>
    snapshot_session(std::string_view client_session_sha256) const;
    // Newest stored response id for the session without copying the lineage; used to
    // detect a checkpoint that already covers the session (redundant-save skip).
    [[nodiscard]] std::optional<std::string>
    latest_response_id(std::string_view client_session_sha256) const;
    [[nodiscard]] std::vector<std::string> session_digests() const;
    // Waits up to timeout while response_id is still being published, then reports where it
    // stands for the session.
    [[nodiscard]] ResponsePublicationState
    await_publication(std::string_view client_session_sha256, const std::string& response_id,
                      std::chrono::milliseconds timeout) const;
    // Replaces the complete lineage of snapshot.client_session_sha256: existing records owned by
    // that session are replacement input (same-session ID overlap is the partial-lineage repair
    // case; stale target records absent from the snapshot are removed), an incoming ID owned by
    // any other record fails closed, and unrelated sessions survive unless capacity evicts them.
    // Builds the complete replacement first, then invokes commit_external while holding the store
    // lock. A false result publishes neither side; a true result is followed only by no-throw
    // swaps, making the external Engine restore and Responses publication one transaction.
    [[nodiscard]] bool restore_session(ResponseStoreSnapshot snapshot,
                                       const std::function<bool()>& commit_external);

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t bytes() const;

private:
    friend class ResponsePublication;

    struct Entry {
        std::shared_ptr<const StoredResponse> response;
        std::list<std::string>::iterator lru;
        std::size_t envelope_bytes = 0;
        std::uint64_t sequence     = 0;
    };

    void retain_context_locked(const ResponseContext& context);
    void release_context_locked(const ResponseContext& context);
    void erase_locked(const std::string& id);
    void insert_locked(std::shared_ptr<const StoredResponse> response, std::size_t envelope_bytes,
                       std::uint64_t sequence);
    void begin_publication(const std::string& response_id);
    void end_publication(const std::string& response_id) noexcept;
    [[nodiscard]] const StoredResponse*
    newest_locked(std::string_view client_session_sha256) const noexcept;

    std::size_t max_records_ = 0;
    std::size_t max_bytes_   = 0;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> records_;
    std::list<std::string> lru_;
    std::unordered_map<const ResponseContextNode*, std::size_t> live_context_references_;
    std::size_t current_bytes_  = 0;
    std::uint64_t next_sequence_ = 1;
    std::unordered_set<std::string> publishing_;
    mutable std::condition_variable publication_changed_;
};

} // namespace ninfer::serve
