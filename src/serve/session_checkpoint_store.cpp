#include "serve/session_checkpoint_store.h"

#include "core/io/durable_file.h"
#include "core/sha256.h"
#include "serve/server_identity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ninfer::serve {
namespace {

using Clock = std::chrono::system_clock;
using io::flush_file;
using io::open_binary_read;
using io::open_binary_write;
using io::replace_path;
using io::sync_directory;
using runtime::ContinuationCheckpointReader;
using runtime::DurableCheckpointStats;
using runtime::ContinuationCheckpointWriter;

inline constexpr std::size_t kMaximumTombstoneAttempts = 4096;
static_assert(std::is_nothrow_move_constructible_v<VerifiedSessionCheckpoint>);
static_assert(std::is_nothrow_move_assignable_v<VerifiedSessionCheckpoint>);

class CheckpointCorruption final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    CheckpointCorruption(runtime::DurableReason cause, const char* message)
        : std::runtime_error(message), reason(cause) {}
    const runtime::DurableReason reason = runtime::DurableReason::InvalidManifest;
};

class CheckpointUnavailable final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    CheckpointUnavailable(runtime::DurableReason cause, const char* message)
        : std::runtime_error(message), reason(cause) {}
    const runtime::DurableReason reason = runtime::DurableReason::IoUnavailable;
};

struct FileDescriptor {
    std::string path;
    std::uint64_t bytes = 0;
    std::string sha256;
    std::uint32_t alignment = 1;
};

[[nodiscard]] bool valid_generation_name(std::string_view value) noexcept {
    if (value.empty() || value.size() > 128) { return false; }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        const bool alphanumeric = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
        if (!alphanumeric && (index == 0 || (byte != '_' && byte != '-'))) { return false; }
    }
    return true;
}

[[nodiscard]] bool valid_payload_path(std::string_view value) noexcept {
    if (value == "responses.cbor" || value == "checksums.json" || value == "engine/index.cbor") {
        return true;
    }
    if (value.starts_with("engine/state/")) {
        value.remove_prefix(std::string_view("engine/state/").size());
    } else if (value.starts_with("engine/kv/")) {
        value.remove_prefix(std::string_view("engine/kv/").size());
    } else {
        return false;
    }
    if (!value.ends_with(".bin")) { return false; }
    value.remove_suffix(4);
    if (value.empty() || (value.size() > 1 && value.front() == '0')) { return false; }
    std::uint32_t id = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

[[nodiscard]] bool exact_keys(const nlohmann::json& value,
                              std::initializer_list<std::string_view> keys) {
    return value.is_object() && value.size() == keys.size() &&
           std::all_of(keys.begin(), keys.end(),
                       [&](std::string_view key) { return value.contains(key); });
}

[[nodiscard]] bool schema_key(std::string_view token) noexcept {
    std::size_t position = 0;
    for (const unsigned char expected : std::string_view("schema_version")) {
        if (position == token.size()) { return false; }
        unsigned value = static_cast<unsigned char>(token[position++]);
        if (value == '\\') {
            // Only \u escapes can encode the ASCII letters/underscore in this key.
            if (token.size() - position < 5 || token[position++] != 'u') { return false; }
            value = 0;
            for (unsigned digit = 0; digit != 4; ++digit) {
                const auto byte = static_cast<unsigned char>(token[position++]);
                unsigned hex;
                if (byte >= '0' && byte <= '9') { hex = byte - '0'; }
                else if (byte >= 'a' && byte <= 'f') { hex = byte - 'a' + 10; }
                else if (byte >= 'A' && byte <= 'F') { hex = byte - 'A' + 10; }
                else { return false; }
                value = value * 16 + hex;
            }
        }
        if (value != expected) { return false; }
    }
    return position == token.size();
}

// Refusal-only envelope probe over the already bounded bytes. Skip strings/nesting lexically,
// without a JSON DOM, interpreting no field except a top-level schema_version integer. Forgery
// can only refuse an import here; >=3 and unrecognized envelopes still require MAC verification.
[[nodiscard]] bool legacy_schema_envelope(std::string_view text) noexcept {
    if (text.starts_with("\xef\xbb\xbf")) { text.remove_prefix(3); }
    const auto whitespace = [](char byte) {
        return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
    };
    const auto skip_space = [&](std::size_t& position) {
        while (position != text.size() && whitespace(text[position])) { ++position; }
    };
    std::size_t position = 0;
    skip_space(position);
    if (position == text.size() || text[position++] != '{') { return false; }
    std::size_t depth = 1;
    while (position != text.size()) {
        const char byte = text[position++];
        if (byte == '"') {
            const std::size_t begin = position;
            while (position != text.size() && text[position] != '"') {
                if (text[position] == '\\') {
                    if (++position == text.size()) { return false; }
                }
                ++position;
            }
            if (position == text.size()) { return false; }
            const auto token = text.substr(begin, position - begin);
            ++position;
            if (depth != 1) { continue; }
            std::size_t scalar = position;
            skip_space(scalar);
            if (scalar == text.size() || text[scalar++] != ':' || !schema_key(token)) {
                continue;
            }
            skip_space(scalar);
            const bool negative = scalar != text.size() && text[scalar] == '-';
            if (negative) { ++scalar; }
            const std::size_t digits = scalar;
            while (scalar != text.size() && text[scalar] >= '0' && text[scalar] <= '9') {
                ++scalar;
            }
            const std::size_t count = scalar - digits;
            if (count == 0 || (count > 1 && text[digits] == '0')) { return false; }
            skip_space(scalar);
            if (scalar == text.size() || (text[scalar] != ',' && text[scalar] != '}')) {
                return false;
            }
            return negative || (count == 1 && text[digits] <= '2');
        }
        if (byte == '{' || byte == '[') { ++depth; }
        else if (byte == '}' || byte == ']') {
            if (--depth == 0) { return false; }
        }
    }
    return false;
}

[[nodiscard]] nlohmann::json parse_canonical_json(std::string_view text) {
    std::vector<std::set<std::string>> object_keys;
    const auto check = [&](int depth, nlohmann::json::parse_event_t event,
                            nlohmann::json& value) {
        // No v3 schema has recursively nested object fields.
        if (depth > 64) { throw CheckpointCorruption("checkpoint metadata nesting is invalid"); }
        if (event == nlohmann::json::parse_event_t::object_start) {
            object_keys.emplace_back();
        } else if (event == nlohmann::json::parse_event_t::key) {
            const auto& key = value.get_ref<const std::string&>();
            if (object_keys.empty() ||
                !std::all_of(key.begin(), key.end(),
                             [](unsigned char byte) { return byte < 128; }) ||
                !object_keys.back().insert(key).second) {
                throw CheckpointCorruption("checkpoint metadata has invalid or duplicate keys");
            }
        } else if (event == nlohmann::json::parse_event_t::object_end) {
            object_keys.pop_back();
        } else if (event == nlohmann::json::parse_event_t::value && value.is_number_float()) {
            throw CheckpointCorruption("checkpoint metadata contains a floating-point value");
        }
        return true;
    };
    nlohmann::json value = nlohmann::json::parse(text, check);
    if (value.dump() != text) {
        throw CheckpointCorruption("checkpoint metadata is not canonical JSON");
    }
    return value;
}

[[nodiscard]] bool unsigned_field(const nlohmann::json& value, std::string_view key,
                                  std::uint64_t maximum =
                                      std::numeric_limits<std::uint64_t>::max()) {
    return value.at(key).is_number_unsigned() && value.at(key).get<std::uint64_t>() <= maximum;
}

[[nodiscard]] bool valid_checkpoint_stats(const DurableCheckpointStats& stats) noexcept {
    return stats.frontier_tokens != 0 && stats.restored_tokens != 0 &&
           stats.restored_tokens <= stats.frontier_tokens && stats.payload_bytes != 0 &&
           static_cast<std::uint64_t>(stats.restored_tokens) * 100ULL >=
               static_cast<std::uint64_t>(stats.frontier_tokens) * 95ULL;
}

[[nodiscard]] std::uint64_t unix_milliseconds() {
    const auto value =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
            .count();
    return value < 0 ? 0 : static_cast<std::uint64_t>(value);
}

[[nodiscard]] bool valid_relative_path(std::string_view value) noexcept {
    if (value.empty() || value.front() == '/' || value.front() == '\\' ||
        value.find("..") != std::string_view::npos || value.find('\\') != std::string_view::npos ||
        value.find(':') != std::string_view::npos) {
        return false;
    }
    bool component_has_byte = false;
    for (const unsigned char byte : value) {
        if (byte == '/') {
            if (!component_has_byte) { return false; }
            component_has_byte = false;
            continue;
        }
        const bool allowed = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                             (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                             byte == '.';
        if (!allowed) { return false; }
        component_has_byte = true;
    }
    return component_has_byte;
}

void write_synced(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::FILE* file = open_binary_write(path);
    if (file == nullptr) { throw std::runtime_error("checkpoint file creation failed"); }
    try {
        io::write_exact(file, bytes);
        flush_file(file);
        if (std::fclose(file) != 0) {
            file = nullptr;
            throw std::runtime_error("checkpoint file close failed");
        }
        file = nullptr;
    } catch (...) {
        if (file != nullptr) { std::fclose(file); }
        throw;
    }
}

void write_synced(const std::filesystem::path& path, std::string_view bytes) {
    write_synced(path, std::as_bytes(std::span(bytes.data(), bytes.size())));
}

[[nodiscard]] bool remove_best_effort(const std::filesystem::path& path) noexcept {
    try {
        (void)io::remove_tree(path);
        return true;
    } catch (...) { return false; }
}

[[nodiscard]] std::vector<std::byte> read_bounded(const std::filesystem::path& path,
                                                  std::size_t limit) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) { throw CheckpointUnavailable("checkpoint file size is unavailable"); }
    if (size > limit || size > std::numeric_limits<std::size_t>::max()) {
        throw CheckpointCorruption("checkpoint file size exceeds its bound");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::FILE* file = nullptr;
    try {
        file = open_binary_read(path);
    } catch (const std::system_error&) {
        throw CheckpointUnavailable("checkpoint file open failed");
    }
    try {
        io::read_exact(file, bytes);
    } catch (...) {
        std::fclose(file);
        throw CheckpointUnavailable("checkpoint file read failed");
    }
    if (std::fclose(file) != 0) { throw CheckpointUnavailable("checkpoint file close failed"); }
    return bytes;
}

[[nodiscard]] std::string read_text_bounded(const std::filesystem::path& path, std::size_t limit) {
    const std::vector<std::byte> bytes = read_bounded(path, limit);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Origin authentication for checkpoint manifests (alphastorm/ninfer#32). The digest chain in
// manifest.json authenticates consistency only: a writer inside the checkpoint root can rewrite
// manifest and payloads coherently. The MAC key lives outside the root, so a valid manifest.mac
// proves the manifest was produced by this server's configured identity.
enum class ManifestOriginState : std::uint8_t { Authentic, Unauthenticated, Forged };

[[nodiscard]] std::string manifest_origin_mac_hex(std::string_view key,
                                                  std::string_view manifest_text) {
    return crypto::sha256_hex(crypto::hmac_sha256(
        std::as_bytes(std::span(key.data(), key.size())),
        std::as_bytes(std::span(manifest_text.data(), manifest_text.size()))));
}

[[nodiscard]] bool constant_time_hex_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) { return false; }
    unsigned char difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        difference = static_cast<unsigned char>(
            difference | (static_cast<unsigned char>(left[i]) ^
                          static_cast<unsigned char>(right[i])));
    }
    return difference == 0;
}

[[nodiscard]] ManifestOriginState manifest_origin_state(const std::filesystem::path& root,
                                                        std::string_view manifest_text,
                                                        std::string_view key) {
    std::error_code probe_error;
    const bool tag_exists = std::filesystem::exists(root / "manifest.mac", probe_error);
    if (probe_error) {
        // A transient inability to even probe the tag is a filesystem fault, never origin
        // evidence: preserve current for retry (council CR-20260831-originauth R2).
        throw CheckpointUnavailable("checkpoint origin tag probe failed");
    }
    if (!tag_exists) { return ManifestOriginState::Unauthenticated; }
    if (key.empty()) {
        throw CheckpointUnavailable(runtime::DurableReason::OriginKeyUnavailable,
                                    "checkpoint origin key is unavailable");
    }
    // read_text_bounded throws CheckpointUnavailable on open/size/read faults; let it
    // propagate so a transient tag read reports Unavailable instead of quarantining a
    // valid generation. Only structural badness or a mismatch is forgery evidence.
    std::string recorded;
    try {
        recorded = read_text_bounded(root / "manifest.mac", 128);
    } catch (const CheckpointCorruption&) { return ManifestOriginState::Forged; }
    if (recorded.size() != 64) { return ManifestOriginState::Forged; }
    const std::string expected = manifest_origin_mac_hex(key, manifest_text);
    return constant_time_hex_equal(recorded, expected) ? ManifestOriginState::Authentic
                                                       : ManifestOriginState::Forged;
}

