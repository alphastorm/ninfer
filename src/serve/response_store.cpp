#include "serve/response_store.h"

#include "serve/credential_compare.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace ninfer::serve {
namespace {

// Session ownership is a credential check: an attacker probing stored response ids with
// candidate session digests must not learn prefix proximity (alphastorm/ninfer#22).
bool session_matches(const std::optional<std::string>& stored,
                     const std::optional<std::string>& presented) {
    if (stored.has_value() != presented.has_value()) { return false; }
    return !stored || credential_equal(*stored, *presented);
}

std::size_t estimate_turn_bytes(const ChatTurn& turn) {
    std::size_t bytes = sizeof(ChatTurn) + turn.tool_call_id.size() + turn.reasoning_content.size();
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

std::size_t record_envelope_bytes(const StoredResponse& record) {
    std::size_t bytes = sizeof(StoredResponse) + record.id.size() + record.session_key.size() +
                        record.response.dump().size();
    if (record.previous_response_id) { bytes += record.previous_response_id->size(); }
    if (record.client_session_sha256) { bytes += record.client_session_sha256->size(); }
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

ResponseContext append_response_context(ResponseContext parent, std::vector<ChatTurn> turns) {
    auto node         = std::make_shared<ResponseContextNode>();
    node->parent      = std::move(parent);
    node->turns       = std::move(turns);
    node->owned_bytes = sizeof(ResponseContextNode);
    for (const ChatTurn& turn : node->turns) { node->owned_bytes += estimate_turn_bytes(turn); }
    node->cumulative_bytes =
        checked_add(node->owned_bytes, node->parent ? node->parent->cumulative_bytes : 0,
                    "response context byte accounting overflowed");
    node->cumulative_turns =
        checked_add(node->turns.size(), node->parent ? node->parent->cumulative_turns : 0,
                    "response context turn accounting overflowed");
    return node;
}

std::vector<ChatTurn> flatten_response_context(const ResponseContext& context) {
    std::vector<const ResponseContextNode*> nodes;
    for (ResponseContext node = context; node != nullptr; node = node->parent) {
        nodes.push_back(node.get());
    }
    std::vector<ChatTurn> turns;
    turns.reserve(context ? context->cumulative_turns : 0);
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        turns.insert(turns.end(), (*it)->turns.begin(), (*it)->turns.end());
    }
    return turns;
}

ResponseStore::ResponseStore(std::size_t max_records, std::size_t max_bytes)
    : max_records_(max_records), max_bytes_(max_bytes) {
    if (max_records_ == 0 || max_bytes_ == 0) {
        throw std::invalid_argument("response store limits must be positive");
    }
}

std::shared_ptr<const StoredResponse>
ResponseStore::get_for_session(const std::string& id,
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

void ResponseStore::put(StoredResponse response) {
    if (response.id.empty() || response.session_key.empty() ||
        response.session_key.size() > kMaximumContextCacheSessionKeyBytes ||
        !response.response.is_object()) {
        throw std::invalid_argument(
            "stored response must have an id, bounded session key and object body");
    }
    const std::size_t envelope_bytes = record_envelope_bytes(response);
    const std::size_t context_bytes  = response.context ? response.context->cumulative_bytes : 0;
    if (envelope_bytes > max_bytes_ || context_bytes > max_bytes_ - envelope_bytes) {
        throw_store_capacity();
    }

    auto owned = std::make_shared<const StoredResponse>(std::move(response));
    std::lock_guard lock(mutex_);
    if (records_.contains(owned->id)) {
        throw std::logic_error("duplicate response id in response store");
    }
    const std::uint64_t sequence = next_sequence_++;
    if (next_sequence_ == 0) { next_sequence_ = 1; }
    insert_locked(std::move(owned), envelope_bytes, sequence);

    while (records_.size() > max_records_ || current_bytes_ > max_bytes_) {
        if (lru_.empty()) { throw std::logic_error("response store LRU is empty"); }
        auto victim = std::prev(lru_.end());
        if (victim == lru_.begin() && records_.size() == 1) { throw_store_capacity(); }
        erase_locked(*victim);
    }
}

void ResponseStore::insert_locked(std::shared_ptr<const StoredResponse> response,
                                  std::size_t envelope_bytes, std::uint64_t sequence) {
    lru_.push_front(response->id);
    try {
        records_.emplace(response->id, Entry{response, lru_.begin(), envelope_bytes, sequence});
    } catch (...) {
        lru_.pop_front();
        throw;
    }
    bool envelope_accounted = false;
    try {
        current_bytes_     = checked_add(current_bytes_, envelope_bytes,
                                         "response store byte accounting overflowed");
        envelope_accounted = true;
        retain_context_locked(response->context);
    } catch (...) {
        if (envelope_accounted) { current_bytes_ -= envelope_bytes; }
        records_.erase(response->id);
        lru_.pop_front();
        throw;
    }
}

bool ResponseStore::erase_for_session(const std::string& id,
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

std::optional<ResponseStoreSnapshot>
ResponseStore::snapshot_session(std::string_view client_session_sha256) const {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::uint64_t, const StoredResponse*>> ordered;
    for (const auto& [id, entry] : records_) {
        (void)id;
        if (entry.response->client_session_sha256 &&
            credential_equal(*entry.response->client_session_sha256, client_session_sha256)) {
            ordered.emplace_back(entry.sequence, entry.response.get());
        }
    }
    if (ordered.empty()) { return std::nullopt; }
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    ResponseStoreSnapshot snapshot;
    snapshot.client_session_sha256.assign(client_session_sha256);
    snapshot.latest_response_id = ordered.back().second->id;
    snapshot.records.reserve(ordered.size());
    for (const auto& [sequence, record] : ordered) {
        (void)sequence;
        snapshot.records.push_back(*record);
    }
    return snapshot;
}

std::optional<std::string>
ResponseStore::latest_response_id(std::string_view client_session_sha256) const {
    std::lock_guard lock(mutex_);
    const StoredResponse* newest = nullptr;
    std::uint64_t newest_sequence = 0;
    for (const auto& [id, entry] : records_) {
        (void)id;
        if (entry.response->client_session_sha256 &&
            credential_equal(*entry.response->client_session_sha256, client_session_sha256) &&
            (newest == nullptr || entry.sequence > newest_sequence)) {
            newest          = entry.response.get();
            newest_sequence = entry.sequence;
        }
    }
    if (newest == nullptr) { return std::nullopt; }
    return newest->id;
}

std::vector<std::string> ResponseStore::session_digests() const {
    std::lock_guard lock(mutex_);
    std::unordered_set<std::string> unique;
    for (const auto& [id, entry] : records_) {
        (void)id;
        if (entry.response->client_session_sha256) {
            unique.insert(*entry.response->client_session_sha256);
        }
    }
    std::vector<std::string> out(unique.begin(), unique.end());
    std::sort(out.begin(), out.end());
    return out;
}

bool ResponseStore::restore_session(ResponseStoreSnapshot snapshot,
                                    const std::function<bool()>& commit_external) {
    if (snapshot.client_session_sha256.empty() || snapshot.latest_response_id.empty() ||
        snapshot.records.empty() || snapshot.records.size() > max_records_) {
        return false;
    }
    std::unordered_set<std::string> ids;
    bool has_latest = false;
    for (const StoredResponse& record : snapshot.records) {
        if (!record.client_session_sha256 ||
            *record.client_session_sha256 != snapshot.client_session_sha256 || record.id.empty() ||
            record.session_key.empty() ||
            record.session_key.size() > kMaximumContextCacheSessionKeyBytes ||
            !record.response.is_object() || !ids.insert(record.id).second) {
            return false;
        }
        has_latest = has_latest || record.id == snapshot.latest_response_id;
    }
    if (!has_latest) { return false; }

    // Preallocate every immutable record before taking the live store lock. The complete
    // replacement below validates capacity before commit_external, and only no-throw swaps follow
    // a successful external commit.
    std::vector<std::shared_ptr<const StoredResponse>> owned;
    std::vector<std::size_t> envelopes;
    try {
        owned.reserve(snapshot.records.size());
        envelopes.reserve(snapshot.records.size());
        for (StoredResponse& record : snapshot.records) {
            envelopes.push_back(record_envelope_bytes(record));
            owned.push_back(std::make_shared<const StoredResponse>(std::move(record)));
        }
    } catch (...) { return false; }

    std::lock_guard lock(mutex_);
    // Restore replaces the target session's complete lineage: an existing record OWNED BY THE
    // TARGET SESSION is replacement input (same-session ID overlap is the partial-lineage repair
    // case, and stale target records absent from the verified snapshot are removed), while an
    // incoming ID owned by any other record - another session or an unscoped one - stays a
    // fail-closed collision so an authenticated snapshot can never overwrite foreign state.
    const auto target_session = [&](const std::shared_ptr<const StoredResponse>& stored) {
        return stored->client_session_sha256 &&
               credential_equal(*stored->client_session_sha256, snapshot.client_session_sha256);
    };
    std::size_t unrelated_count = 0;
    for (const auto& [id, entry] : records_) {
        (void)id;
        if (!target_session(entry.response)) { ++unrelated_count; }
    }
    for (const auto& record : owned) {
        const auto found = records_.find(record->id);
        if (found != records_.end() && !target_session(found->second.response)) { return false; }
    }
    const std::size_t minimum_victims = unrelated_count + owned.size() > max_records_
                                            ? unrelated_count + owned.size() - max_records_
                                            : 0;
    try {
        for (std::size_t victim_count = minimum_victims; victim_count <= unrelated_count;
             ++victim_count) {
            ResponseStore replacement(max_records_, max_bytes_);
            std::size_t oldest_rank = 0;
            // insert_locked pushes to the MRU end, so replay survivors from oldest to newest.
            // Target-session records are replacement input, never capacity victims.
            for (auto position = lru_.rbegin(); position != lru_.rend(); ++position) {
                const Entry& entry = records_.at(*position);
                if (target_session(entry.response)) { continue; }
                if (oldest_rank++ < victim_count) { continue; }
                replacement.insert_locked(entry.response, entry.envelope_bytes, entry.sequence);
            }
            replacement.next_sequence_ = next_sequence_;
            for (std::size_t index = 0; index < owned.size(); ++index) {
                const std::uint64_t sequence = replacement.next_sequence_++;
                if (replacement.next_sequence_ == 0) { replacement.next_sequence_ = 1; }
                replacement.insert_locked(owned[index], envelopes[index], sequence);
            }
            if (replacement.records_.size() > max_records_ ||
                replacement.current_bytes_ > max_bytes_) {
                continue;
            }
            if (!commit_external()) { return false; }
            records_.swap(replacement.records_);
            lru_.swap(replacement.lru_);
            live_context_references_.swap(replacement.live_context_references_);
            std::swap(current_bytes_, replacement.current_bytes_);
            std::swap(next_sequence_, replacement.next_sequence_);
            return true;
        }
    } catch (...) { return false; }
    return false;
}

std::size_t ResponseStore::size() const {
    std::lock_guard lock(mutex_);
    return records_.size();
}

std::size_t ResponseStore::bytes() const {
    std::lock_guard lock(mutex_);
    return current_bytes_;
}

void ResponseStore::retain_context_locked(const ResponseContext& context) {
    if (!context) { return; }
    const auto root = live_context_references_.find(context.get());
    if (root != live_context_references_.end()) {
        if (root->second == std::numeric_limits<std::size_t>::max()) {
            throw std::overflow_error("response context reference count overflowed");
        }
        ++root->second;
        return;
    }

    std::vector<ResponseContext> new_nodes;
    ResponseContext cursor = context;
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
        for (const ResponseContext& node : new_nodes) {
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

void ResponseStore::release_context_locked(const ResponseContext& context) {
    for (ResponseContext node = context; node != nullptr; node = node->parent) {
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

void ResponseStore::erase_locked(const std::string& id) {
    const auto found = records_.find(id);
    if (found == records_.end()) { return; }
    if (current_bytes_ < found->second.envelope_bytes) {
        throw std::logic_error("response envelope byte accounting underflowed");
    }
    current_bytes_ -= found->second.envelope_bytes;
    release_context_locked(found->second.response->context);
    lru_.erase(found->second.lru);
    records_.erase(found);
}

} // namespace ninfer::serve
