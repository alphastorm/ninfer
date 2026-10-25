#pragma once

// Bounded Responses DAG. Session capabilities gate every public read/delete. Disk restore commits
// an Engine transaction under this store's lock, then publishes only preallocated no-throw swaps.

#include "serve/request.h"
#include "runtime/contract/durable_engine_api.h"

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
#include <vector>

namespace ninfer::serve {

struct OpenAIResponseContextNode {
    std::shared_ptr<const OpenAIResponseContextNode> parent;
    std::vector<ChatTurn> turns;
    std::size_t owned_bytes = 0;
    std::size_t cumulative_bytes = 0;
    std::size_t cumulative_turns = 0;
};

using OpenAIResponseContext = std::shared_ptr<const OpenAIResponseContextNode>;
OpenAIResponseContext append_openai_response_context(OpenAIResponseContext parent,
                                                     std::vector<ChatTurn> turns);
std::vector<ChatTurn> flatten_openai_response_context(const OpenAIResponseContext& context);

struct StoredOpenAIResponse {
    std::string id;
    std::string session_key;
    std::optional<std::string> client_session_sha256;
    std::optional<std::string> previous_response_id;
    nlohmann::json response;
    std::vector<nlohmann::json> input_items;
    OpenAIResponseContext context;
    std::optional<bool> preserve_thinking;
    // Live records use Engine submission order, never handler completion order. Snapshots encode
    // only relative lineage order; restore starts a new runtime publication epoch.
    std::uint64_t publication_order = 0;
};

struct OpenAIResponseStoreSnapshot {
    std::string client_session_sha256;
    std::string latest_response_id;
    std::vector<StoredOpenAIResponse> records;
};

enum class ResponsePublicationState : std::uint8_t { Stored, Publishing, Absent };
class OpenAIResponsesStore;

class OpenAIResponsePublication {
public:
    OpenAIResponsePublication() noexcept = default;
    OpenAIResponsePublication(OpenAIResponsesStore& store, std::string response_id,
                              std::optional<std::string> session_sha256,
                              std::function<void()> on_end = {});
    OpenAIResponsePublication(OpenAIResponsePublication&& other) noexcept;
    OpenAIResponsePublication& operator=(OpenAIResponsePublication&& other) noexcept;
    OpenAIResponsePublication(const OpenAIResponsePublication&) = delete;
    OpenAIResponsePublication& operator=(const OpenAIResponsePublication&) = delete;
    ~OpenAIResponsePublication();
    void end() noexcept;
private:
    OpenAIResponsesStore* store_ = nullptr;
    std::string response_id_;
    std::function<void()> on_end_;
};

// Capture before disk loading. The guard pins only a revision entry, not the store mutex. A
// publication, put, deletion or eviction invalidates it, including deletion of the last record.
class OpenAIResponseRestoreGuard {
public:
    OpenAIResponseRestoreGuard() noexcept = default;
    OpenAIResponseRestoreGuard(OpenAIResponseRestoreGuard&& other) noexcept;
    OpenAIResponseRestoreGuard& operator=(OpenAIResponseRestoreGuard&& other) noexcept;
    OpenAIResponseRestoreGuard(const OpenAIResponseRestoreGuard&) = delete;
    OpenAIResponseRestoreGuard& operator=(const OpenAIResponseRestoreGuard&) = delete;
    ~OpenAIResponseRestoreGuard();
private:
    friend class OpenAIResponsesStore;
    void end() noexcept;
    OpenAIResponsesStore* store_ = nullptr;
    std::string session_;
    std::uint64_t revision_ = 0;
};

class OpenAIResponsesStore {
public:
    OpenAIResponsesStore(std::size_t max_records, std::size_t max_bytes);
    // A foreign or omitted capability is deliberately indistinguishable from a missing ID.
    std::shared_ptr<const StoredOpenAIResponse>
    get_for_session(const std::string& id, const std::optional<std::string>& session_sha256);
    void put(StoredOpenAIResponse response);
    bool erase_for_session(const std::string& id,
                            const std::optional<std::string>& session_sha256);
    // Publish the survivor snapshot before the no-allocation RAM erase. An endpoint with
    // survivors cannot be deleted without a matching older native continuation: refuse it.
    [[nodiscard]] bool erase_for_session_durably(
        const std::string& id, const std::string& session_sha256,
        const std::function<bool(const std::optional<OpenAIResponseStoreSnapshot>&)>& publish,
        runtime::DurableReason* refusal = nullptr);