// Payload bytes are hashed once, as the engine streams them (DirectoryCheckpointReader). Load
// only proves the payload exists as a regular file of the promised size, so file_size() answers
// and the manifest's byte summary are trustworthy before any read is issued.
void require_payload_shape(const std::filesystem::path& root, const std::string& relative,
                           std::uint64_t expected_bytes,
                           const std::shared_ptr<runtime::ContinuationCheckpointReadQueue>& read_queue) {
    const std::filesystem::path path = root / relative;
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::exists(status)) {
        throw CheckpointUnavailable("checkpoint payload is unavailable");
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw CheckpointCorruption("checkpoint payload is not a regular file");
    }
    const std::uintmax_t actual_bytes = std::filesystem::file_size(path, error);
    if (error) { throw CheckpointUnavailable("checkpoint payload size is unavailable"); }
    if (actual_bytes != expected_bytes) {
        throw CheckpointCorruption(runtime::DurableReason::PayloadSizeMismatch,
                                   "checkpoint payload size mismatch");
    }
    if (!read_queue || !read_queue->available()) {
        throw CheckpointUnavailable("checkpoint native read queue is unavailable");
    }
}

[[nodiscard]] std::uint64_t directory_bytes(const std::filesystem::path& path) {
    std::error_code error;
    std::uint64_t total = 0;
    for (std::filesystem::recursive_directory_iterator
             iterator(path, std::filesystem::directory_options::none, error),
         end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || error) { continue; }
        const std::uintmax_t size = iterator->file_size(error);
        if (error || size > std::numeric_limits<std::uint64_t>::max() - total) {
            throw std::overflow_error("checkpoint directory byte count overflowed");
        }
        total += static_cast<std::uint64_t>(size);
    }
    if (error) { throw std::runtime_error("checkpoint directory scan failed"); }
    return total;
}

nlohmann::json source_to_json(const product::media_acquire::Source& source) {
    return {{"kind", static_cast<std::uint8_t>(source.kind)},
            {"value", source.value},
            {"media_type", source.media_type},
            {"bytes", nlohmann::json::binary(source.bytes)}};
}

std::optional<product::media_acquire::Source> source_from_json(const nlohmann::json& value) {
    if (!value.is_object()) { return std::nullopt; }
    const int kind = value.at("kind").get<int>();
    if (kind < static_cast<int>(product::media_acquire::SourceKind::Path) ||
        kind > static_cast<int>(product::media_acquire::SourceKind::Bytes) ||
        !value.at("value").is_string() || !value.at("media_type").is_string() ||
        !value.at("bytes").is_binary()) {
        return std::nullopt;
    }
    product::media_acquire::Source source;
    source.kind       = static_cast<product::media_acquire::SourceKind>(kind);
    source.value      = value.at("value").get<std::string>();
    source.media_type = value.at("media_type").get<std::string>();
    const auto& bytes = value.at("bytes").get_binary();
    source.bytes.assign(bytes.begin(), bytes.end());
    return source;
}

nlohmann::json boundary_to_json(const std::optional<CacheBoundary>& boundary) {
    if (!boundary) { return nullptr; }
    return {{"kind", static_cast<std::uint8_t>(boundary->kind)},
            {"evidence", static_cast<std::uint8_t>(boundary->evidence)},
            {"ttl", static_cast<std::uint8_t>(boundary->ttl)}};
}

std::optional<CacheBoundary> boundary_from_json(const nlohmann::json& value) {
    if (value.is_null()) { return std::nullopt; }
    const auto kind = value.at("kind").get<unsigned>();
    const auto evidence = value.at("evidence").get<unsigned>();
    const auto ttl = value.at("ttl").get<unsigned>();
    if (kind > static_cast<unsigned>(PromptCacheMarkerKind::PrivateLongAnchor) ||
        evidence > (static_cast<unsigned>(SharedCandidateEvidence::ExplicitBoundary) |
                    static_cast<unsigned>(SharedCandidateEvidence::RequestedAutomatic)) ||
        ttl > static_cast<unsigned>(CacheBoundary::Ttl::OneHour)) {
        throw std::invalid_argument("checkpoint cache boundary is invalid");
    }
    return CacheBoundary{.kind = static_cast<PromptCacheMarkerKind>(kind),
                         .evidence = static_cast<SharedCandidateEvidence>(evidence),
                         .ttl = static_cast<CacheBoundary::Ttl>(ttl)};
}

std::string_view checkpoint_tool_kind_name(ToolKind kind) {
    switch (kind) {
    case ToolKind::Function: return "function";
    case ToolKind::Custom: return "custom";
    }
    throw std::invalid_argument("checkpoint tool kind is invalid");
}

ToolKind checkpoint_tool_kind(const nlohmann::json& value) {
    if (value == "function") { return ToolKind::Function; }
    if (value == "custom") { return ToolKind::Custom; }
    throw std::invalid_argument("checkpoint tool kind is invalid");
}

nlohmann::json turn_to_json(const ChatTurn& turn) {
    nlohmann::json content = nlohmann::json::array();
    for (const ContentPart& part : turn.content) {
        content.push_back({{"kind", static_cast<std::uint8_t>(part.kind)},
                           {"text", part.text},
                           {"type_raw", part.type_raw},
                           {"source", source_to_json(part.source)},
                           {"image_resize_policy",
                            static_cast<std::uint8_t>(part.image_resize_policy)},
                           {"cache_boundary_after", boundary_to_json(part.cache_boundary_after)}});
    }
    nlohmann::json calls = nlohmann::json::array();
    for (const ToolCall& call : turn.tool_calls) {
        calls.push_back(
            {{"id", call.id}, {"name", call.name}, {"arguments_json", call.arguments_json},
             {"kind", checkpoint_tool_kind_name(call.kind)}});
    }
    return {{"role", static_cast<std::uint8_t>(turn.role)},
            {"content", std::move(content)},
            {"tool_calls", std::move(calls)},
            {"tool_call_id", turn.tool_call_id},
            {"tool_kind", checkpoint_tool_kind_name(turn.tool_kind)},
            {"tool_result_name", turn.tool_result_name ? nlohmann::json(*turn.tool_result_name)
                                                       : nlohmann::json(nullptr)},
            {"tool_result_is_error", turn.tool_result_is_error},
            {"reasoning_content", turn.reasoning_content},
            {"cache_boundary_after", boundary_to_json(turn.cache_boundary_after)},
            {"private_cache_boundary_after", turn.private_cache_boundary_after}};
}

std::optional<ChatTurn> turn_from_json(const nlohmann::json& value) {
    if (!value.is_object() || !value.at("content").is_array() ||
        !value.at("tool_calls").is_array()) {
        return std::nullopt;
    }
    const int role = value.at("role").get<int>();
    if (role < static_cast<int>(ChatRole::System) || role > static_cast<int>(ChatRole::Tool)) {
        return std::nullopt;
    }
    ChatTurn turn;
    turn.role              = static_cast<ChatRole>(role);
    turn.tool_call_id      = value.at("tool_call_id").get<std::string>();
    turn.tool_kind         = checkpoint_tool_kind(value.at("tool_kind"));
    if (!value.at("tool_result_name").is_null()) {
        turn.tool_result_name = value.at("tool_result_name").get<std::string>();
    }
    turn.tool_result_is_error = value.at("tool_result_is_error").get<bool>();
    turn.reasoning_content = value.at("reasoning_content").get<std::string>();
    turn.cache_boundary_after = boundary_from_json(value.at("cache_boundary_after"));
    turn.private_cache_boundary_after = value.at("private_cache_boundary_after").get<bool>();
    for (const nlohmann::json& encoded : value.at("content")) {
        const int kind = encoded.at("kind").get<int>();
        if (kind < static_cast<int>(ContentKind::Text) ||
            kind > static_cast<int>(ContentKind::Video)) {
            return std::nullopt;
        }
        const auto resize = encoded.at("image_resize_policy").get<unsigned>();
        if (resize > static_cast<unsigned>(ImageResizePolicy::RejectOversized)) {
            return std::nullopt;
        }
        std::optional<product::media_acquire::Source> source =
            source_from_json(encoded.at("source"));
        if (!source) { return std::nullopt; }
        turn.content.push_back(ContentPart{.kind     = static_cast<ContentKind>(kind),
                                           .text     = encoded.at("text").get<std::string>(),
                                           .type_raw = encoded.at("type_raw").get<std::string>(),
                                           .source   = std::move(*source),
                                           .image_resize_policy =
                                               static_cast<ImageResizePolicy>(resize),
                                           .cache_boundary_after = boundary_from_json(
                                               encoded.at("cache_boundary_after"))});
    }
    for (const nlohmann::json& encoded : value.at("tool_calls")) {
        turn.tool_calls.push_back(
            ToolCall{.id             = encoded.at("id").get<std::string>(),
                     .name           = encoded.at("name").get<std::string>(),
                     .arguments_json = encoded.at("arguments_json").get<std::string>(),
                     .kind           = checkpoint_tool_kind(encoded.at("kind"))});
    }
    return turn;
}

class DirectoryCheckpointWriter final : public ContinuationCheckpointWriter {
public:
    DirectoryCheckpointWriter(std::filesystem::path root, std::size_t staging_limit)
        : root_(std::move(root)), staging_limit_(staging_limit) {}

    ~DirectoryCheckpointWriter() override {
        for (auto& [path, state] : states_) {
            (void)path;
            if (state.file != nullptr) { std::fclose(state.file); }
        }
    }

    bool write_file(std::string_view path, std::uint64_t offset, std::uint64_t total_bytes,
                    std::span<const std::byte> bytes, std::uint32_t alignment) override {
        if (!path.starts_with("engine/") || path.size() == std::string_view("engine/").size()) {
            failed_ = true;
            return false;
        }
        return write_impl(path, offset, total_bytes, bytes, alignment);
    }

    bool write_store_file(std::string_view path, std::uint64_t offset, std::uint64_t total_bytes,
                          std::span<const std::byte> bytes) {
        return write_impl(path, offset, total_bytes, bytes, 1);
    }

    [[nodiscard]] bool finish() override {
        if (failed_ || states_.empty() ||
            !std::all_of(states_.begin(), states_.end(), [](const auto& entry) {
                const auto& state = entry.second;
                return state.complete && state.file == nullptr && state.written == state.total;
            })) {
            return false;
        }
#ifndef _WIN32
        // File fsync does not publish nested directory entries. Children precede their
        // parents; the store syncs the generation root after writing its manifest/MAC.
        for (auto path = dirty_directories_.rbegin(); path != dirty_directories_.rend(); ++path) {
            sync_directory(*path);
        }
        dirty_directories_.clear();
#endif
        return true;
    }

    [[nodiscard]] std::optional<std::vector<FileDescriptor>> files() const {
        if (failed_) { return std::nullopt; }
        std::vector<FileDescriptor> out;
        out.reserve(states_.size());
        for (const auto& [path, state] : states_) {
            if (!state.complete || state.file != nullptr || state.written != state.total) {
                return std::nullopt;
            }
            out.push_back({.path = path, .bytes = state.total, .sha256 = state.digest,
                           .alignment = state.alignment});
        }
        return out;
    }

private:
    bool write_impl(std::string_view path, std::uint64_t offset, std::uint64_t total_bytes,
                    std::span<const std::byte> bytes, std::uint32_t alignment) {
        if (failed_ || !valid_relative_path(path) || bytes.size() > staging_limit_ ||
            !std::has_single_bit(alignment) ||
            ((path == "engine/index.cbor" || path == "responses.cbor") &&
             (total_bytes > staging_limit_ || alignment != 1)) ||
            offset > total_bytes || bytes.size() > total_bytes - offset) {
            failed_ = true;
            return false;
        }
        try {
            auto position = states_.find(path);
            const bool inserted = position == states_.end();
            if (inserted) { position = states_.try_emplace(std::string(path)).first; }
            const std::string& key = position->first;
            State& state              = position->second;
            if (inserted) {
                if (offset != 0 || states_.size() > 4096) {
                    failed_ = true;
                    return false;
                }
                state.total                             = total_bytes;
                state.alignment = alignment;
                const std::filesystem::path destination = root_ / key;
                io::create_directories(destination.parent_path());
#ifndef _WIN32
                for (auto parent = destination.parent_path(); parent != root_;
                     parent = parent.parent_path()) {
                    dirty_directories_.insert(parent);
                }
#endif
                state.file = open_binary_write(destination);
                if (state.file == nullptr) {
                    failed_ = true;
                    return false;
                }
            }
            if (state.complete || state.total != total_bytes || state.written != offset ||
                state.alignment != alignment ||
                state.file == nullptr) {
                failed_ = true;
                return false;
            }
            io::write_exact(state.file, bytes);
            state.hasher.update(bytes);
            state.written += static_cast<std::uint64_t>(bytes.size());
            if (state.written == state.total) {
                flush_file(state.file);
                if (std::fclose(state.file) != 0) {
                    state.file = nullptr;
                    failed_    = true;
                    return false;
                }
                state.file     = nullptr;
                state.complete = true;
                state.digest   = crypto::sha256_hex(state.hasher.finish());
            }
            return true;
        } catch (...) {
            failed_ = true;
            return false;
        }
    }

private:
    struct State {
        std::FILE* file = nullptr;
        crypto::Sha256 hasher;
        std::uint64_t total   = 0;
        std::uint64_t written = 0;
        std::string digest;
        bool complete = false;
        std::uint32_t alignment = 1;
    };

    std::filesystem::path root_;
    std::size_t staging_limit_ = 0;
    std::map<std::string, State, std::less<>> states_;
#ifndef _WIN32
    std::set<std::filesystem::path> dirty_directories_;
#endif
    bool failed_ = false;
};

