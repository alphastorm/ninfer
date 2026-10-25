#include "serve/openai_responses_store.h"

#include "serve/client_identity.h"
#include "serve/credential_compare.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace ninfer::serve {
namespace {

bool session_matches(const std::optional<std::string>& stored,
                     const std::optional<std::string>& presented) noexcept {
    return stored.has_value() == presented.has_value() &&
           (!stored || credential_equal(*stored, *presented));
}

std::size_t estimate_turn_bytes(const ChatTurn& turn) {
    std::size_t bytes = sizeof(ChatTurn) + turn.tool_call_id.size() + turn.reasoning_content.size();
    if (turn.tool_result_name) { bytes += turn.tool_result_name->size(); }
    for (const ContentPart& part : turn.content) {
        bytes += sizeof(ContentPart) + part.text.size() + part.type_raw.size() +
                 part.source.value.size() + part.source.media_type.size() +
                 part.source.bytes.size();
    }
    for (const ToolCall& call : turn.tool_calls) {
        bytes += sizeof(ToolCall) + call.id.size() + call.name.size() + call.arguments_json.size();
    }
    return bytes;
}

std::size_t record_envelope_bytes(const StoredOpenAIResponse& record) {
    std::size_t bytes = sizeof(StoredOpenAIResponse) + record.id.size() +
                        record.session_key.size() + record.response.dump().size();
    if (record.client_session_sha256) { bytes += record.client_session_sha256->size(); }
    if (record.previous_response_id) { bytes += record.previous_response_id->size(); }
    for (const nlohmann::json& item : record.input_items) {
        bytes += sizeof(nlohmann::json) + item.dump().size();
    }
    return bytes;
}

std::size_t checked_add(std::size_t left, std::size_t right, const char* label) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw std::overflow_error(label);
    }
    return left + right;
}

[[noreturn]] void throw_store_capacity() {
    ApiError error;
    error.status  = 500;
    error.type    = "server_error";
    error.code    = "response_store_capacity_exceeded";
    error.message = "response exceeds the configured local response store capacity";
    throw ApiException(std::move(error));
}

} // namespace

OpenAIResponseContext append_openai_response_context(OpenAIResponseContext parent,
                                                     std::vector<ChatTurn> turns) {
    auto node         = std::make_shared<OpenAIResponseContextNode>();
    node->parent      = std::move(parent);
    node->turns       = std::move(turns);
    node->owned_bytes = sizeof(OpenAIResponseContextNode);
    for (const ChatTurn& turn : node->turns) { node->owned_bytes += estimate_turn_bytes(turn); }
    node->cumulative_bytes =
        checked_add(node->owned_bytes, node->parent ? node->parent->cumulative_bytes : 0,
                    "response context byte accounting overflowed");
    node->cumulative_turns =
        checked_add(node->turns.size(), node->parent ? node->parent->cumulative_turns : 0,
                    "response context turn accounting overflowed");
    return node;
}

std::vector<ChatTurn> flatten_openai_response_context(const OpenAIResponseContext& context) {
    std::vector<const OpenAIResponseContextNode*> nodes;
    for (OpenAIResponseContext node = context; node != nullptr; node = node->parent) {
        nodes.push_back(node.get());
    }
    std::vector<ChatTurn> turns;
    turns.reserve(context ? context->cumulative_turns : 0);
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        turns.insert(turns.end(), (*it)->turns.begin(), (*it)->turns.end());
    }
    return turns;
}

OpenAIResponsesStore::OpenAIResponsesStore(std::size_t max_records, std::size_t max_bytes)
    : max_records_(max_records), max_bytes_(max_bytes) {
    if (max_records_ == 0 || max_bytes_ == 0) {
        throw std::invalid_argument("response store limits must be positive");
    }
}

std::shared_ptr<const StoredOpenAIResponse>
OpenAIResponsesStore::get_for_session(const std::string& id,
                                      const std::optional<std::string>& session_sha256) {
    std::lock_guard lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end() ||
        !session_matches(found->second.response->client_session_sha256, session_sha256)) {
        return {};
    }
    lru_.splice(lru_.begin(), lru_, found->second.lru);
    return found->second.response;
}

