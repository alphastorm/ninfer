#include "models/qwen3_5/program/storage/checkpoint_codec.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

// These field lists are the K1 wire inventory, not native object-memory serialization.
namespace ninfer::runtime {
NLOHMANN_JSON_SERIALIZE_ENUM(DurablePointRole, {
    {DurablePointRole::Continuation, "continuation"},
    {DurablePointRole::InputReplay, "input_replay"},
    {DurablePointRole::LongAnchor, "long_anchor"}})
NLOHMANN_JSON_SERIALIZE_ENUM(DurablePointOrigin, {
    {DurablePointOrigin::Semantic, "semantic"},
    {DurablePointOrigin::Intermediate, "intermediate"}})
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DurablePayload, path, bytes, sha256, alignment)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DurableTokenSpan, begin, count)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DurableVisionIdentity, modality, temporal, height, width,
    patch_begin, patch_count, content_sha256, timestamps_bits, token_spans)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DurableIdentity, ledger, token_types, positions,
    execution_frontiers, vision_items, shortlist_tag)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DurableKvPoolLayout, pool_id, name, layout_sha256, storage_type,
    layers, tokens_per_page, kv_heads, head_dim, page_bytes, host_serialization_stride)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DurablePage, payload, offset, bytes, committed_tokens)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DurablePool, layout, pages)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DurableHistory, pool, pages, committed_tokens)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DurableState, layout_sha256, payload, offset, bytes)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DurableHistoryPrefix, history, frontier)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DurablePoint, role, origin, frontier, text_kv_frontier,
    backend_frontier, rope_delta, tail_hidden_valid, dflash_context_frontier, identity, state, histories)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DurableCheckpointSet, architecture, native_codec,
    state_layout_sha256, endpoint, points, identities, histories, states, pools, payloads)
} // namespace ninfer::runtime