// Owns each accepted chunk while a drain thread hashes and writes it (ninfer#34). K3 captures
// immutable native views under Engine serialization, then streams and backpressures off-lock.
// finish() joins the drain off-lock and reports every deferred failure before publication.
class BufferedCheckpointWriter final : public ContinuationCheckpointWriter {
public:
    BufferedCheckpointWriter(DirectoryCheckpointWriter& inner, std::size_t capacity_bytes)
        : inner_(inner), capacity_(capacity_bytes == 0 ? 1 : capacity_bytes) {
        drain_ = std::thread([this] { drain(); });
    }

    ~BufferedCheckpointWriter() override { (void)finish(); }

    bool write_file(std::string_view path, std::uint64_t offset, std::uint64_t total_bytes,
                    std::span<const std::byte> bytes, std::uint32_t alignment) override {
        try {
            const std::size_t size = bytes.size();
            {
                std::unique_lock lock(mutex_);
                if (failed_ || closed_) { return false; }
                // Reserve before copying so accounted bytes cover every copy this writer
                // holds, including the chunk the drain thread is currently writing
                // (review CR-20260831-ckptdecouple R2). A chunk larger than the whole
                // capacity is admitted only once the writer holds nothing at all.
                space_.wait(lock, [&] {
                    return failed_ || queued_bytes_ == 0 || queued_bytes_ + size <= capacity_;
                });
                if (failed_) { return false; }
                queued_bytes_ += size;
            }
            // Copy outside the lock so the drain thread keeps writing during the memcpy.
            Chunk chunk;
            chunk.path.assign(path);
            chunk.offset = offset;
            chunk.total  = total_bytes;
            chunk.alignment = alignment;
            chunk.bytes.assign(bytes.begin(), bytes.end());
            {
                std::lock_guard lock(mutex_);
                if (failed_) {
                    queued_bytes_ -= size;
                    return false;
                }
                queue_.push_back(std::move(chunk));
            }
            ready_.notify_one();
            return true;
        } catch (...) {
            std::lock_guard lock(mutex_);
            failed_ = true;
            return false;
        }
    }

    // Idempotent: closes the queue, joins the drain thread, and reports whether every
    // deferred write reached the inner writer successfully.
    [[nodiscard]] bool finish() noexcept override {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
        space_.notify_all();
        if (drain_.joinable()) {
            try {
                drain_.join();
            } catch (...) { return false; }
        }
        std::lock_guard lock(mutex_);
        try {
            return !failed_ && inner_.finish();
        } catch (...) {
            failed_ = true;
            return false;
        }
    }

private:
    struct Chunk {
        std::string path;
        std::uint64_t offset = 0;
        std::uint64_t total  = 0;
        std::vector<std::byte> bytes;
        std::uint32_t alignment = 1;
    };

    void drain() {
        for (;;) {
            Chunk chunk;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return failed_ || closed_ || !queue_.empty(); });
                if (queue_.empty()) { return; }
                chunk = std::move(queue_.front());
                queue_.pop_front();
                // queued_bytes_ keeps covering this in-flight chunk until its disk write
                // completes, so the configured bound is a real RAM bound (review R2).
            }
            const bool wrote =
                inner_.write_file(chunk.path, chunk.offset, chunk.total, chunk.bytes,
                                   chunk.alignment);
            {
                std::lock_guard lock(mutex_);
                queued_bytes_ -= chunk.bytes.size();
                if (!wrote) {
                    failed_ = true;
                    queue_.clear();
                    queued_bytes_ = 0;
                    space_.notify_all();
                    return;
                }
            }
            space_.notify_all();
        }
    }

    DirectoryCheckpointWriter& inner_;
    std::size_t capacity_ = 0;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable space_;
    std::deque<Chunk> queue_;
    std::size_t queued_bytes_ = 0;
    bool failed_              = false;
    bool closed_              = false;
    std::thread drain_;
};

[[nodiscard]] nlohmann::json descriptor_json(const FileDescriptor& file) {
    return {{"alignment", file.alignment}, {"path", file.path},
            {"bytes", file.bytes}, {"sha256", file.sha256}};
}

[[nodiscard]] std::optional<FileDescriptor> descriptor_from_json(const nlohmann::json& value) {
    if (!exact_keys(value, {"alignment", "bytes", "path", "sha256"}) ||
        !value.at("path").is_string() || !value.at("sha256").is_string() ||
        !unsigned_field(value, "bytes") ||
        !unsigned_field(value, "alignment", std::numeric_limits<std::uint32_t>::max())) {
        return std::nullopt;
    }
    FileDescriptor descriptor{.path   = value.at("path").get<std::string>(),
                              .bytes  = value.at("bytes").get<std::uint64_t>(),
                              .sha256 = value.at("sha256").get<std::string>(),
                              .alignment = value.at("alignment").get<std::uint32_t>()};
    if (!valid_payload_path(descriptor.path) || !std::has_single_bit(descriptor.alignment) ||
        ((!descriptor.path.starts_with("engine/state/") &&
          !descriptor.path.starts_with("engine/kv/")) && descriptor.alignment != 1) ||
        descriptor.sha256.size() != 64 ||
        !std::all_of(descriptor.sha256.begin(), descriptor.sha256.end(), [](unsigned char byte) {
            return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
        })) {
        return std::nullopt;
    }
    return descriptor;
}

void require_manifest_shape(const nlohmann::json& manifest) {
    if (!exact_keys(manifest, {"artifact_type", "schema_version", "session_sha256", "generation",
                               "created_at_unix_ms", "runtime_fingerprint", "latest_response_id",
                               "response_records", "frontier_tokens", "restored_tokens",
                               "engine_payload_bytes", "payload_bytes", "files"}) ||
        manifest.at("artifact_type") != "ninfer_session_checkpoint" ||
        !unsigned_field(manifest, "schema_version") ||
        manifest.at("schema_version") != kSessionCheckpointSchemaVersion ||
        !manifest.at("session_sha256").is_string() ||
        !manifest.at("generation").is_string() ||
        !valid_generation_name(manifest.at("generation").get_ref<const std::string&>()) ||
        !unsigned_field(manifest, "created_at_unix_ms") ||
        !manifest.at("latest_response_id").is_string() ||
        manifest.at("latest_response_id").get_ref<const std::string&>().empty() ||
        !unsigned_field(manifest, "response_records") || manifest.at("response_records") == 0 ||
        !unsigned_field(manifest, "frontier_tokens", std::numeric_limits<std::uint32_t>::max()) ||
        !unsigned_field(manifest, "restored_tokens", std::numeric_limits<std::uint32_t>::max()) ||
        !unsigned_field(manifest, "engine_payload_bytes") ||
        !unsigned_field(manifest, "payload_bytes") ||
        !manifest.at("files").is_array() || manifest.at("files").empty() ||
        manifest.at("files").size() > runtime::kDurableMaxFiles) {
        throw CheckpointCorruption("checkpoint manifest schema is invalid");
    }
    std::string previous;
    bool responses = false;
    bool index = false;
    bool state = false;
    bool checksums = false;
    std::uint64_t payload_bytes = 0;
    std::uint64_t engine_bytes = 0;
    for (const auto& value : manifest.at("files")) {
        const auto file = descriptor_from_json(value);
        if (!file || (!previous.empty() && file->path <= previous)) {
            throw CheckpointCorruption("checkpoint manifest payload inventory is invalid");
        }
        previous = file->path;
        if (file->bytes > std::numeric_limits<std::uint64_t>::max() - payload_bytes ||
            (file->path.starts_with("engine/") &&
             file->bytes > std::numeric_limits<std::uint64_t>::max() - engine_bytes)) {
            throw CheckpointCorruption("checkpoint manifest payload byte count overflowed");
        }
        payload_bytes += file->bytes;
        if (file->path.starts_with("engine/")) { engine_bytes += file->bytes; }
        responses = responses || file->path == "responses.cbor";
        index = index || file->path == "engine/index.cbor";
        state = state || file->path.starts_with("engine/state/");
        checksums = checksums || file->path == "checksums.json";
    }
    const DurableCheckpointStats stats{
        .frontier_tokens = manifest.at("frontier_tokens").get<std::uint32_t>(),
        .restored_tokens = manifest.at("restored_tokens").get<std::uint32_t>(),
        .payload_bytes = engine_bytes};
    if (!responses || !index || !state || !checksums ||
        manifest.at("payload_bytes").get<std::uint64_t>() != payload_bytes ||
        manifest.at("engine_payload_bytes").get<std::uint64_t>() != engine_bytes ||
        !valid_checkpoint_stats(stats)) {
        throw CheckpointCorruption("checkpoint manifest required payload is missing");
    }
}

[[nodiscard]] std::optional<std::string> current_generation(const std::filesystem::path& session) {
    std::error_code error;
    const std::filesystem::path path          = session / "current";
    const std::filesystem::file_status status = std::filesystem::status(path, error);
    if (error == std::errc::no_such_file_or_directory) { return std::nullopt; }
    if (error) { throw CheckpointUnavailable("checkpoint current pointer is unavailable"); }
    if (!std::filesystem::exists(status)) { return std::nullopt; }
    if (!std::filesystem::is_regular_file(status)) {
        throw CheckpointCorruption("checkpoint current pointer is not a regular file");
    }
    std::string value = read_text_bounded(path, 129);
    if (value.empty() || value.back() != '\n') {
        throw CheckpointCorruption("checkpoint current pointer is invalid");
    }
    value.pop_back();
    if (!valid_generation_name(value)) {
        throw CheckpointCorruption("checkpoint current pointer is invalid");
    }
    return value;
}

} // namespace

class SessionCheckpointStore::Impl : public std::enable_shared_from_this<Impl> {
public:
    struct CorruptGeneration {
        runtime::DurableReason reason;
        bool quarantine_requested = false;
    };
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::uint32_t> active_generations;
    // Generations whose streamed import re-hash observed bytes diverging from the
    // load-time digests. load() and status() consult this and quarantine instead of
    // advertising the generation as available (council CR-20260831-fanout43 R3).
    std::map<std::string, CorruptGeneration> corrupt_generations;
    std::atomic<std::uint64_t> sequence{0};
    bool save_in_progress = false;

    void mark_corrupt(const std::string& active_key) {
        std::lock_guard lock(mutex);
        try {
            corrupt_generations.try_emplace(
                active_key, CorruptGeneration{runtime::DurableReason::PayloadDigestMismatch});
        } catch (...) {}
    }


    [[nodiscard]] bool is_corrupt(const std::string& active_key) const {
        return corrupt_generations.contains(active_key);
    }
};

namespace {

[[nodiscard]] bool quarantine_generation(const std::filesystem::path& session,
                                         const std::string& generation, bool pinned) noexcept;

// Preserve the store's single-flight save/quota semantics without making an Engine worker wait
// for a save which may itself need that worker. Catalog ownership is never held across callbacks.
class CheckpointSaveLease {
public:
    CheckpointSaveLease(SessionCheckpointStore::Impl& owner,
                        const std::filesystem::path& session, std::string_view digest,
                        const std::string& generation)
        : owner_(owner), session_(session), generation_(generation) {
        std::lock_guard lock(owner_.mutex);
        if (owner_.save_in_progress) { return; }
        std::string staging_key;
        staging_key.reserve(digest.size() + 10 + generation.size());
        staging_key.append(digest).append("/.staging-").append(generation);
        std::string published_key;
        published_key.reserve(digest.size() + 1 + generation.size());
        published_key.append(digest).append("/").append(generation);
        const auto staging = owner_.active_generations.emplace(std::move(staging_key), 1).first;
        const std::string* staging_reference = &staging->first;
        try {
            const auto published =
                owner_.active_generations.emplace(std::move(published_key), 1).first;
            // References to unordered_map elements survive rehashes by concurrent reader loads.
            staging_key_ = staging_reference;
            published_key_ = &published->first;
        } catch (...) {
            owner_.active_generations.erase(*staging_reference);
            throw;
        }
        owner_.save_in_progress = true;
    }

    ~CheckpointSaveLease() {
        if (staging_key_ == nullptr) { return; }
        std::lock_guard lock(owner_.mutex);
        for (const std::string* key : {staging_key_, published_key_}) {
            const auto found = owner_.active_generations.find(*key);
            if (found == owner_.active_generations.end() || found->second == 0) {
                std::terminate();
            }
            if (--found->second == 0) {
                const auto corrupt = owner_.corrupt_generations.find(*key);
                owner_.active_generations.erase(found);
                if (corrupt != owner_.corrupt_generations.end() &&
                    corrupt->second.quarantine_requested &&
                    quarantine_generation(session_, generation_, false)) {
                    owner_.corrupt_generations.erase(corrupt);
                }
            }
        }
        owner_.save_in_progress = false;
    }