void OpenAIResponsesStore::put(StoredOpenAIResponse response) {
    if (response.id.empty() || response.session_key.empty() ||
        response.session_key.size() > kMaximumContextCacheSessionKeyBytes ||
        !response.response.is_object()) {
        throw std::invalid_argument(
            "stored response must have an id, bounded session key and object body");
    }
    if (response.client_session_sha256) {
        (void)parse_client_identity_sha256(*response.client_session_sha256, "ninfer_session");
    }
    const std::size_t envelope_bytes = record_envelope_bytes(response);
    const std::size_t context_bytes  = response.context ? response.context->cumulative_bytes : 0;
    if (envelope_bytes > max_bytes_ || context_bytes > max_bytes_ - envelope_bytes) {
        throw_store_capacity();
    }

    auto owned = std::make_shared<const StoredOpenAIResponse>(std::move(response));
    std::lock_guard lock(mutex_);
    if (records_.contains(owned->id)) {
        throw std::logic_error("duplicate response id in response store");
    }
    const std::uint64_t order = owned->publication_order != 0 ? owned->publication_order
                                                              : next_order_;
    if (order == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("response publication order exhausted");
    }
    auto session = sessions_.end();
    if (owned->client_session_sha256) {
        session = sessions_.try_emplace(*owned->client_session_sha256).first;
    }
    try {
        insert_locked(owned, envelope_bytes, order, true);
    } catch (...) {
        if (session != sessions_.end()) { prune_revision_locked(session->first); }
        throw;
    }
    next_order_ = std::max(next_order_, order + 1);
    if (session != sessions_.end()) {
        ++session->second.records;
        ++session->second.revision;
    }
    // Publication ends atomically with insertion; a waiter cannot observe an intermediate miss.
    end_publication_locked(owned->id);
    publication_changed_.notify_all();

    while (records_.size() > max_records_ || current_bytes_ > max_bytes_) {
        if (lru_.empty()) { throw std::logic_error("response store LRU is empty"); }
        auto victim = std::prev(lru_.end());
        if (*victim == owned->id) {
            if (victim == lru_.begin()) { throw_store_capacity(); }
            --victim;
        }
        const std::string victim_id = *victim;
        erase_locked(victim_id);
    }
}

void OpenAIResponsesStore::insert_locked(std::shared_ptr<const StoredOpenAIResponse> response,
                                         std::size_t envelope_bytes, std::uint64_t order,
                                         bool live) {
    lru_.push_front(response->id);
    try {
        records_.emplace(response->id, Entry{response, lru_.begin(), envelope_bytes, order, live});
    } catch (...) {
        lru_.pop_front();
        throw;
    }
    bool accounted = false;
    try {
        current_bytes_ = checked_add(current_bytes_, envelope_bytes,
                                     "response store byte accounting overflowed");
        accounted = true;
        retain_context_locked(response->context);
    } catch (...) {
        if (accounted) { current_bytes_ -= envelope_bytes; }
        records_.erase(response->id);
        lru_.pop_front();
        throw;
    }
}

bool OpenAIResponsesStore::erase_for_session(const std::string& id,
                                             const std::optional<std::string>& session_sha256) {
    std::lock_guard lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end() ||
        !session_matches(found->second.response->client_session_sha256, session_sha256)) {
        return false;
    }
    erase_locked(id);
    return true;
}

bool OpenAIResponsesStore::erase_for_session_durably(
    const std::string& id, const std::string& session,
    const std::function<bool(const std::optional<OpenAIResponseStoreSnapshot>&)>& publish,
    runtime::DurableReason* refusal, const OpenAIResponseStoreSnapshot* persisted) {
    using Reason = runtime::DurableReason;
    if (refusal) { *refusal = Reason::None; }
    std::lock_guard lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end() || !session_matches(found->second.response->client_session_sha256, session)) {
        return false;
    }
    auto snapshot = snapshot_locked(session, id);
    if (snapshot && newest_locked(session)->response->id == id) {
        snapshot->replay_only = true;
    }
    // LRU eviction is not DELETE. Retain disk-only survivors too, so deleting the last
    // addressable RAM record cannot accidentally erase an otherwise durable session.
    if (persisted) {
        if (!credential_equal(persisted->client_session_sha256, session)) {
            if (refusal) { *refusal = Reason::InvalidManifest; }
            return false;
        }
        std::vector<StoredOpenAIResponse> retained;
        for (const auto& record : persisted->records) {
            if (record.id == id) { continue; }
            if (!snapshot || std::none_of(snapshot->records.begin(), snapshot->records.end(),
                [&](const auto& live) { return live.id == record.id; })) { retained.push_back(record); }
        }
        if (!retained.empty()) {
            if (retained.size() + (snapshot ? snapshot->records.size() : 0) > max_records_) {
                if (refusal) { *refusal = Reason::Capacity; }
                return false;
            }
            if (!snapshot) {
                snapshot.emplace();
                snapshot->client_session_sha256 = session;
                snapshot->latest_response_id = retained.back().id;
                snapshot->replay_only = true;
            } else {
                for (auto& record : snapshot->records) { retained.push_back(std::move(record)); }
            }
            snapshot->records = std::move(retained);
            for (std::size_t i = 0; i < snapshot->records.size(); ++i) {
                snapshot->records[i].publication_order = i + 1;
            }
        }
        if (snapshot && persisted->replay_only &&
            snapshot->latest_response_id == persisted->latest_response_id) { snapshot->replay_only = true; }
    }
    std::string replay_endpoint = snapshot && snapshot->replay_only ? snapshot->latest_response_id : "";
    if (!publish || !publish(snapshot)) { return false; }
    // Publication may wait for the Engine worker. Its pressure callback never takes this mutex.
    // All allocating work and durable acknowledgement precede the invariant-preserving erase.
    erase_locked(id);
    if (const auto revision = sessions_.find(session); revision != sessions_.end()) {
        revision->second.replay_endpoint.swap(replay_endpoint);
    }
    return true;
}