namespace ninfer::models::qwen3_5::detail::checkpoint_codec {
namespace {
using namespace runtime;
void require(bool value, const char* message) {
    if (!value) throw std::invalid_argument(message);
}
bool hex(std::string_view text, std::size_t size) {
    return text.size() == size && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::uint64_t bits(std::string_view text) {
    require(hex(text, 16), "invalid timestamp bits");
    std::uint64_t result = 0;
    for (char c : text) result = (result << 4) | (c <= '9' ? c - '0' : c - 'a' + 10);
    return result;
}
bool decimal_file(std::string_view path, std::string_view prefix) {
    if (!path.starts_with(prefix) || !path.ends_with(".bin")) return false;
    auto id = path.substr(prefix.size(), path.size() - prefix.size() - 4);
    return !id.empty() && (id.size() == 1 || id.front() != '0') &&
           std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}
} // namespace

std::vector<std::uint8_t> encode(const DurableCheckpointSet& set) {
    nlohmann::json document = set;
    document["format"] = kDurableCheckpointSetFormat;
    return nlohmann::json::to_cbor(document);
}

DurableCheckpointSet decode(std::span<const std::byte> bytes, const DurableLimits& limits) {
    require(bytes.size() <= limits.manifest_bytes && bytes.size() <= limits.staging_bytes,
            "native index exceeds metadata limit");
    require(!bytes.empty(), "native index is empty");
    try {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.data());
        const auto document = nlohmann::json::from_cbor(begin, begin + bytes.size());
        require(document.at("format") == kDurableCheckpointSetFormat, "unsupported native index format");
        auto set = document.get<DurableCheckpointSet>();
        // Typed round-trip also rejects integer narrowing, floats, unknown keys, duplicate keys,
        // indefinite containers, tags, non-shortest integers and out-of-order map keys.
        const auto canonical = encode(set);
        require(canonical.size() == bytes.size() && std::equal(canonical.begin(), canonical.end(), begin),
                "native index is not canonical field-wise CBOR");
        validate(set, limits);
        return set;
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("invalid native checkpoint CBOR");
    }
}

void validate(const DurableCheckpointSet& set, const DurableLimits& limits) {
    require(!set.architecture.empty() && !set.native_codec.empty() && hex(set.state_layout_sha256, 64),
            "native index has no layout identity");
    require(!set.points.empty() && set.points.size() <= std::min(limits.max_points, kDurableMaxPoints) &&
            set.endpoint < set.points.size() &&
            set.points[set.endpoint].role == DurablePointRole::Continuation,
            "native index endpoint or point count is invalid");
    require(!set.identities.empty() && set.identities.size() <= set.points.size() &&
            !set.states.empty() && set.states.size() <= set.points.size() &&
            !set.histories.empty() && set.histories.size() <= 2 * set.points.size() &&
            !set.pools.empty() && set.pools.size() <= 2 &&
            !set.payloads.empty() && set.payloads.size() < std::min(limits.max_files, kDurableMaxFiles),
            "native index table count is invalid");
    require(encode(set).size() <= std::min(limits.manifest_bytes, limits.staging_bytes),
            "native index exceeds metadata limit");
    std::vector<bool> identities(set.identities.size()), states(set.states.size()),
        histories(set.histories.size()), pools(set.pools.size()), payloads(set.payloads.size());
    std::vector<std::vector<bool>> pages;
    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> ranges(set.payloads.size());
    for (std::size_t i = 0; i < set.payloads.size(); ++i) {
        const auto& file = set.payloads[i];
        require(file.bytes && hex(file.sha256, 64) && std::has_single_bit(file.alignment) &&
                file.alignment <= (1U << 20) &&
                (decimal_file(file.path, "engine/state/") || decimal_file(file.path, "engine/kv/")) &&
                (i == 0 || set.payloads[i - 1].path < file.path), "invalid native payload descriptor");
    }
    const auto range = [&](std::uint32_t payload, std::uint64_t offset, std::uint64_t size,
                           std::string_view prefix) {
        require(payload < set.payloads.size(), "native payload reference is out of range");
        const auto& file = set.payloads[payload];
        require(file.path.starts_with(prefix) && size && offset <= file.bytes &&
                size <= file.bytes - offset && offset % file.alignment == 0,
                "native payload range is invalid");
        payloads[payload] = true;
        ranges[payload].emplace_back(offset, offset + size);
    };
    for (const auto& state : set.states) {
        require(state.layout_sha256 == set.state_layout_sha256, "StateImage layout mismatch");
        range(state.payload, state.offset, state.bytes, "engine/state/");
    }
    for (std::size_t i = 0; i < set.pools.size(); ++i) {
        const auto& pool = set.pools[i];
        require(pool.layout.pool_id == i && hex(pool.layout.layout_sha256, 64) &&
                pool.layout.tokens_per_page && pool.layout.page_bytes &&
                pool.layout.host_serialization_stride >= pool.layout.page_bytes,
                "invalid native KV layout");
        pages.emplace_back(pool.pages.size());
        for (const auto& page : pool.pages) {
            require(page.committed_tokens && page.committed_tokens <= pool.layout.tokens_per_page &&
                    page.bytes == pool.layout.host_serialization_stride, "invalid native page coverage");
            range(page.payload, page.offset, page.bytes, "engine/kv/");
        }
    }
    for (const auto& history : set.histories) {
        require(history.pool < set.pools.size(), "native history pool is out of range");
        pools[history.pool] = true;
        const auto& pool = set.pools[history.pool];
        const auto count = history.committed_tokens == 0 ? 0 :
            1 + (history.committed_tokens - 1) / pool.layout.tokens_per_page;
        require(history.pages.size() == count, "native history page count disagrees with frontier");
        std::vector<std::uint32_t> unique = history.pages;
        std::sort(unique.begin(), unique.end());
        require(std::adjacent_find(unique.begin(), unique.end()) == unique.end(),
                "native history repeats a logical page");
        for (std::size_t i = 0; i < history.pages.size(); ++i) {
            const auto id = history.pages[i];
            require(id < pool.pages.size(), "native page index is out of range");
            const auto needed = std::min<std::uint64_t>(pool.layout.tokens_per_page,
                history.committed_tokens - i * static_cast<std::uint64_t>(pool.layout.tokens_per_page));
            require(pool.pages[id].committed_tokens >= needed, "native history exceeds page coverage");
            pages[history.pool][id] = true;
        }
    }
    for (const auto& identity : set.identities) {
        const auto count = identity.ledger.size();
        require(count && count <= UINT32_MAX && identity.token_types.size() == count,
                "native identity has inconsistent ledger");
        for (const auto& axis : identity.positions)
            require(axis.size() == count, "native identity has inconsistent positions");
        std::uint32_t previous = 0;
        for (auto frontier : identity.execution_frontiers) {
            require(frontier > previous && frontier <= count, "invalid rewrite execution frontier");
            previous = frontier;
        }
        std::uint64_t previous_end = 0;
        std::uint64_t patch_cursor = 0;
        for (const auto& vision : identity.vision_items) {
            require((vision.modality == "image" || vision.modality == "video") &&
                    vision.temporal > 0 && vision.height > 0 && vision.width > 0 &&
                    vision.patch_count && vision.patch_begin <= UINT64_MAX - vision.patch_count &&
                    hex(vision.content_sha256, 64) && !vision.token_spans.empty(),
                    "invalid native Vision identity");
            const auto temporal = static_cast<std::uint64_t>(vision.temporal);
            const auto height = static_cast<std::uint64_t>(vision.height);
            const auto width = static_cast<std::uint64_t>(vision.width);
            require(temporal <= UINT64_MAX / height && temporal * height <= UINT64_MAX / width &&
                    vision.patch_begin == patch_cursor && vision.patch_count == temporal * height * width,
                    "Vision patch ranges disagree with grid");
            patch_cursor += vision.patch_count;
            for (const auto& timestamp : vision.timestamps_bits)
                require(std::isfinite(std::bit_cast<double>(bits(timestamp))), "non-finite Vision timestamp");
            for (const auto& span : vision.token_spans) {
                require(span.count && span.begin >= previous_end && span.begin <= count &&
                        span.count <= count - span.begin, "Vision token span exceeds identity");
                previous_end = static_cast<std::uint64_t>(span.begin) + span.count;
            }
        }
    }
    std::size_t endpoints = 0;
    for (const auto& point : set.points) {
        require(point.identity < identities.size() && point.state < states.size() && point.frontier &&
                point.frontier == point.text_kv_frontier && point.tail_hidden_valid &&
                point.frontier <= set.identities[point.identity].ledger.size() &&
                point.backend_frontier <= point.frontier && point.dflash_context_frontier <= point.frontier,
                "native point has invalid identity or frontier");
        require(point.role == DurablePointRole::Continuation || point.role == DurablePointRole::InputReplay ||
                point.role == DurablePointRole::LongAnchor, "invalid native point role");
        require(point.origin == DurablePointOrigin::Semantic || point.origin == DurablePointOrigin::Intermediate,
                "invalid native point origin");
        endpoints += point.role == DurablePointRole::Continuation;
        identities[point.identity] = states[point.state] = true;
        require(!point.histories.empty() && point.histories.size() <= set.pools.size(),
                "native point has invalid pool count");
        std::uint32_t next_pool = 0;
        for (const auto& prefix : point.histories) {
            require(prefix.history < set.histories.size(), "native history index is out of range");
            const auto& history = set.histories[prefix.history];
            require(history.pool == next_pool++ && prefix.frontier <= history.committed_tokens &&
                    prefix.frontier == (history.pool == 0 ? point.text_kv_frontier : point.backend_frontier),
                    "native point pool prefix is invalid");
            histories[prefix.history] = true;
        }
    }
    require(endpoints == 1, "native set must contain exactly one endpoint");
    const auto all = [](const auto& used) { return std::all_of(used.begin(), used.end(), [](bool v) { return v; }); };
    require(all(identities) && all(states) && all(histories) && all(pools) && all(payloads),
            "native index contains unreferenced table rows");
    for (const auto& used : pages) require(all(used), "native index contains unreferenced pages");
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        auto& spans = ranges[i];
        std::sort(spans.begin(), spans.end());
        std::uint64_t end = 0;
        for (const auto& span : spans) {
            require(span.first == end, "native payload records overlap or leave a gap");
            end = span.second;
        }
        require(end == set.payloads[i].bytes, "native payload has unreferenced trailing bytes");
    }
}
} // namespace ninfer::models::qwen3_5::detail::checkpoint_codec