    CheckpointSaveLease(const CheckpointSaveLease&) = delete;
    CheckpointSaveLease& operator=(const CheckpointSaveLease&) = delete;
    [[nodiscard]] bool admitted() const noexcept { return staging_key_ != nullptr; }

private:
    SessionCheckpointStore::Impl& owner_;
    const std::filesystem::path& session_;
    const std::string& generation_;
    const std::string* staging_key_ = nullptr;
    const std::string* published_key_ = nullptr;
};

enum class CandidateKind : std::uint8_t {
    Tombstone,
    AbandonedStaging,
    StaleGeneration,
    CurrentSession,
};

struct GenerationCandidate {
    CandidateKind kind = CandidateKind::StaleGeneration;
    std::filesystem::path path;
    std::string digest;
    std::string name;
    std::filesystem::file_time_type time;
    std::uint64_t bytes = 0;
    // A current session checkpointing fewer than options.short_session_tokens tokens.
    bool short_session = false;
};

struct GenerationInventory {
    std::uint64_t used = 0;
    std::vector<GenerationCandidate> candidates;
};

void account_bytes(std::uint64_t& used, std::uint64_t bytes) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - used) {
        throw std::overflow_error("checkpoint quota accounting overflowed");
    }
    used += bytes;
}

[[nodiscard]] std::uint64_t session_generation_bytes(const std::filesystem::path& session) {
    const std::filesystem::path generations = session / "generations";
    std::error_code error;
    if (!std::filesystem::is_directory(generations, error)) {
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error("checkpoint generation scan failed");
        }
        return 0;
    }
    return directory_bytes(generations);
}

[[nodiscard]] std::uint64_t tombstone_generation_bytes(const std::filesystem::path& path) {
    if (path.filename().string().find("--session--") != std::string::npos) {
        return session_generation_bytes(path);
    }
    return directory_bytes(path);
}

// Whether a published generation checkpoints fewer tokens than options.short_session_tokens. Only
// the reclamation order reads it, never a load or a skip decision, so a manifest that cannot be
// read orders its session as long - kept longer - instead of failing the pass.
[[nodiscard]] bool short_session_generation(const SessionCheckpointStoreOptions& options,
                                            const std::filesystem::path& generation) noexcept {
    if (options.short_session_tokens == 0) { return false; }
    try {
        const std::string text = read_text_bounded(generation / "manifest.json", 16ULL << 20);
        const auto origin = manifest_origin_state(generation, text, options.origin_mac_key);
        if (origin == ManifestOriginState::Forged ||
            (origin == ManifestOriginState::Unauthenticated && options.require_origin_auth)) {
            return false;
        }
        const nlohmann::json manifest = parse_canonical_json(text);
        require_manifest_shape(manifest);
        return manifest.at("frontier_tokens").get<std::uint64_t>() < options.short_session_tokens;
    } catch (...) { return false; }
}

[[nodiscard]] GenerationInventory inventory_generations(
    const SessionCheckpointStoreOptions& options, const SessionCheckpointStore::Impl& impl,
    const std::optional<std::filesystem::path>& protected_generation = std::nullopt,
    const std::optional<std::filesystem::path>& protected_session    = std::nullopt) {
    GenerationInventory inventory;
    const std::filesystem::path tombstones = options.root / ".tombstones";
    for (const auto& entry : std::filesystem::directory_iterator(tombstones)) {
        const std::uint64_t bytes = entry.is_directory()
                                        ? tombstone_generation_bytes(entry.path())
                                        : static_cast<std::uint64_t>(entry.file_size());
        account_bytes(inventory.used, bytes);
        inventory.candidates.push_back({.kind  = CandidateKind::Tombstone,
                                        .path  = entry.path(),
                                        .time  = entry.last_write_time(),
                                        .bytes = bytes});
    }

    const std::filesystem::path sessions_root = options.root / "sessions";
    for (const auto& session_entry : std::filesystem::directory_iterator(sessions_root)) {
        if (!session_entry.is_directory()) { continue; }
        const std::string digest        = session_entry.path().filename().string();
        const std::string active_prefix = digest + "/";
        const bool session_active =
            std::any_of(impl.active_generations.begin(), impl.active_generations.end(),
                        [&](const auto& entry) { return entry.first.starts_with(active_prefix); });
        bool current_readable = true;
        std::optional<std::string> current;
        try {
            current = current_generation(session_entry.path());
        } catch (...) { current_readable = false; }
        const std::filesystem::path generations = session_entry.path() / "generations";
        std::error_code error;
        if (!std::filesystem::is_directory(generations, error)) {
            if (error && error != std::errc::no_such_file_or_directory) {
                throw std::runtime_error("checkpoint generation scan failed");
            }
            continue;
        }
        std::uint64_t session_bytes = 0;
        std::optional<std::filesystem::file_time_type> current_time;
        for (const auto& entry : std::filesystem::directory_iterator(generations)) {
            if (!entry.is_directory()) { continue; }
            const std::string name    = entry.path().filename().string();
            const std::uint64_t bytes = directory_bytes(entry.path());
            account_bytes(inventory.used, bytes);
            account_bytes(session_bytes, bytes);
            const bool is_current = current && name == *current;
            if (is_current) { current_time = entry.last_write_time(); }
            if (protected_generation && entry.path() == *protected_generation) { continue; }
            const std::string active_key = digest + "/" + name;
            if (impl.active_generations.contains(active_key)) { continue; }
            if (name.starts_with(".staging-")) {
                inventory.candidates.push_back({.kind   = CandidateKind::AbandonedStaging,
                                                .path   = entry.path(),
                                                .digest = digest,
                                                .name   = name,
                                                .time   = entry.last_write_time(),
                                                .bytes  = bytes});
            } else if (current_readable && !is_current) {
                inventory.candidates.push_back({.kind   = CandidateKind::StaleGeneration,
                                                .path   = entry.path(),
                                                .digest = digest,
                                                .name   = name,
                                                .time   = entry.last_write_time(),
                                                .bytes  = bytes});
            }
        }
        if (current_readable && current && current_time && !session_active &&
            (!protected_session || session_entry.path() != *protected_session)) {
            inventory.candidates.push_back(
                {.kind          = CandidateKind::CurrentSession,
                 .path          = session_entry.path(),
                 .digest        = digest,
                 .name          = *current,
                 .time          = *current_time,
                 .bytes         = session_bytes,
                 .short_session = short_session_generation(options, generations / *current)});
        }
    }
    std::sort(inventory.candidates.begin(), inventory.candidates.end(),
              [](const GenerationCandidate& left, const GenerationCandidate& right) {
                  if (left.kind != right.kind) { return left.kind < right.kind; }
                  if (left.short_session != right.short_session) { return left.short_session; }
                  if (left.time != right.time) { return left.time < right.time; }
                  return left.path.generic_string() < right.path.generic_string();
              });
    return inventory;
}

[[nodiscard]] std::filesystem::path unique_tombstone(const SessionCheckpointStoreOptions& options,
                                                     std::string stem) {
    const std::filesystem::path root = options.root / ".tombstones";
    const std::string stamp          = std::to_string(unix_milliseconds());
    for (std::size_t attempt = 0; attempt < kMaximumTombstoneAttempts; ++attempt) {
        const std::filesystem::path candidate =
            root / (stem + "--" + stamp + "-" + std::to_string(attempt));
        std::error_code error;
        const bool exists = std::filesystem::exists(candidate, error);
        if (error) { throw std::runtime_error("checkpoint tombstone lookup failed"); }
        if (!exists) { return candidate; }
    }
    throw std::runtime_error("checkpoint tombstone namespace is exhausted");
}

[[nodiscard]] std::filesystem::path
candidate_tombstone(const SessionCheckpointStoreOptions& options,
                    const GenerationCandidate& candidate) {
    const char* kind =
        candidate.kind == CandidateKind::CurrentSession ? "--session--" : "--generation--";
    return unique_tombstone(options, candidate.digest + kind + candidate.name);
}

[[nodiscard]] bool cleanup_tombstone(const SessionCheckpointStoreOptions& options,
                                     const std::filesystem::path& path) {
    if (options.tombstone_cleanup) {
        // A cleanup hook outage is an unhealthy reclamation, never a failed save.
        try {
            if (!options.tombstone_cleanup(path)) { return false; }
        } catch (...) { return false; }
    } else {
        if (!remove_best_effort(path)) { return false; }
    }
    std::error_code error;
    return !std::filesystem::exists(path, error) && !error;
}

[[nodiscard]] bool sync_rename_parents(const std::filesystem::path& source,
                                       const std::filesystem::path& destination) noexcept {
    try {
        sync_directory(source.parent_path());
        sync_directory(destination.parent_path());
        return true;
    } catch (...) { return false; }
}

[[nodiscard]] bool enforce_disk_quota_locked(
    const SessionCheckpointStoreOptions& options, const SessionCheckpointStore::Impl& impl,
    const std::optional<std::filesystem::path>& protected_generation = std::nullopt,
    const std::optional<std::filesystem::path>& protected_session    = std::nullopt,
    std::uint64_t tolerated_transient_bytes                          = 0,
    std::uint64_t unpublished_bytes                                  = 0) {
    GenerationInventory inventory =
        inventory_generations(options, impl, protected_generation, protected_session);
    std::uint64_t physical_used = inventory.used;
    // The transient tolerance below only applies while reclamation is fully healthy: if any
    // eviction this pass fails, the store falls back to the hard cap so a broken cleanup path
    // can never let disk usage creep past the quota one tolerated save at a time. A failed pass
    // also stops evicting other sessions' current checkpoints: through a broken cleanup path each
    // one would lose its only copy without freeing a byte, so the save is refused instead and
    // usage stays over the cap by the stuck bytes alone.
    bool reclamation_healthy = true;
    for (const GenerationCandidate& candidate : inventory.candidates) {
        const bool unconditional = candidate.kind == CandidateKind::Tombstone ||
                                   candidate.kind == CandidateKind::AbandonedStaging;
        // The tolerated transient is the saving session's superseded generation, which the
        // post-publish pass reclaims first. Evicting another session to make room for it would
        // delete that session's only checkpoint and leave the superseded copy on disk.
        const std::uint64_t allowed =
            options.disk_quota_bytes + (reclamation_healthy ? tolerated_transient_bytes : 0);
        if (inventory.used <= allowed && !unconditional) { continue; }
        if (!reclamation_healthy && candidate.kind == CandidateKind::CurrentSession) { continue; }
        if (unpublished_bytes && candidate.kind == CandidateKind::CurrentSession) {
            // Plan only. No current authority may disappear before the requester's
            // durable publication. Re-plan under this lock in the final callback.
            inventory.used -= std::min(inventory.used, session_generation_bytes(candidate.path));
            continue;
        }
        if (candidate.kind == CandidateKind::Tombstone) {
            if (cleanup_tombstone(options, candidate.path)) {
                inventory.used -= candidate.bytes;
                physical_used -= candidate.bytes;
            } else {
                reclamation_healthy = false;
            }
            continue;
        }

        const std::uint64_t reclaimed         = candidate.kind == CandidateKind::CurrentSession
                                                    ? session_generation_bytes(candidate.path)
                                                    : directory_bytes(candidate.path);
        const std::filesystem::path tombstone = candidate_tombstone(options, candidate);
        try {
            replace_path(candidate.path, tombstone);
        } catch (...) {
            reclamation_healthy = false;
            continue;
        }
        if (!sync_rename_parents(candidate.path, tombstone)) { reclamation_healthy = false; }
        if (!cleanup_tombstone(options, tombstone)) {
            reclamation_healthy = false;
            continue;
        }
        inventory.used -= std::min(inventory.used, reclaimed);
        physical_used -= std::min(physical_used, reclaimed);
    }
    // At most one incoming set (itself <= quota) is tolerated. Stuck cleanup or
    // another publication's transient cannot accumulate unbounded excess.
    if (unpublished_bytes &&
        (!reclamation_healthy ||
         (physical_used > options.disk_quota_bytes &&
          physical_used - options.disk_quota_bytes > unpublished_bytes))) { return false; }
    if (inventory.used <= options.disk_quota_bytes) { return true; }
    return reclamation_healthy &&
           inventory.used <= options.disk_quota_bytes + tolerated_transient_bytes;
}

// A checkpoint payload entry and the digest the manifest promises for it.
struct VerifiedFile {
    std::uint64_t bytes;
    std::string sha256;
    std::uint32_t alignment = 1;
};
using VerifiedFiles = std::map<std::string, VerifiedFile, std::less<>>;

// Serves published payloads to the engine and verifies them as they stream: every payload is
// consumed exactly once, front to back, and the digest of the bytes handed to the engine must
// match the manifest before the final chunk is accepted. This is the only hash pass over a
// restore. Load-time hashing was dropped because it doubled the restore's dominant cost and
// verified bytes the engine never saw (the backend reopens the pathname on every submit,
// alphastorm/ninfer#21); hashing the streamed bytes closes that window by construction. A
// mismatch fails the import closed - the engine publishes nothing until every read succeeds -
// and marks the generation corrupt so the next load quarantines it.
class DirectoryCheckpointReader final : public ContinuationCheckpointReader {
public:
    DirectoryCheckpointReader(std::filesystem::path root,
                              std::shared_ptr<const VerifiedFiles> files,
                              std::shared_ptr<runtime::ContinuationCheckpointReadQueue> read_queue,
                              std::shared_ptr<SessionCheckpointStore::Impl> owner,
                              std::string active_key)
        : root_(std::move(root)), files_(std::move(files)), read_queue_(std::move(read_queue)),
          owner_(std::move(owner)), active_key_(std::move(active_key)) {
        // Constructed by SessionCheckpointStore::load while Impl::mutex is held.
        for (const auto& [path, file] : *files_) {
            if (path.starts_with("engine/") && file.bytes == 0) {
                if (crypto::sha256_hex(crypto::sha256({})) != file.sha256) {
                    throw CheckpointCorruption(runtime::DurableReason::PayloadDigestMismatch,
                                               "empty checkpoint payload digest mismatch");
                }
                verify_[path].verified = true;
            }
        }
        ++owner_->active_generations[active_key_];
    }