std::size_t OpenAIResponsesStore::size() const {
    std::lock_guard lock(mutex_);
    return records_.size();
}

std::size_t OpenAIResponsesStore::bytes() const {
    std::lock_guard lock(mutex_);
    return current_bytes_;
}

void OpenAIResponsesStore::retain_context_locked(const OpenAIResponseContext& context) {
    if (!context) { return; }
    const auto root = live_context_references_.find(context.get());
    if (root != live_context_references_.end()) {
        if (root->second == std::numeric_limits<std::size_t>::max()) {
            throw std::overflow_error("response context reference count overflowed");
        }
        ++root->second;
        return;
    }

    std::vector<OpenAIResponseContext> new_nodes;
    OpenAIResponseContext cursor = context;
    while (cursor && !live_context_references_.contains(cursor.get())) {
        new_nodes.push_back(cursor);
        cursor = cursor->parent;
    }

    std::size_t inserted         = 0;
    const auto rollback_inserted = [&] {
        while (inserted != 0) {
            --inserted;
            current_bytes_ -= new_nodes[inserted]->owned_bytes;
            live_context_references_.erase(new_nodes[inserted].get());
        }
    };
    try {
        for (const OpenAIResponseContext& node : new_nodes) {
            const std::size_t next_bytes = checked_add(current_bytes_, node->owned_bytes,
                                                       "response store byte accounting overflowed");
            const auto [entry, added]    = live_context_references_.emplace(node.get(), 1);
            (void)entry;
            if (!added) { throw std::logic_error("response context activation raced itself"); }
            current_bytes_ = next_bytes;
            ++inserted;
        }
    } catch (...) {
        rollback_inserted();
        throw;
    }

    if (cursor) {
        const auto existing = live_context_references_.find(cursor.get());
        if (existing == live_context_references_.end()) {
            rollback_inserted();
            throw std::logic_error("response context activation lost its live parent");
        }
        if (existing->second == std::numeric_limits<std::size_t>::max()) {
            rollback_inserted();
            throw std::overflow_error("response context reference count overflowed");
        }
        ++existing->second;
    }
}

void OpenAIResponsesStore::release_context_locked(const OpenAIResponseContext& context) {
    for (OpenAIResponseContext node = context; node != nullptr; node = node->parent) {
        const auto found = live_context_references_.find(node.get());
        if (found == live_context_references_.end() || found->second == 0) {
            throw std::logic_error("response context release lost its live node");
        }
        if (found->second != 1) {
            --found->second;
            return;
        }
        if (current_bytes_ < node->owned_bytes) {
            throw std::logic_error("response context byte accounting underflowed");
        }
        current_bytes_ -= node->owned_bytes;
        live_context_references_.erase(found);
    }
}

void OpenAIResponsesStore::erase_locked(const std::string& id) {
    const auto found = records_.find(id);
    if (found == records_.end()) { return; }
    auto session = sessions_.end();
    if (found->second.response->client_session_sha256) {
        session = sessions_.find(*found->second.response->client_session_sha256);
    }
    if (current_bytes_ < found->second.envelope_bytes) {
        throw std::logic_error("response envelope byte accounting underflowed");
    }
    current_bytes_ -= found->second.envelope_bytes;
    release_context_locked(found->second.response->context);
    lru_.erase(found->second.lru);
    records_.erase(found);
    if (session != sessions_.end()) {
        --session->second.records;
        ++session->second.revision;
        prune_revision_locked(session->first);
    }
    publication_changed_.notify_all();
}