    [[nodiscard]] std::optional<OpenAIResponseStoreSnapshot>
    snapshot_session(std::string_view session_sha256) const;
    [[nodiscard]] std::optional<std::string> latest_response_id(std::string_view session_sha256) const;
    [[nodiscard]] std::vector<std::string> session_digests() const;
    [[nodiscard]] ResponsePublicationState
    await_publication(std::string_view session_sha256, const std::string& response_id,
                       std::chrono::milliseconds timeout) const;

    [[nodiscard]] OpenAIResponseRestoreGuard begin_restore(std::string session_sha256);
    // Callback commits the prepared Engine import, including its captured session-epoch check.
    // False/throw publishes neither Responses side; capacity never evicts unrelated sessions.
    [[nodiscard]] bool restore_session(OpenAIResponseStoreSnapshot snapshot,
                                        const OpenAIResponseRestoreGuard& guard,
                                        const std::function<bool()>& commit_external,
                                        runtime::DurableReason* refusal = nullptr);
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t bytes() const;

private:
    friend class OpenAIResponsePublication;
    friend class OpenAIResponseRestoreGuard;
    struct Entry {
        std::shared_ptr<const StoredOpenAIResponse> response;
        std::list<std::string>::iterator lru;
        std::size_t envelope_bytes = 0;
        std::uint64_t order = 0;
        bool live = true;
    };
    struct SessionRevision {
        std::uint64_t revision = 0;
        std::size_t records = 0;
        std::size_t publications = 0;
        std::size_t guards = 0;
    };
    void retain_context_locked(const OpenAIResponseContext& context);
    void release_context_locked(const OpenAIResponseContext& context);
    void erase_locked(const std::string& id);
    void insert_locked(std::shared_ptr<const StoredOpenAIResponse> response,
                        std::size_t envelope_bytes, std::uint64_t order, bool live);
    void begin_publication(const std::string& id, const std::optional<std::string>& session);
    void end_publication(const std::string& id) noexcept;
    void end_publication_locked(const std::string& id) noexcept;
    void end_restore(const std::string& session) noexcept;
    void prune_revision_locked(const std::string& session) noexcept;
    [[nodiscard]] const Entry* newest_locked(std::string_view session) const noexcept;
    [[nodiscard]] std::optional<OpenAIResponseStoreSnapshot>
    snapshot_locked(std::string_view session, std::string_view excluded = {}) const;

    std::size_t max_records_;
    std::size_t max_bytes_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> records_;
    std::list<std::string> lru_;
    std::unordered_map<const OpenAIResponseContextNode*, std::size_t> live_context_references_;
    std::size_t current_bytes_ = 0;
    std::uint64_t next_order_ = 1;
    std::unordered_map<std::string, std::optional<std::string>> publishing_;
    std::unordered_map<std::string, SessionRevision> sessions_;
    mutable std::condition_variable publication_changed_;
};

// K4 hook: the service captures the Engine epoch and a Responses guard before loading. It may
// restore only the requested endpoint when required_response_id is present. No callback means
// persistence is unavailable, not a successful restore.
using OpenAIResponseRestoreCallback =
    std::function<runtime::DurableImportResult(
        std::string_view session_sha256, std::optional<std::string_view> required_response_id,
        OpenAIResponsesStore& responses)>;

// The prepared Engine commit is invoked only after the complete replacement has been built.
// Store-local admission/race failures retain their own K7 reason; a native refusal is not
// reclassified as corruption. The caller still owns/aborts its prepared transaction.
[[nodiscard]] runtime::DurableImportResult commit_openai_response_restore(
    OpenAIResponsesStore& responses, OpenAIResponseStoreSnapshot snapshot,
    const OpenAIResponseRestoreGuard& guard,
    const std::function<runtime::DurableImportResult()>& commit_engine);

} // namespace ninfer::serve