    ~DirectoryCheckpointReader() override {
        std::lock_guard lock(owner_->mutex);
        const auto found = owner_->active_generations.find(active_key_);
        if (found == owner_->active_generations.end() || found->second == 0) { std::terminate(); }
        if (--found->second == 0) {
            owner_->active_generations.erase(found);
            const auto corrupt = owner_->corrupt_generations.find(active_key_);
            if (corrupt != owner_->corrupt_generations.end() &&
                corrupt->second.quarantine_requested) {
                try {
                    if (quarantine_generation(root_.parent_path().parent_path(),
                                              root_.filename().string(), false)) {
                        owner_->corrupt_generations.erase(active_key_);
                    }
                } catch (...) {}
            }
        }
    }

    std::optional<std::uint64_t> file_size(std::string_view path) const override {
        const auto found = files_->find(path);
        return found == files_->end() ? std::nullopt
                                     : std::optional<std::uint64_t>(found->second.bytes);
    }

    std::optional<runtime::DurablePayload>
    file_descriptor(std::string_view path) const override {
        const auto found = files_->find(path);
        if (found == files_->end()) { return std::nullopt; }
        return runtime::DurablePayload{.path = found->first, .bytes = found->second.bytes,
                                       .sha256 = found->second.sha256,
                                       .alignment = found->second.alignment};
    }

    bool read_file(std::string_view path, std::uint64_t offset,
                   std::span<std::byte> destination) const override {
        std::lock_guard verify_lock(verify_mutex_);
        const auto found = files_->find(path);
        if (found == files_->end() || offset > found->second.bytes ||
            destination.size() > found->second.bytes - offset) {
            verification_failed_ = true;
            record_failure(found == files_->end() ? runtime::DurableReason::InvalidIndex
                                                 : runtime::DurableReason::PayloadSizeMismatch);
            return false;
        }
        if (!read_queue_->available()) {
            verification_failed_ = true;
            record_failure(runtime::DurableReason::IoUnavailable);
            return false;
        }
        ReadVerifyState& state = verify_[found->first];
        if (state.failed || state.verified || offset != state.next_offset) {
            verification_failed_ = true;
            record_failure(runtime::DurableReason::InvalidIndex);
            return false;
        }

        // The destination belongs to the engine, so no read may still be landing in it when
        // this call returns: every issued batch is awaited on every exit path.
        const std::filesystem::path file = root_ / found->first;
        std::vector<std::unique_ptr<runtime::ContinuationCheckpointReadCompletion>> issued;
        issued.reserve(2);
        const auto settle = [&]() noexcept {
            for (auto& completion : issued) {
                try {
                    completion->wait();
                } catch (...) {}
            }
            issued.clear();
        };
        try {
            // One batch hashes while the next is still on the device when the backend can hold
            // two in flight (io_uring); a serialising queue (DirectStorage) is drained between
            // batches, so hashing then follows each landing instead of overlapping it.
            const bool overlap = read_queue_->overlaps_batches();
            const auto issue = [&](std::size_t begin) {
                const std::size_t count = std::min(kReadBatchBytes, destination.size() - begin);
                const runtime::ContinuationCheckpointReadRequest request{
                    .file_offset = offset + begin,
                    .destination = destination.subspan(begin, count),
                };
                std::unique_ptr<runtime::ContinuationCheckpointReadCompletion> completion =
                    read_queue_->submit(file, std::span(&request, 1));
                if (!completion) {
                    throw CheckpointUnavailable("checkpoint native read returned no completion");
                }
                issued.push_back(std::move(completion));
                return count;
            };
            std::size_t landed_begin = 0;
            std::size_t landed_count = issue(0);
            for (;;) {
                const std::size_t next_begin = landed_begin + landed_count;
                std::size_t next_count       = 0;
                if (overlap && next_begin < destination.size()) { next_count = issue(next_begin); }
                issued.front()->wait();
                issued.erase(issued.begin());
                state.hasher.update(destination.subspan(landed_begin, landed_count));
                state.next_offset += landed_count;
                if (!overlap && next_begin < destination.size()) { next_count = issue(next_begin); }
                if (next_count == 0) { break; }
                landed_begin = next_begin;
                landed_count = next_count;
            }
            if (state.next_offset == found->second.bytes) {
                if (crypto::sha256_hex(state.hasher.finish()) != found->second.sha256) {
                    state.failed = true;
                    record_failure(runtime::DurableReason::PayloadDigestMismatch);
                    // Divergence from the manifest digest is corruption evidence, not a
                    // transient read error: surface it so the next load or status call
                    // quarantines this generation instead of advertising it available.
                    owner_->mark_corrupt(active_key_);
                    return false;
                }
                state.verified = true;
            }
            return true;
        } catch (...) {
            settle();
            state.failed = true;
            record_failure(runtime::DurableReason::IoUnavailable);
            return false;
        }
    }

    [[nodiscard]] bool verify_complete() const override {
        std::lock_guard verify_lock(verify_mutex_);
        if (verification_failed_ ||
            failure_reason_.load(std::memory_order_relaxed) != runtime::DurableReason::None) {
            return false;
        }
        bool has_engine_payload = false;
        for (const auto& [path, file] : *files_) {
            if (!path.starts_with("engine/")) { continue; }
            has_engine_payload = true;
            const auto state = verify_.find(path);
            if (state == verify_.end() || state->second.failed || !state->second.verified ||
                state->second.next_offset != file.bytes) {
                return false;
            }
        }
        return has_engine_payload;
    }

    [[nodiscard]] runtime::DurableReason failure_reason() const noexcept override {
        return failure_reason_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::shared_ptr<const ContinuationCheckpointReader>
    fresh_reader() const override {
        std::lock_guard lock(owner_->mutex);
        const auto corrupt = owner_->corrupt_generations.find(active_key_);
        if (corrupt != owner_->corrupt_generations.end()) {
            record_failure(corrupt->second.reason);
            return {};
        }
        return std::make_shared<DirectoryCheckpointReader>(
            root_, files_, read_queue_, owner_, active_key_);
    }

private:
    void record_failure(runtime::DurableReason reason) const noexcept {
        auto expected = runtime::DurableReason::None;
        (void)failure_reason_.compare_exchange_strong(expected, reason,
                                                      std::memory_order_relaxed);
    }

    // Sized so the io_uring backend keeps its whole queue depth busy per batch while the
    // previous batch is being hashed.
    static constexpr std::size_t kReadBatchBytes = 32ULL << 20;

    struct ReadVerifyState {
        crypto::Sha256 hasher;
        std::uint64_t next_offset = 0;
        bool failed               = false;
        bool verified             = false;
    };

    std::filesystem::path root_;
    std::shared_ptr<const VerifiedFiles> files_;
    std::shared_ptr<runtime::ContinuationCheckpointReadQueue> read_queue_;
    std::shared_ptr<SessionCheckpointStore::Impl> owner_;
    std::string active_key_;
    mutable std::mutex verify_mutex_;
    mutable std::map<std::string, ReadVerifyState> verify_;
    mutable bool verification_failed_ = false;
    mutable std::atomic<runtime::DurableReason> failure_reason_{runtime::DurableReason::None};
};

bool quarantine_generation(const std::filesystem::path& session,
                             const std::string& generation, bool pinned) noexcept {
    try {
        const auto current = current_generation(session);
        if (current && *current == generation) {
            (void)io::remove_tree(session / "current");
            sync_directory(session);
        }
        if (pinned) { return true; }
        const std::filesystem::path source = session / "generations" / generation;
        const std::filesystem::path target =
            session / "generations" /
            (generation + ".corrupt-" + std::to_string(unix_milliseconds()));
        replace_path(source, target);
        sync_directory(source.parent_path());
        return true;
    } catch (...) { return false; }
}

// Caller owns the catalog lock. The mark survives an unavailable rename; a pinned generation
// loses only its current pointer, keeping paths usable until the last reader retires.
[[nodiscard]] bool quarantine_generation_locked(SessionCheckpointStore::Impl& impl,
                                                 const std::filesystem::path& session,
                                                 const std::string& generation,
                                                 runtime::DurableReason reason) noexcept {
    try {
        const std::string key = session.filename().string() + "/" + generation;
        const auto corrupt =
            impl.corrupt_generations.try_emplace(
                key, SessionCheckpointStore::Impl::CorruptGeneration{reason, true}).first;
        corrupt->second.quarantine_requested = true;
        const bool pinned = impl.active_generations.contains(key);
        if (!quarantine_generation(session, generation, pinned)) { return false; }
        if (!pinned) { impl.corrupt_generations.erase(key); }
        return true;
    } catch (...) { return false; }
}

} // namespace

std::vector<std::byte> encode_response_store_snapshot(const OpenAIResponseStoreSnapshot& snapshot,
                                                      std::size_t byte_limit) {
    if (byte_limit == 0 || snapshot.client_session_sha256.empty() ||
        snapshot.latest_response_id.empty() || snapshot.records.empty()) {
        throw std::invalid_argument("response checkpoint snapshot is empty");
    }
    std::unordered_map<const OpenAIResponseContextNode*, std::uint64_t> context_ids;
    std::vector<OpenAIResponseContext> contexts;
    // Iterative ancestor walk: one stack frame regardless of session depth, so a
    // many-thousand-turn lineage cannot exhaust the serving thread's stack during
    // save (review CR-20260830 R7).
    const auto add_context = [&](const OpenAIResponseContext& context) {
        std::vector<const OpenAIResponseContext*> lineage;
        for (const OpenAIResponseContext* cursor = &context; *cursor != nullptr;
             cursor = &(*cursor)->parent) {
            if (context_ids.contains(cursor->get())) { break; }
            lineage.push_back(cursor);
        }
        for (auto it = lineage.rbegin(); it != lineage.rend(); ++it) {
            const std::uint64_t id = static_cast<std::uint64_t>(contexts.size()) + 1;
            context_ids.emplace((*it)->get(), id);
            contexts.push_back(**it);
        }
    };
    for (const StoredOpenAIResponse& response : snapshot.records) {
        add_context(response.context);
    }

    nlohmann::json encoded_contexts = nlohmann::json::array();
    for (const OpenAIResponseContext& context : contexts) {
        nlohmann::json turns = nlohmann::json::array();
        for (const ChatTurn& turn : context->turns) { turns.push_back(turn_to_json(turn)); }
        encoded_contexts.push_back(
            {{"id", context_ids.at(context.get())},
             {"parent", context->parent ? context_ids.at(context->parent.get()) : 0},
             {"turns", std::move(turns)}});
    }

    nlohmann::json records = nlohmann::json::array();
    for (const StoredOpenAIResponse& response : snapshot.records) {
        records.push_back(
            {{"id", response.id},
             {"session_key", response.session_key},
             {"client_session_sha256", response.client_session_sha256
                                           ? nlohmann::json(*response.client_session_sha256)
                                           : nlohmann::json(nullptr)},
             {"previous_response_id", response.previous_response_id
                                          ? nlohmann::json(*response.previous_response_id)
                                          : nlohmann::json(nullptr)},
             {"response", response.response},
             {"input_items", response.input_items},
             {"context", response.context ? context_ids.at(response.context.get()) : 0},
             {"publication_order", response.publication_order},
             {"preserve_thinking", response.preserve_thinking
                                       ? nlohmann::json(*response.preserve_thinking)
                                       : nlohmann::json(nullptr)}});
    }
    const nlohmann::json root      = {{"schema_version", kSessionCheckpointSchemaVersion},
                                      {"client_session_sha256", snapshot.client_session_sha256},
                                      {"latest_response_id", snapshot.latest_response_id},
                                      {"contexts", std::move(encoded_contexts)},
                                      {"records", std::move(records)}};
    std::vector<std::uint8_t> cbor = nlohmann::json::to_cbor(root);
    if (cbor.size() > byte_limit) {
        throw std::length_error("response checkpoint exceeds staging limit");
    }
    std::vector<std::byte> bytes(cbor.size());
    std::transform(cbor.begin(), cbor.end(), bytes.begin(),
                   [](std::uint8_t value) { return static_cast<std::byte>(value); });
    return bytes;
}