const OpenAIResponsesStore::Entry*
OpenAIResponsesStore::newest_locked(std::string_view session) const noexcept {
    const Entry* newest = nullptr;
    for (const auto& [id, entry] : records_) {
        (void)id;
        if (entry.response->client_session_sha256 &&
            credential_equal(*entry.response->client_session_sha256, session) &&
            (!newest || std::tie(entry.live, entry.order) >
                            std::tie(newest->live, newest->order))) {
            newest = &entry;
        }
    }
    return newest;
}

std::optional<OpenAIResponseStoreSnapshot>
OpenAIResponsesStore::snapshot_session(std::string_view session) const {
    std::lock_guard lock(mutex_);
    return snapshot_locked(session);
}

std::optional<OpenAIResponseStoreSnapshot>
OpenAIResponsesStore::snapshot_locked(std::string_view session, std::string_view excluded) const {
    std::vector<const Entry*> ordered;
    for (const auto& [id, entry] : records_) {
        if (id != excluded && entry.response->client_session_sha256 &&
            credential_equal(*entry.response->client_session_sha256, session)) {
            ordered.push_back(&entry);
        }
    }
    if (ordered.empty()) { return std::nullopt; }
    std::sort(ordered.begin(), ordered.end(), [](const Entry* a, const Entry* b) {
        return std::tie(a->live, a->order) < std::tie(b->live, b->order);
    });
    OpenAIResponseStoreSnapshot snapshot;
    snapshot.client_session_sha256.assign(session);
    snapshot.latest_response_id = ordered.back()->response->id;
    const auto revision = sessions_.find(snapshot.client_session_sha256);
    snapshot.replay_only = revision != sessions_.end() &&
                           revision->second.replay_endpoint == snapshot.latest_response_id;
    snapshot.records.reserve(ordered.size());
    for (const Entry* entry : ordered) {
        snapshot.records.push_back(*entry->response);
        snapshot.records.back().publication_order = snapshot.records.size();
    }
    return snapshot;
}

std::optional<std::string>
OpenAIResponsesStore::latest_response_id(std::string_view session) const {
    std::lock_guard lock(mutex_);
    const Entry* newest = newest_locked(session);
    return newest ? std::optional(newest->response->id) : std::nullopt;
}

std::vector<std::string> OpenAIResponsesStore::session_digests() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    result.reserve(sessions_.size());
    for (const auto& [session, state] : sessions_) {
        if (state.records != 0) { result.push_back(session); }
    }
    std::sort(result.begin(), result.end());
    return result;
}

ResponsePublicationState
OpenAIResponsesStore::await_publication(std::string_view session, const std::string& id,
                                        std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    const auto state = [&] {
        const Entry* newest = newest_locked(session);
        if (newest && newest->response->id == id) { return ResponsePublicationState::Stored; }
        const auto pending = publishing_.find(id);
        if (pending != publishing_.end() && pending->second &&
            credential_equal(*pending->second, session)) {
            return ResponsePublicationState::Publishing;
        }
        return ResponsePublicationState::Absent;
    };
    (void)publication_changed_.wait_for(lock, timeout, [&] {
        return state() != ResponsePublicationState::Publishing;
    });
    return state();
}

void OpenAIResponsesStore::prune_revision_locked(const std::string& session) noexcept {
    const auto found = sessions_.find(session);
    if (found != sessions_.end() && found->second.records == 0 &&
        found->second.publications == 0 && found->second.guards == 0) {
        sessions_.erase(found);
    }
}

void OpenAIResponsesStore::begin_publication(const std::string& id,
                                             const std::optional<std::string>& session) {
    std::lock_guard lock(mutex_);
    auto revision = sessions_.end();
    if (session) { revision = sessions_.try_emplace(*session).first; }
    try {
        if (!publishing_.emplace(id, session).second) {
            throw std::logic_error("duplicate response publication");
        }
    } catch (...) {
        if (session) { prune_revision_locked(*session); }
        throw;
    }
    if (revision != sessions_.end()) {
        ++revision->second.publications;
        ++revision->second.revision;
    }
    publication_changed_.notify_all();
}