std::optional<OpenAIResponseStoreSnapshot>
decode_response_store_snapshot(std::span<const std::byte> bytes, std::size_t byte_limit) {
    if (bytes.empty() || bytes.size() > byte_limit) { return std::nullopt; }
    try {
        const auto* cbor    = reinterpret_cast<const std::uint8_t*>(bytes.data());
        nlohmann::json root = nlohmann::json::from_cbor(cbor, cbor + bytes.size(), true, true);
        if (!root.is_object() ||
            root.at("schema_version").get<std::uint32_t>() != kSessionCheckpointSchemaVersion ||
            !root.at("contexts").is_array() || !root.at("records").is_array()) {
            return std::nullopt;
        }
        OpenAIResponseStoreSnapshot snapshot;
        snapshot.client_session_sha256 =
            std::move(root.at("client_session_sha256").get_ref<std::string&>());
        snapshot.latest_response_id =
            std::move(root.at("latest_response_id").get_ref<std::string&>());
        if (snapshot.client_session_sha256.empty() || snapshot.latest_response_id.empty() ||
            root.at("records").empty()) {
            return std::nullopt;
        }
        std::vector<OpenAIResponseContext> contexts(root.at("contexts").size() + 1);
        std::size_t expected_id = 1;
        for (const nlohmann::json& encoded : root.at("contexts")) {
            if (encoded.at("id").get<std::size_t>() != expected_id ||
                !encoded.at("turns").is_array()) {
                return std::nullopt;
            }
            const std::size_t parent = encoded.at("parent").get<std::size_t>();
            if (parent >= expected_id) { return std::nullopt; }
            std::vector<ChatTurn> turns;
            turns.reserve(encoded.at("turns").size());
            for (const nlohmann::json& value : encoded.at("turns")) {
                std::optional<ChatTurn> turn = turn_from_json(value);
                if (!turn) { return std::nullopt; }
                turns.push_back(std::move(*turn));
            }
            contexts[expected_id] = append_openai_response_context(contexts[parent], std::move(turns));
            ++expected_id;
        }
        std::unordered_set<std::string> ids;
        bool has_latest = false;
        std::uint64_t previous_order = 0;
        snapshot.records.reserve(root.at("records").size());
        for (nlohmann::json& encoded : root.at("records")) {
            StoredOpenAIResponse response;
            response.id          = std::move(encoded.at("id").get_ref<std::string&>());
            response.session_key = std::move(encoded.at("session_key").get_ref<std::string&>());
            if (!encoded.at("client_session_sha256").is_null()) {
                response.client_session_sha256 =
                    std::move(encoded.at("client_session_sha256").get_ref<std::string&>());
            }
            if (!encoded.at("previous_response_id").is_null()) {
                response.previous_response_id =
                    std::move(encoded.at("previous_response_id").get_ref<std::string&>());
            }
            response.response = std::move(encoded.at("response"));
            response.input_items =
                std::move(encoded.at("input_items").get_ref<nlohmann::json::array_t&>());
            if (!unsigned_field(encoded, "publication_order")) { return std::nullopt; }
            response.publication_order = encoded.at("publication_order").get<std::uint64_t>();
            if (!encoded.at("preserve_thinking").is_null()) {
                response.preserve_thinking = encoded.at("preserve_thinking").get<bool>();
            }
            const std::size_t context  = encoded.at("context").get<std::size_t>();
            if (context >= contexts.size() || response.id.empty() || response.session_key.empty() ||
                !response.response.is_object() || !response.client_session_sha256 ||
                *response.client_session_sha256 != snapshot.client_session_sha256 ||
                response.publication_order <= previous_order ||
                !ids.insert(response.id).second) {
                return std::nullopt;
            }
            response.context = contexts[context];
            previous_order = response.publication_order;
            has_latest       = has_latest || response.id == snapshot.latest_response_id;
            snapshot.records.push_back(std::move(response));
        }
        return has_latest && snapshot.records.back().id == snapshot.latest_response_id
                   ? std::optional<OpenAIResponseStoreSnapshot>(std::move(snapshot))
                          : std::nullopt;
    } catch (...) { return std::nullopt; }
}

void SessionCheckpointSkipMessage::append(std::string_view value) noexcept {
    const std::size_t count = std::min(value.size(), bytes_.size() - size_);
    if (count != 0) { std::memcpy(bytes_.data() + size_, value.data(), count); }
    size_ += count;
}

void SessionCheckpointSkipMessage::number(std::uint64_t value) noexcept {
    std::array<char, 20> digits;
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    append({digits.data(), static_cast<std::size_t>(result.ptr - digits.data())});
}

SessionCheckpointSkipMessage format_session_checkpoint_skip(
    std::string_view prefix, std::string_view session_sha256,
    const runtime::SessionCheckpointSkipDetail& skip) noexcept {
    using Reason = runtime::SessionCheckpointSkipReason;
    using ExportReason = runtime::ContinuationExportSkipReason;
    SessionCheckpointSkipMessage message;
    message.append(prefix);
    const auto& detail = skip.export_detail;
    const bool store_gate = detail.reason == ExportReason::StoreInputInvalid ||
                            detail.reason == ExportReason::StoreStatsInvalid;
    const bool canonical_primary = skip.reason == Reason::None && !store_gate &&
                                   skip.durable_reason != runtime::DurableReason::None;
    if (canonical_primary) {
        message.append(runtime::durable_reason_name(skip.durable_reason));
    } else {
        message.append(skip.reason == Reason::None && store_gate
                           ? "checkpoint store refused export"
                           : runtime::session_checkpoint_skip_reason_name(skip.reason));
        if (skip.durable_reason != runtime::DurableReason::None) {
            message.append(" (reason=");
            message.append(runtime::durable_reason_name(skip.durable_reason));
            message.append(")");
        }
    }
    if (skip.first_reason != Reason::None && skip.first_reason != Reason::ProgramRejected &&
        skip.first_reason != skip.reason) {
        message.append(" (engine gate: ");
        message.append(runtime::session_checkpoint_skip_reason_name(skip.first_reason));
        message.append(")");
    }
    if (detail.reason != ExportReason::None) {
        message.append(store_gate ? " (store gate: " : " (program gate: ");
        message.append(runtime::continuation_export_skip_reason_name(detail.reason));
        if (detail.stage != runtime::ContinuationExportStage::None) {
            message.append("; stage=");
            message.append(runtime::continuation_export_stage_name(detail.stage));
        }
        switch (detail.kind) {
        case runtime::ContinuationExportKind::None:
            break;
        case runtime::ContinuationExportKind::State:
            message.append("; kind=state");
            break;
        case runtime::ContinuationExportKind::TextKv:
            message.append("; kind=text");
            break;
        case runtime::ContinuationExportKind::SpeculativeKv:
            message.append("; kind=speculative");
            break;
        }
        if (detail.ordinal) {
            message.append("; ordinal=");
            message.number(*detail.ordinal);
        }
        if (detail.required_bytes) {
            message.append("; required_bytes=");
            message.number(*detail.required_bytes);
        }
        if (detail.capacity_bytes) {
            message.append("; capacity_bytes=");
            message.number(*detail.capacity_bytes);
        }
        if (detail.occupied_bytes) {
            message.append("; occupied_bytes=");
            message.number(*detail.occupied_bytes);
        }
        if (detail.exception != runtime::ContinuationExportException::None) {
            message.append(detail.exception == runtime::ContinuationExportException::Standard
                               ? "; exception=standard" : "; exception=non-standard");
        }
        message.append(")");
    }
    message.append(" (session ");
    message.append(session_sha256.substr(0, 12));
    message.append(")");
    const auto tag = [&](std::string_view value) noexcept {
        if (value.empty()) {
            message.append("<empty>");
        } else if (value.size() > 256) {
            message.append(value.substr(0, 253));
            message.append("...");
        } else {
            message.append(value);
        }
        message.append(")");
    };
    if (!skip.attempted_tag.empty()) {
        message.append(" (attempted ");
        tag(skip.attempted_tag);
    }
    if (!skip.catalogued_tag.empty() || skip.first_reason == Reason::TagMismatch ||
        skip.reason == Reason::TagMismatch) {
        message.append(" (catalogued ");
        tag(skip.catalogued_tag);
    }
    return message;
}

SessionCheckpointStore::SessionCheckpointStore(SessionCheckpointStoreOptions options)
    : options_(std::move(options)), impl_(std::make_shared<Impl>()) {
    if (options_.root.empty() || options_.disk_quota_bytes == 0 || options_.staging_bytes == 0) {
        throw std::invalid_argument("checkpoint store options must be positive");
    }
    if (!options_.read_queue) {
        throw std::invalid_argument("checkpoint store requires a native read queue");
    }
    if (!options_.read_queue->available()) {
        throw std::invalid_argument("checkpoint read queue is unavailable: " +
                                    std::string(options_.read_queue->unavailable_reason()));
    }
    io::create_directories(options_.root / "sessions");
    io::create_directories(options_.root / ".tombstones");
    sync_directory(options_.root / "sessions");
    sync_directory(options_.root / ".tombstones");
    sync_directory(options_.root);
    collect_garbage();
}