void OpenAIResponsesStore::end_publication_locked(const std::string& id) noexcept {
    const auto pending = publishing_.find(id);
    if (pending == publishing_.end()) { return; }
    if (pending->second) {
        const auto revision = sessions_.find(*pending->second);
        if (revision != sessions_.end()) {
            --revision->second.publications;
            ++revision->second.revision;
            prune_revision_locked(revision->first);
        }
    }
    publishing_.erase(pending);
    publication_changed_.notify_all();
}

void OpenAIResponsesStore::end_publication(const std::string& id) noexcept {
    std::lock_guard lock(mutex_);
    end_publication_locked(id);
}

OpenAIResponsePublication::OpenAIResponsePublication(OpenAIResponsesStore& store, std::string id,
                                                     std::optional<std::string> session,
                                                     std::function<void()> on_end)
    : response_id_(std::move(id)), on_end_(std::move(on_end)) {
    store.begin_publication(response_id_, session);
    store_ = &store;
}
OpenAIResponsePublication::OpenAIResponsePublication(OpenAIResponsePublication&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)), response_id_(std::move(other.response_id_)),
      on_end_(std::move(other.on_end_)) {}
OpenAIResponsePublication&
OpenAIResponsePublication::operator=(OpenAIResponsePublication&& other) noexcept {
    if (this != &other) {
        end();
        store_ = std::exchange(other.store_, nullptr);
        response_id_ = std::move(other.response_id_);
        on_end_ = std::move(other.on_end_);
    }
    return *this;
}
OpenAIResponsePublication::~OpenAIResponsePublication() { end(); }
void OpenAIResponsePublication::end() noexcept {
    if (store_) {
        store_->end_publication(response_id_);
        // Wake checkpoint pressure only after releasing the Responses mutex.
        try { if (on_end_) { on_end_(); } } catch (...) {}
    }
    store_ = nullptr;
}

OpenAIResponseRestoreGuard OpenAIResponsesStore::begin_restore(std::string session) {
    (void)parse_client_identity_sha256(session, "ninfer_session");
    OpenAIResponseRestoreGuard guard;
    guard.session_ = std::move(session);
    std::lock_guard lock(mutex_);
    auto& revision = sessions_[guard.session_];
    ++revision.guards;
    guard.revision_ = revision.revision;
    guard.store_ = this;
    return guard;
}
void OpenAIResponsesStore::end_restore(const std::string& session) noexcept {
    std::lock_guard lock(mutex_);
    const auto revision = sessions_.find(session);
    if (revision != sessions_.end()) {
        --revision->second.guards;
        prune_revision_locked(revision->first);
    }
}
OpenAIResponseRestoreGuard::OpenAIResponseRestoreGuard(OpenAIResponseRestoreGuard&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)), session_(std::move(other.session_)),
      revision_(other.revision_) {}
OpenAIResponseRestoreGuard&
OpenAIResponseRestoreGuard::operator=(OpenAIResponseRestoreGuard&& other) noexcept {
    if (this != &other) {
        end();
        store_ = std::exchange(other.store_, nullptr);
        session_ = std::move(other.session_);
        revision_ = other.revision_;
    }
    return *this;
}
OpenAIResponseRestoreGuard::~OpenAIResponseRestoreGuard() { end(); }
void OpenAIResponseRestoreGuard::end() noexcept {
    if (store_) { store_->end_restore(session_); }
    store_ = nullptr;
}

bool OpenAIResponsesStore::restore_session(OpenAIResponseStoreSnapshot snapshot,
                                           const OpenAIResponseRestoreGuard& guard,
                                           const std::function<bool()>& commit_external,
                                           runtime::DurableReason* refusal) {
    using Reason = runtime::DurableReason;
    if (refusal) { *refusal = Reason::None; }
    const auto reject = [&](Reason reason) {
        if (refusal) { *refusal = reason; }
        return false;
    };
    if (guard.store_ != this || !credential_equal(guard.session_, snapshot.client_session_sha256) ||
        snapshot.records.empty() || !commit_external) {
        return reject(Reason::InvalidManifest);
    }
    if (snapshot.records.size() > max_records_) { return reject(Reason::Capacity); }
    std::unordered_set<std::string> ids;
    std::uint64_t previous_order = 0;
    for (const auto& record : snapshot.records) {
        if (!record.client_session_sha256 ||
            !credential_equal(*record.client_session_sha256, snapshot.client_session_sha256) ||
            record.id.empty() || record.session_key.empty() ||
            record.session_key.size() > kMaximumContextCacheSessionKeyBytes ||
            !record.response.is_object() || !ids.insert(record.id).second ||
            record.publication_order <= previous_order) {
            return reject(Reason::InvalidManifest);
        }
        previous_order = record.publication_order;
    }
    if (snapshot.latest_response_id != snapshot.records.back().id) {
        return reject(Reason::InvalidManifest);
    }

    std::lock_guard lock(mutex_);
    const auto revision = sessions_.find(guard.session_);
    if (revision == sessions_.end() || revision->second.revision != guard.revision_) {
        return reject(Reason::StaleRevision);
    }
    if (revision->second.publications != 0) { return reject(Reason::TransactionBusy); }
    const auto target = [&](const StoredOpenAIResponse& response) {
        return response.client_session_sha256 &&
               credential_equal(*response.client_session_sha256, snapshot.client_session_sha256);
    };
    // Revision equality only proves that nothing changed since capture. Capture itself may
    // follow a newer unsaved turn. Never use persisted numeric orders as a runtime clock.
    const auto* newest = newest_locked(snapshot.client_session_sha256);
    if (newest && newest->response->id != snapshot.latest_response_id) {
        return reject(Reason::StaleRevision);
    }
    for (const auto& [id, entry] : records_) {
        if (target(*entry.response) && !ids.contains(id)) { return reject(Reason::StaleRevision); }
    }
    for (const auto& record : snapshot.records) {
        const auto collision = records_.find(record.id);
        if (collision != records_.end() && !target(*collision->second.response)) {
            return reject(Reason::ForeignSessionCollision);
        }
    }
    try {
        OpenAIResponsesStore replacement(max_records_, max_bytes_);
        for (auto position = lru_.rbegin(); position != lru_.rend(); ++position) {
            const Entry& entry = records_.at(*position);
            if (!target(*entry.response)) {
                replacement.insert_locked(entry.response, entry.envelope_bytes,
                                            entry.order, entry.live);
            }
        }
        for (auto& record : snapshot.records) {
            const std::size_t envelope = record_envelope_bytes(record);
            const auto order = record.publication_order;
            replacement.insert_locked(std::make_shared<const StoredOpenAIResponse>(std::move(record)),
                                        envelope, order, false);
        }
        if (replacement.records_.size() > max_records_ ||
            replacement.current_bytes_ > max_bytes_) { return reject(Reason::Capacity); }
        std::string replay_endpoint = snapshot.replay_only ? snapshot.latest_response_id : "";
        // The caller retains its full K3 result. A false external commit must not be relabelled
        // as a store failure; refusal remains None and the K3 reason is authoritative.
        if (!commit_external()) { return false; }
        records_.swap(replacement.records_);
        lru_.swap(replacement.lru_);
        live_context_references_.swap(replacement.live_context_references_);
        std::swap(current_bytes_, replacement.current_bytes_);
        revision->second.records = snapshot.records.size();
        revision->second.replay_endpoint.swap(replay_endpoint);
        ++revision->second.revision;
        publication_changed_.notify_all();
        return true;
    } catch (const std::bad_alloc&) { return reject(Reason::Capacity); }
    catch (...) { return reject(Reason::InvalidManifest); }
}

runtime::DurableImportResult commit_openai_response_restore(
    OpenAIResponsesStore& responses, OpenAIResponseStoreSnapshot snapshot,
    const OpenAIResponseRestoreGuard& guard,
    const std::function<runtime::DurableImportResult()>& commit_engine) {
    using State = runtime::DurableImportState;
    using Reason = runtime::DurableReason;
    if (!commit_engine) { return {.state = State::PolicyDisabled, .reason = Reason::StoreDisabled}; }
    runtime::DurableImportResult result{.state = State::IdentityMismatch,
                                        .reason = Reason::StaleRevision};
    Reason refusal = Reason::None;
    const bool published = responses.restore_session(
        std::move(snapshot), guard, [&] {
            result = commit_engine();
            return result.state == State::Imported && result.reason == Reason::None;
        }, &refusal);
    if (published || refusal == Reason::None) { return result; }
    if (refusal == Reason::Capacity) {
        return {.state = State::CapacityRefused, .reason = refusal};
    }
    if (refusal == Reason::InvalidManifest) {
        return {.state = State::Corrupt, .reason = refusal};
    }
    return {.state = State::IdentityMismatch, .reason = refusal};
}

} // namespace ninfer::serve