bool SessionCheckpointStore::valid_digest(std::string_view digest) noexcept {
    return digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](unsigned char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

std::filesystem::path SessionCheckpointStore::session_path(std::string_view digest) const {
    if (!valid_digest(digest)) {
        throw std::invalid_argument("checkpoint session digest is invalid");
    }
    return options_.root / "sessions" / std::string(digest);
}

bool SessionCheckpointStore::may_hold(std::string_view session_sha256) const noexcept {
    if (!valid_digest(session_sha256)) { return false; }
    try {
        // load() needs the published current pointer; a session directory alone can be left by a
        // first save that was refused before it published.
        std::error_code error;
        const bool exists =
            std::filesystem::exists(session_path(session_sha256) / "current", error);
        // A filesystem error cannot rule the session out; load() decides under its lock.
        return exists || static_cast<bool>(error);
    } catch (...) { return true; }
}

std::optional<SessionCheckpointSaveResult>
SessionCheckpointStore::save(const OpenAIResponseStoreSnapshot& responses,
                             const nlohmann::json& runtime_fingerprint,
                             const ExportCheckpoint& exporter,
                             const PublishExport& publish_export,
                             runtime::SessionCheckpointSkipDetail* skip) {
    if (skip != nullptr) {
        skip->export_state = runtime::DurableExportState::Refused;
        skip->durable_reason = runtime::DurableReason::None;
    }
    if (!valid_digest(responses.client_session_sha256) ||
        !validate_session_checkpoint_runtime_fingerprint(runtime_fingerprint) ||
        !exporter || !publish_export) {
        if (skip != nullptr) {
            skip->durable_reason = runtime::DurableReason::InvalidManifest;
            runtime::ContinuationExportSkipDetail::record(
                &skip->export_detail, runtime::ContinuationExportSkipReason::StoreInputInvalid);
        }
        return std::nullopt;
    }
    if (runtime_fingerprint.at("responses").at("codec") != kResponseStoreSnapshotCodec) {
        if (skip != nullptr) { skip->durable_reason = runtime::DurableReason::UnsupportedCodec; }
        return std::nullopt;
    }
    const std::filesystem::path session     = session_path(responses.client_session_sha256);
    const std::filesystem::path generations = session / "generations";
    const std::uint64_t ordinal = impl_->sequence.fetch_add(1, std::memory_order_relaxed);
    std::string generation =
        std::to_string(unix_milliseconds()) + "-" + std::to_string(ordinal);
    CheckpointSaveLease save_lease(*impl_, session, responses.client_session_sha256, generation);
    if (!save_lease.admitted()) {
        if (skip != nullptr) {
            skip->reason = runtime::SessionCheckpointSkipReason::TransactionBusy;
            skip->durable_reason = runtime::DurableReason::TransactionBusy;
        }
        return std::nullopt;
    }
    std::unique_lock lock(impl_->mutex);
    io::create_directories(generations);
    const std::filesystem::path staging   = generations / (".staging-" + generation);
    const std::filesystem::path published = generations / generation;
    (void)io::remove_tree(staging);
    io::create_directories(staging);

    try {
        const std::vector<std::byte> response_bytes =
            encode_response_store_snapshot(responses, options_.staging_bytes);
        DirectoryCheckpointWriter writer(staging, options_.staging_bytes);
        if (!writer.write_store_file("responses.cbor", 0, response_bytes.size(), response_bytes)) {
            throw std::runtime_error("response checkpoint write failed");
        }
        // K3 streams the leased native set off-lock. Drain all accepted copies before publishing;
        // neither backpressure nor finish may run under the Engine execution lock (ninfer#34).
        BufferedCheckpointWriter buffered(writer, options_.write_buffer_bytes);
        lock.unlock();
        const runtime::DurableExportResult exported = exporter(buffered);
        if (skip != nullptr) {
            skip->export_state = exported.state;
            skip->durable_reason = exported.reason;
        }
        if (!buffered.finish()) {
            if (skip != nullptr) {
                skip->export_state = runtime::DurableExportState::Refused;
                skip->durable_reason = runtime::DurableReason::IoUnavailable;
            }
            throw std::runtime_error("checkpoint payload write failed");
        }
        lock.lock();
        if (exported.state != runtime::DurableExportState::Exported ||
            exported.reason != runtime::DurableReason::None ||
            !valid_checkpoint_stats(exported.stats)) {
            (void)remove_best_effort(staging);
            if (skip != nullptr) {
                if (exported.state == runtime::DurableExportState::Exported) {
                    skip->export_state = runtime::DurableExportState::Refused;
                }
                if (skip->durable_reason == runtime::DurableReason::None) {
                    skip->durable_reason = exported.state == runtime::DurableExportState::Stale
                                               ? runtime::DurableReason::StalePublication
                                               : runtime::DurableReason::InvalidIndex;
                }
                if (skip->first_reason == runtime::SessionCheckpointSkipReason::None) {
                    skip->first_reason = skip->reason;
                }
                if (exported.state == runtime::DurableExportState::Exported &&
                    skip->export_detail.reason ==
                                  runtime::ContinuationExportSkipReason::None) {
                    skip->export_detail = {
                        .reason = runtime::ContinuationExportSkipReason::StoreStatsInvalid};
                    skip->durable_reason = runtime::DurableReason::InvalidIndex;
                }
                // Retain fork log detail; durable_reason remains the policy authority.
                skip->reason = runtime::SessionCheckpointSkipReason::ProgramRejected;
            }
            return std::nullopt;
        }
        const auto* engine = &exported.stats;
        std::optional<std::vector<FileDescriptor>> payloads = writer.files();
        if (!payloads) { throw std::runtime_error("checkpoint payload set is incomplete"); }

        nlohmann::json checksum_body = nlohmann::json::array();
        for (const FileDescriptor& file : *payloads) {
            checksum_body.push_back(descriptor_json(file));
        }
        const std::string checksum_text = checksum_body.dump();
        if (!writer.write_store_file(
                "checksums.json", 0, checksum_text.size(),
                std::as_bytes(std::span(checksum_text.data(), checksum_text.size())))) {
            throw std::runtime_error("checkpoint checksum write failed");
        }
        payloads = writer.files();
        if (!payloads) { throw std::runtime_error("checkpoint checksum set is incomplete"); }

        std::uint64_t payload_bytes        = 0;
        std::uint64_t engine_payload_bytes = 0;
        nlohmann::json manifest_files      = nlohmann::json::array();
        for (const FileDescriptor& file : *payloads) {
            if (file.bytes > std::numeric_limits<std::uint64_t>::max() - payload_bytes) {
                throw std::overflow_error("checkpoint payload byte count overflowed");
            }
            payload_bytes += file.bytes;
            if (file.path.starts_with("engine/")) {
                if (file.bytes > std::numeric_limits<std::uint64_t>::max() - engine_payload_bytes) {
                    throw std::overflow_error("checkpoint engine byte count overflowed");
                }
                engine_payload_bytes += file.bytes;
            }
            manifest_files.push_back(descriptor_json(file));
        }
        if (engine_payload_bytes != engine->payload_bytes) {
            throw std::runtime_error("checkpoint engine payload summary is inconsistent");
        }
        const nlohmann::json manifest = {
            {"artifact_type", "ninfer_session_checkpoint"},
            {"schema_version", kSessionCheckpointSchemaVersion},
            {"session_sha256", responses.client_session_sha256},
            {"generation", generation},
            {"created_at_unix_ms", unix_milliseconds()},
            {"runtime_fingerprint", runtime_fingerprint},
            {"latest_response_id", responses.latest_response_id},
            {"response_records", responses.records.size()},
            {"frontier_tokens", engine->frontier_tokens},
            {"restored_tokens", engine->restored_tokens},
            {"engine_payload_bytes", engine->payload_bytes},
            {"payload_bytes", payload_bytes},
            {"files", std::move(manifest_files)},
        };
        require_manifest_shape(manifest);
        const std::string manifest_text = manifest.dump();
        if (manifest_text.size() > runtime::kDurableManifestMaxBytes) {
            throw std::length_error("checkpoint manifest exceeds its metadata limit");
        }
        write_synced(staging / "manifest.json", manifest_text);
        if (!options_.origin_mac_key.empty()) {
            write_synced(staging / "manifest.mac",
                         manifest_origin_mac_hex(options_.origin_mac_key, manifest_text));
        }
        sync_directory(staging);
        // A session's previously published generation is deliberately never evicted while its
        // successor stages (readers may hold it; a failed save must leave it untouched). Its
        // bytes are tolerated as a publish transient while reclamation is healthy; after
        // publish it becomes an ordinary stale candidate for quota enforcement. Without the
        // tolerance a session whose checkpoint exceeds half the quota could never save twice
        // (alphastorm/ninfer#25).
        std::uint64_t previous_bytes = 0;
        try {
            if (const std::optional<std::string> previous_current = current_generation(session)) {
                previous_bytes = directory_bytes(generations / *previous_current);
            }
        } catch (...) { previous_bytes = 0; }
        const std::uint64_t total_bytes = directory_bytes(staging);
        if (total_bytes > options_.disk_quota_bytes ||
            !enforce_disk_quota_locked(options_, *impl_, staging, session, previous_bytes,
                                      total_bytes)) {
            (void)remove_best_effort(staging);
            if (skip != nullptr) {
                skip->export_state = runtime::DurableExportState::Refused;
                skip->reason = runtime::SessionCheckpointSkipReason::QuotaExceeded;
                skip->durable_reason = runtime::DurableReason::Quota;
            }
            return std::nullopt;
        }

        replace_path(staging, published);
        sync_directory(generations);
        const std::filesystem::path current_staging = session / (".current-" + generation);
        write_synced(current_staging, generation + "\n");
        bool committed = false;
        bool pointer_replaced = false;
        bool invoked = false;
        bool quota_refused = false;
        lock.unlock();
        const auto publication = publish_export(exported.captured_epoch, [&] {
            std::lock_guard publication_lock(impl_->mutex);
            if (invoked) { return false; }
            invoked = true;
            if (!enforce_disk_quota_locked(options_, *impl_, published, session, previous_bytes,
                                          total_bytes)) {
                quota_refused = true;
                return false;
            }
            replace_path(current_staging, session / "current");
            pointer_replaced = true;
            sync_directory(session);
            // First save also publishes the session directory itself.
            sync_directory(options_.root / "sessions");
            committed = true;
            // Still under the catalog lock: the successful publication makes planned
            // current victims reclaimable; no intervening reader can change the plan.
            try {
                (void)enforce_disk_quota_locked(options_, *impl_, std::nullopt, session);
            } catch (...) {}
            return true;
        });
        lock.lock();
        if (publication.state != runtime::DurableExportState::Exported ||
            publication.reason != runtime::DurableReason::None || !committed) {
            (void)remove_best_effort(current_staging);
            if (!pointer_replaced) { (void)remove_best_effort(published); }
            if (skip != nullptr) {
                skip->export_state = publication.state == runtime::DurableExportState::Stale
                                         ? runtime::DurableExportState::Stale
                                         : runtime::DurableExportState::Refused;
                skip->durable_reason =
                    quota_refused ? runtime::DurableReason::Quota :
                    publication.reason != runtime::DurableReason::None
                        ? publication.reason
                        : publication.state == runtime::DurableExportState::Stale
                              ? runtime::DurableReason::StalePublication
                              : runtime::DurableReason::IoUnavailable;
            }
            return std::nullopt;
        }

        return SessionCheckpointSaveResult{
            .generation = std::move(generation), .engine = *engine, .bytes = total_bytes,
            .dir_sync = io::directory_sync_mode(),
            .origin_authenticated = !options_.origin_mac_key.empty()};
    } catch (...) {
        if (!lock.owns_lock()) { lock.lock(); }
        (void)remove_best_effort(staging);
        if (skip != nullptr && skip->durable_reason == runtime::DurableReason::None) {
            skip->export_state = runtime::DurableExportState::Refused;
            skip->durable_reason = runtime::DurableReason::IoUnavailable;
        }
        throw;
    }
}

SessionCheckpointLoadResult
SessionCheckpointStore::load(std::string_view session_sha256,
                             const nlohmann::json& runtime_fingerprint,
                             std::optional<std::string_view> required_response_id) {
    if (!valid_digest(session_sha256)) {
        return {.state = SessionCheckpointLoadState::Missing,
                 .reason = runtime::DurableReason::NoSessionRecords};
    }
    if (!validate_session_checkpoint_runtime_fingerprint(runtime_fingerprint)) {
        return {.state = SessionCheckpointLoadState::Incompatible,
                 .reason = runtime::DurableReason::FingerprintMismatch};
    }
    if (runtime_fingerprint.at("responses").at("codec") != kResponseStoreSnapshotCodec) {
        return {.state = SessionCheckpointLoadState::Incompatible,
                 .reason = runtime::DurableReason::UnsupportedCodec};
    }
    std::lock_guard lock(impl_->mutex);
    const std::filesystem::path session = session_path(session_sha256);
    std::optional<std::string> generation;
    try {
        generation = current_generation(session);
    } catch (const CheckpointCorruption&) {
        return {.state = SessionCheckpointLoadState::Corrupt,
                 .reason = runtime::DurableReason::InvalidManifest};
    } catch (...) {
        return {.state = SessionCheckpointLoadState::Unavailable,
                 .reason = runtime::DurableReason::IoUnavailable};
    }
    if (!generation) {
        return {.state = SessionCheckpointLoadState::Missing,
                 .reason = runtime::DurableReason::NoSessionRecords};
    }
    const std::filesystem::path root = session / "generations" / *generation;

    try {
        const std::string manifest_text = read_text_bounded(root / "manifest.json", 16ULL << 20);
        if (legacy_schema_envelope(manifest_text)) {
            return {.state = SessionCheckpointLoadState::Incompatible,
                     .reason = runtime::DurableReason::IncompatibleSchema};
        }
        // Verify origin before trusting any manifest content (alphastorm/ninfer#32). A
        // present-but-wrong MAC is tamper evidence and quarantines like corruption. An
        // absent MAC under the strict import posture is a REVERSIBLE policy refusal, not
        // evidence: report Incompatible (clean-replay path) and mutate nothing, so
        // clearing the flag restores the generation (council CR-20260831-originauth
        // grok R2). Off-strict, locally-produced legacy generations keep loading.
        const auto origin = manifest_origin_state(root, manifest_text, options_.origin_mac_key);
        switch (origin) {
        case ManifestOriginState::Authentic: break;
        case ManifestOriginState::Forged:
            throw CheckpointCorruption(runtime::DurableReason::OriginMacMismatch,
                                       "checkpoint manifest origin authentication failed");
        case ManifestOriginState::Unauthenticated:
            if (options_.require_origin_auth) {
                return {.state = SessionCheckpointLoadState::Incompatible,
                         .reason = runtime::DurableReason::OriginRequired};
            }
            break;
        }
        const nlohmann::json manifest = parse_canonical_json(manifest_text);
        if (manifest.is_object() && manifest.contains("schema_version") &&
            manifest.at("schema_version").is_number_unsigned() &&
            manifest.at("schema_version").get<std::uint64_t>() !=
                kSessionCheckpointSchemaVersion) {
            return {.state = SessionCheckpointLoadState::Incompatible,
                     .reason = runtime::DurableReason::IncompatibleSchema};
        }
        if (const auto corrupt = impl_->corrupt_generations.find(
                std::string(session_sha256) + "/" + *generation);
            corrupt != impl_->corrupt_generations.end()) {
            const auto reason = corrupt->second.reason;
            (void)quarantine_generation_locked(*impl_, session, *generation, reason);
            return {.state = SessionCheckpointLoadState::Corrupt, .reason = reason};
        }
        require_manifest_shape(manifest);
        if (!manifest.is_object() ||
            manifest.at("artifact_type").get<std::string>() != "ninfer_session_checkpoint" ||
            manifest.at("schema_version").get<std::uint32_t>() != kSessionCheckpointSchemaVersion ||
            manifest.at("session_sha256").get<std::string>() != session_sha256 ||
            manifest.at("generation").get<std::string>() != *generation) {
            throw CheckpointCorruption("checkpoint manifest identity is corrupt");
        }
        if (!validate_session_checkpoint_runtime_fingerprint(manifest.at("runtime_fingerprint")) ||
            manifest.at("runtime_fingerprint") != runtime_fingerprint) {
            return {.state = SessionCheckpointLoadState::Incompatible,
                     .reason = runtime::DurableReason::FingerprintMismatch};
        }
        if (!manifest.at("files").is_array() || manifest.at("files").empty() ||
            manifest.at("files").size() > 4096) {
            throw CheckpointCorruption("checkpoint manifest file set is corrupt");
        }
        VerifiedFiles allowed;
        std::uint64_t verified_bytes        = 0;
        std::uint64_t verified_engine_bytes = 0;
        for (const nlohmann::json& encoded : manifest.at("files")) {
            std::optional<FileDescriptor> expected = descriptor_from_json(encoded);
            if (!expected ||
                !allowed.emplace(expected->path, VerifiedFile{expected->bytes, expected->sha256,
                                                               expected->alignment})
                     .second) {
                throw CheckpointCorruption("checkpoint manifest file descriptor is corrupt");
            }
            require_payload_shape(root, expected->path, expected->bytes, options_.read_queue);
            if ((expected->path == "engine/index.cbor" || expected->path == "responses.cbor") &&
                expected->bytes > options_.staging_bytes) {
                return {.state = SessionCheckpointLoadState::Incompatible,
                         .reason = runtime::DurableReason::StagingLimit};
            }
            if (expected->bytes > std::numeric_limits<std::uint64_t>::max() - verified_bytes) {
                throw CheckpointCorruption("checkpoint verified byte count overflowed");
            }
            verified_bytes += expected->bytes;
            if (expected->path.starts_with("engine/")) {
                if (expected->bytes >
                    std::numeric_limits<std::uint64_t>::max() - verified_engine_bytes) {
                    throw CheckpointCorruption("checkpoint verified engine byte count overflowed");
                }
                verified_engine_bytes += expected->bytes;
            }
        }
        if (manifest.at("payload_bytes").get<std::uint64_t>() != verified_bytes ||
            manifest.at("engine_payload_bytes").get<std::uint64_t>() != verified_engine_bytes) {
            throw CheckpointCorruption("checkpoint manifest payload summary is corrupt");
        }
        const auto checksum_file = allowed.find("checksums.json");
        if (checksum_file == allowed.end()) {
            throw CheckpointCorruption("checkpoint checksum descriptor is corrupt");
        }
        if (checksum_file->second.bytes > options_.staging_bytes) {
            return {.state = SessionCheckpointLoadState::Incompatible,
                     .reason = runtime::DurableReason::StagingLimit};
        }
        const auto checksum_bytes = read_bounded(root / "checksums.json", options_.staging_bytes);
        if (checksum_bytes.size() != checksum_file->second.bytes ||
            crypto::sha256_hex(crypto::sha256(checksum_bytes)) != checksum_file->second.sha256) {
            throw CheckpointCorruption(runtime::DurableReason::PayloadDigestMismatch,
                                       "checkpoint checksum inventory digest mismatch");
        }
        nlohmann::json checksum_inventory = nlohmann::json::array();
        for (const auto& [path, file] : allowed) {
            if (path != "checksums.json") {
                checksum_inventory.push_back(
                    descriptor_json({.path = path, .bytes = file.bytes, .sha256 = file.sha256,
                                      .alignment = file.alignment}));
            }
        }
        const auto* checksum_begin =
            reinterpret_cast<const char*>(checksum_bytes.data());
        if (parse_canonical_json(std::string_view(checksum_begin, checksum_bytes.size())) !=
            checksum_inventory) {
            throw CheckpointCorruption("checkpoint checksum inventory differs from manifest");
        }
        const auto response_file = allowed.find("responses.cbor");
        if (response_file == allowed.end()) {
            throw CheckpointCorruption("checkpoint response payload descriptor is corrupt");
        }
        const std::vector<std::byte> response_bytes =
            read_bounded(root / "responses.cbor", options_.staging_bytes);
        // The response snapshot is the one payload consumed here rather than streamed by the
        // engine, so it is verified against its manifest digest on these exact bytes.
        if (response_bytes.size() != response_file->second.bytes ||
            crypto::sha256_hex(crypto::sha256(std::span<const std::byte>(response_bytes))) !=
                response_file->second.sha256) {
            throw CheckpointCorruption(runtime::DurableReason::PayloadDigestMismatch,
                                       "checkpoint response payload checksum mismatch");
        }
        std::optional<OpenAIResponseStoreSnapshot> responses =
            decode_response_store_snapshot(response_bytes, options_.staging_bytes);
        if (!responses || responses->client_session_sha256 != session_sha256 ||
            responses->latest_response_id != manifest.at("latest_response_id").get<std::string>() ||
            responses->records.size() != manifest.at("response_records").get<std::size_t>()) {
            throw CheckpointCorruption("checkpoint response state is corrupt");
        }
        if (required_response_id &&
            std::none_of(responses->records.begin(), responses->records.end(),
                         [&](const StoredOpenAIResponse& response) {
                             return response.id == *required_response_id;
                         })) {
            return {.state = SessionCheckpointLoadState::Missing,
                     .reason = runtime::DurableReason::RequiredResponseMissing};
        }
        DurableCheckpointStats expected{
            .frontier_tokens = manifest.at("frontier_tokens").get<std::uint32_t>(),
            .restored_tokens = manifest.at("restored_tokens").get<std::uint32_t>(),
            .payload_bytes   = manifest.at("engine_payload_bytes").get<std::uint64_t>(),
        };
        if (!valid_checkpoint_stats(expected)) {
            throw CheckpointCorruption("checkpoint engine summary is corrupt");
        }
        const std::uint64_t generation_bytes =
            options_.generation_size ? options_.generation_size(root) : directory_bytes(root);
        const std::string active_key = std::string(session_sha256) + "/" + *generation;
        VerifiedSessionCheckpoint verified;
        verified.responses       = std::move(*responses);
        verified.expected_engine = expected;
        verified.generation      = *generation;
        verified.bytes           = generation_bytes;
        verified.origin_authenticated = origin == ManifestOriginState::Authentic;
        auto reader              = std::make_shared<DirectoryCheckpointReader>(
            root, std::make_shared<const VerifiedFiles>(std::move(allowed)),
            options_.read_queue, impl_, active_key);
        verified.engine = std::move(reader);
        return {.state = SessionCheckpointLoadState::Available, .checkpoint = std::move(verified)};
    } catch (const CheckpointCorruption& error) {
        (void)quarantine_generation_locked(*impl_, session, *generation, error.reason);
        return {.state = SessionCheckpointLoadState::Corrupt, .reason = error.reason};
    } catch (const CheckpointUnavailable& error) {
        return {.state = SessionCheckpointLoadState::Unavailable, .reason = error.reason};
    } catch (const nlohmann::json::exception&) {
        (void)quarantine_generation_locked(*impl_, session, *generation,
                                            runtime::DurableReason::InvalidManifest);
        return {.state = SessionCheckpointLoadState::Corrupt,
                 .reason = runtime::DurableReason::InvalidManifest};
    } catch (...) {
        return {.state = SessionCheckpointLoadState::Unavailable,
                 .reason = runtime::DurableReason::IoUnavailable};
    }
}

bool SessionCheckpointStore::covers(std::string_view session_sha256,
                                    const nlohmann::json& runtime_fingerprint,
                                    std::string_view response_id) const {
    if (!valid_digest(session_sha256) ||
        !validate_session_checkpoint_runtime_fingerprint(runtime_fingerprint) ||
        runtime_fingerprint.at("responses").at("codec") != kResponseStoreSnapshotCodec ||
        response_id.empty()) {
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    try {
        const std::filesystem::path session         = session_path(session_sha256);
        const std::optional<std::string> generation = current_generation(session);
        if (!generation) { return false; }
        if (impl_->is_corrupt(std::string(session_sha256) + "/" + *generation)) { return false; }
        const std::filesystem::path root = session / "generations" / *generation;
        const std::string manifest_text = read_text_bounded(root / "manifest.json", 16ULL << 20);
        const ManifestOriginState origin =
            manifest_origin_state(root, manifest_text, options_.origin_mac_key);
        if (origin == ManifestOriginState::Forged ||
            (origin == ManifestOriginState::Unauthenticated && options_.require_origin_auth)) {
            // A skip decision must never trust an unauthenticated catalogue entry: fall
            // through to a real save instead of skipping against forged state.
            return false;
        }
        const nlohmann::json manifest = parse_canonical_json(manifest_text);
        require_manifest_shape(manifest);
        return manifest.is_object() &&
               manifest.at("artifact_type").get<std::string>() == "ninfer_session_checkpoint" &&
               manifest.at("schema_version").get<std::uint32_t>() ==
                   kSessionCheckpointSchemaVersion &&
               manifest.at("session_sha256").get<std::string>() == session_sha256 &&
               manifest.at("generation").get<std::string>() == *generation &&
               manifest.at("runtime_fingerprint") == runtime_fingerprint &&
               manifest.at("latest_response_id").get<std::string>() == response_id;
    } catch (...) { return false; }
}

nlohmann::json SessionCheckpointStore::status(std::string_view session_sha256,
                                              const nlohmann::json& runtime_fingerprint) const {
    nlohmann::json result = {{"artifact_type", "ninfer_session_checkpoint_status"},
                             {"schema_version", kSessionCheckpointSchemaVersion},
                             {"read_backend", std::string(options_.read_queue->backend_name())},
                             {"dir_sync", io::directory_sync_mode()},
                             {"state", "missing"}};
    if (!valid_digest(session_sha256)) { return result; }
    if (!validate_session_checkpoint_runtime_fingerprint(runtime_fingerprint) ||
        runtime_fingerprint.at("responses").at("codec") != kResponseStoreSnapshotCodec) {
        result["state"] = "incompatible";
        return result;
    }
    std::lock_guard lock(impl_->mutex);
    try {
        const std::filesystem::path session         = session_path(session_sha256);
        const std::optional<std::string> generation = current_generation(session);
        if (!generation) { return result; }
        const std::filesystem::path root = session / "generations" / *generation;
        const std::string manifest_text = read_text_bounded(root / "manifest.json", 16ULL << 20);
        if (legacy_schema_envelope(manifest_text)) {
            result["state"] = "incompatible";
            return result;
        }
        switch (manifest_origin_state(root, manifest_text, options_.origin_mac_key)) {
        case ManifestOriginState::Authentic: break;
        case ManifestOriginState::Forged:
            throw CheckpointCorruption("checkpoint manifest origin authentication failed");
        case ManifestOriginState::Unauthenticated:
            if (options_.require_origin_auth) {
                result["state"] = "incompatible";
                return result;
            }
            break;
        }
        const nlohmann::json manifest = parse_canonical_json(manifest_text);
        if (manifest.is_object() && manifest.contains("schema_version") &&
            manifest.at("schema_version").is_number_unsigned() &&
            manifest.at("schema_version").get<std::uint64_t>() !=
                kSessionCheckpointSchemaVersion) {
            result["state"] = "incompatible";
            return result;
        }
        require_manifest_shape(manifest);
        if (impl_->is_corrupt(std::string(session_sha256) + "/" + *generation)) {
            result["state"] = "corrupt";
            return result;
        }
        if (!manifest.is_object() ||
            manifest.at("artifact_type").get<std::string>() != "ninfer_session_checkpoint" ||
            manifest.at("schema_version").get<std::uint32_t>() != kSessionCheckpointSchemaVersion ||
            manifest.at("session_sha256").get<std::string>() != session_sha256 ||
            manifest.at("generation").get<std::string>() != *generation) {
            throw CheckpointCorruption("checkpoint manifest identity is corrupt");
        }
        result["state"]              =
            validate_session_checkpoint_runtime_fingerprint(manifest.at("runtime_fingerprint")) &&
                    manifest.at("runtime_fingerprint") == runtime_fingerprint
                                           ? "available"
                                           : "incompatible";
        result["generation"]         = *generation;
        result["created_at_unix_ms"] = manifest.at("created_at_unix_ms");
        result["bytes"]              = directory_bytes(root);
        result["frontier_tokens"]    = manifest.at("frontier_tokens");
        result["restored_tokens"]    = manifest.at("restored_tokens");
        result["response_records"]   = manifest.at("response_records");
        return result;
    } catch (const CheckpointCorruption&) {
        result["state"] = "corrupt";
        return result;
    } catch (const nlohmann::json::exception&) {
        result["state"] = "corrupt";
        return result;
    } catch (...) {
        // The released OMP client's checkpoint-state vocabulary is closed
        // (available/missing/incompatible/corrupt/disabled/deleted); a transient
        // store failure reports "disabled" so the pinned client falls back to
        // transcript replay instead of failing the status parse.
        result["state"] = "disabled";
        return result;
    }
}

SessionCheckpointEraseResult SessionCheckpointStore::erase(std::string_view session_sha256) {
    if (!valid_digest(session_sha256)) { return SessionCheckpointEraseResult::Missing; }
    std::lock_guard lock(impl_->mutex);
    const std::string prefix = std::string(session_sha256) + "/";
    if (std::any_of(impl_->active_generations.begin(), impl_->active_generations.end(),
                    [&](const auto& entry) { return entry.first.starts_with(prefix); })) {
        return SessionCheckpointEraseResult::Conflict;
    }
    const std::filesystem::path session = session_path(session_sha256);
    std::filesystem::path tombstone;
    try {
        tombstone = unique_tombstone(options_, std::string(session_sha256) + "--session");
    } catch (...) { return SessionCheckpointEraseResult::Conflict; }
    try {
        replace_path(session, tombstone);
    } catch (const std::system_error& error) {
        if (error.code() == std::errc::no_such_file_or_directory) {
            return SessionCheckpointEraseResult::Missing;
        }
        return SessionCheckpointEraseResult::Conflict;
    } catch (...) { return SessionCheckpointEraseResult::Conflict; }
    if (!sync_rename_parents(session, tombstone)) {
        // The rename may not be durable; report a retryable failure instead of
        // acknowledging a deletion that a crash could resurrect (review CR-20260830 R2).
        return SessionCheckpointEraseResult::Conflict;
    }
    (void)cleanup_tombstone(options_, tombstone);
    return SessionCheckpointEraseResult::Erased;
}

bool SessionCheckpointStore::quarantine(std::string_view session_sha256,
                                        std::string_view generation,
                                        runtime::DurableReason reason) noexcept {
    if (!valid_digest(session_sha256) || !valid_generation_name(generation)) { return false; }
    switch (reason) {
    case runtime::DurableReason::OriginMacMismatch:
    case runtime::DurableReason::InvalidManifest:
    case runtime::DurableReason::InvalidIndex:
    case runtime::DurableReason::PayloadSizeMismatch:
    case runtime::DurableReason::PayloadDigestMismatch: break;
    default: return false;
    }
    try {
        std::lock_guard lock(impl_->mutex);
        const auto session = session_path(session_sha256);
        const std::string name(generation);
        const std::string key = std::string(session_sha256) + "/" + name;
        if (!impl_->active_generations.contains(key) &&
            !std::filesystem::is_directory(session / "generations" / name)) {
            return false;
        }
        return quarantine_generation_locked(*impl_, session, name, reason);
    } catch (...) { return false; }
}

void SessionCheckpointStore::collect_garbage() {
    std::lock_guard lock(impl_->mutex);
    (void)enforce_disk_quota_locked(options_, *impl_);
}

} // namespace ninfer::serve
