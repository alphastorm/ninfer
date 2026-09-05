#include "serve/session_checkpoint_store.h"

#include "core/sha256.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <fstream>
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

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ninfer::serve {
namespace {

using Clock = std::chrono::system_clock;
using runtime::ContinuationCheckpointReader;
using runtime::ContinuationCheckpointReadCompletion;
using runtime::ContinuationCheckpointReadQueue;
using runtime::ContinuationCheckpointReadRequest;
using runtime::ContinuationCheckpointStats;
using runtime::ContinuationCheckpointWriter;
using runtime::AuthenticatedCheckpointNamespace;
inline constexpr std::size_t kMaximumTombstoneAttempts = 4096;
// Matches DSTORAGE_STAGING_BUFFER_SIZE_32MB configured by the DirectStorage read queue; a
// larger reader call becomes several requests in one batch.
inline constexpr std::size_t kDirectStorageRequestBytes = 32ULL << 20;
static_assert(std::is_nothrow_move_constructible_v<ResponseStoreSnapshot>);
static_assert(std::is_nothrow_move_constructible_v<VerifiedSessionCheckpoint>);

[[nodiscard]] std::string namespace_storage_digest(
    const AuthenticatedCheckpointNamespace& checkpoint_namespace) {
    constexpr std::string_view domain = "ninfer-checkpoint-namespace-v1";
    crypto::Sha256 hasher;
    hasher.update(std::as_bytes(std::span(domain.data(), domain.size())));
    hasher.update(std::as_bytes(std::span(checkpoint_namespace.tenant_sha256().data(),
                                          checkpoint_namespace.tenant_sha256().size())));
    hasher.update(std::as_bytes(std::span(checkpoint_namespace.session_sha256().data(),
                                          checkpoint_namespace.session_sha256().size())));
    return crypto::sha256_hex(hasher.finish());
}

class CheckpointCorruption final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class CheckpointUnavailable final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct FileDescriptor {
    std::string path;
    std::uint64_t bytes = 0;
    std::string sha256;
};

[[nodiscard]] bool valid_checkpoint_stats(const ContinuationCheckpointStats& stats) noexcept {
    return stats.frontier_tokens != 0 && stats.restored_tokens != 0 &&
           stats.restored_tokens <= stats.frontier_tokens && stats.payload_bytes != 0 &&
           static_cast<std::uint64_t>(stats.restored_tokens) * 100ULL >=
               static_cast<std::uint64_t>(stats.frontier_tokens) * 95ULL;
}

[[nodiscard]] SessionCheckpointLoadResult load_state(SessionCheckpointLoadState state) {
    return {.state = state, .checkpoint = std::nullopt};
}

[[nodiscard]] std::uint64_t unix_milliseconds() {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now().time_since_epoch())
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

[[nodiscard]] std::FILE* open_binary_write(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

[[nodiscard]] std::FILE* open_binary_read(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

void flush_file(std::FILE* file) {
    if (std::fflush(file) != 0) { throw std::runtime_error("checkpoint file flush failed"); }
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) { throw std::runtime_error("checkpoint file sync failed"); }
#else
    if (::fsync(::fileno(file)) != 0) { throw std::runtime_error("checkpoint file sync failed"); }
#endif
}

void sync_directory(const std::filesystem::path& path) {
#ifndef _WIN32
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { throw std::runtime_error("checkpoint directory open failed"); }
    const int result = ::fsync(fd);
    const int saved  = errno;
    ::close(fd);
    if (result != 0) {
        errno = saved;
        throw std::runtime_error("checkpoint directory sync failed");
    }
#else
    (void)path;
#endif
}

void replace_path(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("checkpoint atomic replacement failed");
    }
#else
    if (::rename(source.c_str(), destination.c_str()) != 0) {
        throw std::runtime_error("checkpoint atomic replacement failed");
    }
#endif
}

void write_synced(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::FILE* file = open_binary_write(path);
    if (file == nullptr) { throw std::runtime_error("checkpoint file creation failed"); }
    try {
        if (!bytes.empty() &&
            std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
            throw std::runtime_error("checkpoint file write failed");
        }
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

[[nodiscard]] std::vector<std::byte> read_bounded(const std::filesystem::path& path,
                                                  std::size_t limit) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) { throw CheckpointUnavailable("checkpoint file size is unavailable"); }
    if (size > limit || size > std::numeric_limits<std::size_t>::max()) {
        throw CheckpointCorruption("checkpoint file size exceeds its bound");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::FILE* file = open_binary_read(path);
    if (file == nullptr) { throw CheckpointUnavailable("checkpoint file open failed"); }
    const std::size_t read =
        bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
    const bool failed = read != bytes.size() || std::ferror(file) != 0;
    const bool close_failed = std::fclose(file) != 0;
    if (failed || close_failed) { throw CheckpointUnavailable("checkpoint file read failed"); }
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
    if (key.empty()) { return ManifestOriginState::Unauthenticated; }
    std::error_code probe_error;
    const bool tag_exists = std::filesystem::exists(root / "manifest.mac", probe_error);
    if (probe_error) {
        // A transient inability to even probe the tag is a filesystem fault, never origin
        // evidence: preserve current for retry.
        throw CheckpointUnavailable("checkpoint origin tag probe failed");
    }
    if (!tag_exists) { return ManifestOriginState::Unauthenticated; }
    // read_text_bounded throws CheckpointUnavailable on open/size/read faults; let it
    // propagate so a transient tag read reports Unavailable instead of quarantining a
    // valid generation. Only structural badness or a mismatch is forgery evidence.
    std::string recorded = read_text_bounded(root / "manifest.mac", 128);
    while (!recorded.empty() && (recorded.back() == '\n' || recorded.back() == '\r')) {
        recorded.pop_back();
    }
    if (recorded.size() != 64) { return ManifestOriginState::Forged; }
    const std::string expected = manifest_origin_mac_hex(key, manifest_text);
    return constant_time_hex_equal(recorded, expected) ? ManifestOriginState::Authentic
                                                       : ManifestOriginState::Forged;
}

[[nodiscard]] FileDescriptor hash_file(const std::filesystem::path& root,
                                       const std::string& relative,
                                       std::uint64_t expected_bytes) {
    const std::filesystem::path path = root / relative;
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(path, error);
    if (error || !std::filesystem::exists(status)) {
        throw CheckpointUnavailable("checkpoint payload is unavailable");
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw CheckpointCorruption("checkpoint payload is not a regular file");
    }
    const std::uintmax_t actual_bytes = std::filesystem::file_size(path, error);
    if (error) { throw CheckpointUnavailable("checkpoint payload size is unavailable"); }
    if (actual_bytes != expected_bytes) {
        throw CheckpointCorruption("checkpoint payload size mismatch");
    }
    std::FILE* file = open_binary_read(path);
    if (file == nullptr) { throw CheckpointUnavailable("checkpoint payload open failed"); }
    crypto::Sha256 hasher;
    std::array<std::byte, 64ULL << 10> buffer{};
    std::uint64_t consumed = 0;
    try {
        while (consumed < expected_bytes) {
            const std::size_t requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), expected_bytes - consumed));
            const std::size_t count = std::fread(buffer.data(), 1, requested, file);
            if (count != requested) {
                throw CheckpointUnavailable("checkpoint payload read failed");
            }
            hasher.update(std::span<const std::byte>(buffer.data(), count));
            consumed += count;
        }
        if (std::fgetc(file) != EOF) {
            throw CheckpointCorruption("checkpoint payload grew");
        }
        if (std::ferror(file) != 0) {
            throw CheckpointUnavailable("checkpoint payload read failed");
        }
        if (std::fclose(file) != 0) {
            file = nullptr;
            throw CheckpointUnavailable("checkpoint payload close failed");
        }
        file = nullptr;
    } catch (...) {
        if (file != nullptr) { std::fclose(file); }
        throw;
    }
    return {.path = relative,
            .bytes = expected_bytes,
            .sha256 = crypto::sha256_hex(hasher.finish())};
}

[[nodiscard]] std::uint64_t directory_bytes(const std::filesystem::path& path) {
    std::error_code error;
    std::uint64_t total = 0;
    for (std::filesystem::recursive_directory_iterator iterator(
             path, std::filesystem::directory_options::skip_permission_denied, error),
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

template <class RoleValue>
[[nodiscard]] std::string checkpoint_role_name(const RoleValue& role_value) {
    using Role = std::remove_cvref_t<RoleValue>;
    if constexpr (std::is_same_v<Role, std::string>) {
        return role_value;
    } else {
        switch (static_cast<std::uint8_t>(role_value)) {
        case 0:
            return "system";
        case 1:
            return "developer";
        case 2:
            return "user";
        case 3:
            return "assistant";
        case 4:
            return "tool";
        default:
            throw std::invalid_argument("checkpoint response role is invalid");
        }
    }
}

template <class RoleValue>
[[nodiscard]] bool restore_checkpoint_role(RoleValue& role_value, std::string_view role) {
    using Role = std::remove_cvref_t<RoleValue>;
    if (role != "system" && role != "developer" && role != "user" &&
        role != "assistant" && role != "tool") {
        return false;
    }
    if constexpr (std::is_same_v<Role, std::string>) {
        role_value = role;
    } else {
        const std::uint8_t value = role == "system"    ? 0U
                                   : role == "developer" ? 1U
                                   : role == "user"      ? 2U
                                   : role == "assistant" ? 3U
                                                           : 4U;
        role_value = static_cast<Role>(value);
    }
    return true;
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

nlohmann::json turn_to_json(const ChatTurn& turn) {
    nlohmann::json content = nlohmann::json::array();
    for (const ContentPart& part : turn.content) {
        content.push_back({{"kind", static_cast<std::uint8_t>(part.kind)},
                           {"text", part.text},
                           {"type_raw", part.type_raw},
                           {"source", source_to_json(part.source)}});
    }
    nlohmann::json calls = nlohmann::json::array();
    for (const ToolCall& call : turn.tool_calls) {
        calls.push_back({{"id", call.id},
                         {"name", call.name},
                         {"arguments_json", call.arguments_json}});
    }
    return {{"role", checkpoint_role_name(turn.role)},
            {"content", std::move(content)},
            {"tool_calls", std::move(calls)},
            {"tool_call_id", turn.tool_call_id},
            {"reasoning_content", turn.reasoning_content},
            {"shared_cache_boundaries_after_text_bytes",
             turn.shared_cache_boundaries_after_text_bytes}};
}

std::optional<ChatTurn> turn_from_json(const nlohmann::json& value) {
    if (!value.is_object() || !value.at("content").is_array() ||
        !value.at("tool_calls").is_array()) {
        return std::nullopt;
    }
    if (!value.at("role").is_string()) {
        return std::nullopt;
    }
    const std::string role = value.at("role").get<std::string>();
    ChatTurn turn;
    if (!restore_checkpoint_role(turn.role, role)) { return std::nullopt; }
    turn.tool_call_id      = value.at("tool_call_id").get<std::string>();
    turn.reasoning_content = value.at("reasoning_content").get<std::string>();
    turn.shared_cache_boundaries_after_text_bytes =
        value.at("shared_cache_boundaries_after_text_bytes").get<std::vector<std::uint32_t>>();
    for (const nlohmann::json& encoded : value.at("content")) {
        const int kind = encoded.at("kind").get<int>();
        if (kind < static_cast<int>(ContentKind::Text) ||
            kind > static_cast<int>(ContentKind::Unsupported)) {
            return std::nullopt;
        }
        std::optional<product::media_acquire::Source> source =
            source_from_json(encoded.at("source"));
        if (!source) { return std::nullopt; }
        turn.content.push_back(ContentPart{.kind = static_cast<ContentKind>(kind),
                                           .text = encoded.at("text").get<std::string>(),
                                           .type_raw = encoded.at("type_raw").get<std::string>(),
                                           .source = std::move(*source)});
    }
    for (const nlohmann::json& encoded : value.at("tool_calls")) {
        turn.tool_calls.push_back(ToolCall{.id = encoded.at("id").get<std::string>(),
                                           .name = encoded.at("name").get<std::string>(),
                                           .arguments_json =
                                               encoded.at("arguments_json").get<std::string>()});
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
                    std::span<const std::byte> bytes) override {
        if (!path.starts_with("engine/") || path.size() == std::string_view("engine/").size()) {
            failed_ = true;
            return false;
        }
        return write_impl(path, offset, total_bytes, bytes);
    }

    bool write_store_file(std::string_view path, std::uint64_t offset,
                          std::uint64_t total_bytes, std::span<const std::byte> bytes) {
        return write_impl(path, offset, total_bytes, bytes);
    }

    [[nodiscard]] std::optional<std::vector<FileDescriptor>> files() const {
        if (failed_) { return std::nullopt; }
        std::vector<FileDescriptor> out;
        out.reserve(states_.size());
        for (const auto& [path, state] : states_) {
            if (!state.complete || state.file != nullptr || state.written != state.total) {
                return std::nullopt;
            }
            out.push_back({.path = path, .bytes = state.total, .sha256 = state.digest});
        }
        return out;
    }

private:
    bool write_impl(std::string_view path, std::uint64_t offset, std::uint64_t total_bytes,
                    std::span<const std::byte> bytes) {
        if (failed_ || !valid_relative_path(path) || bytes.size() > staging_limit_ ||
            offset > total_bytes || bytes.size() > total_bytes - offset) {
            failed_ = true;
            return false;
        }
        try {
            const std::string key(path);
            auto [position, inserted] = states_.try_emplace(key);
            State& state              = position->second;
            if (inserted) {
                if (offset != 0 || states_.size() > 4096) {
                    failed_ = true;
                    return false;
                }
                state.total = total_bytes;
                const std::filesystem::path destination = root_ / key;
                std::filesystem::create_directories(destination.parent_path());
                state.file = open_binary_write(destination);
                if (state.file == nullptr) {
                    failed_ = true;
                    return false;
                }
            }
            if (state.complete || state.total != total_bytes || state.written != offset ||
                state.file == nullptr) {
                failed_ = true;
                return false;
            }
            if (!bytes.empty() &&
                std::fwrite(bytes.data(), 1, bytes.size(), state.file) != bytes.size()) {
                failed_ = true;
                return false;
            }
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
    };

    std::filesystem::path root_;
    std::size_t staging_limit_ = 0;
    std::map<std::string, State> states_;
    bool failed_ = false;
};

// Decouples the engine-held export from disk latency (ninfer#34): the engine's exporter runs
// under the Engine execution lock, so every microsecond it spends inside write_file serializes
// queued requests behind the save. This writer copies each chunk into a bounded in-memory queue
// and returns, while one drain thread performs the real DirectoryCheckpointWriter writes.
// finish() joins the drain and reports whether every deferred write succeeded; a deferred
// failure fails the save exactly like an inline write failure, before anything publishes.
class BufferedCheckpointWriter final : public ContinuationCheckpointWriter {
public:
    BufferedCheckpointWriter(DirectoryCheckpointWriter& inner, std::size_t capacity_bytes)
        : inner_(inner), capacity_(capacity_bytes == 0 ? 1 : capacity_bytes) {
        drain_ = std::thread([this] { drain(); });
    }

    ~BufferedCheckpointWriter() override { (void)finish(); }

    bool write_file(std::string_view path, std::uint64_t offset, std::uint64_t total_bytes,
                    std::span<const std::byte> bytes) override {
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
    [[nodiscard]] bool finish() noexcept {
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
        return !failed_;
    }

private:
    struct Chunk {
        std::string path;
        std::uint64_t offset = 0;
        std::uint64_t total  = 0;
        std::vector<std::byte> bytes;
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
                inner_.write_file(chunk.path, chunk.offset, chunk.total, chunk.bytes);
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
    return {{"path", file.path}, {"bytes", file.bytes}, {"sha256", file.sha256}};
}

[[nodiscard]] std::optional<FileDescriptor> descriptor_from_json(const nlohmann::json& value) {
    if (!value.is_object() || !value.at("path").is_string() ||
        !value.at("sha256").is_string()) {
        return std::nullopt;
    }
    FileDescriptor descriptor{.path = value.at("path").get<std::string>(),
                              .bytes = value.at("bytes").get<std::uint64_t>(),
                              .sha256 = value.at("sha256").get<std::string>()};
    if (!valid_relative_path(descriptor.path) || descriptor.sha256.size() != 64 ||
        !std::all_of(descriptor.sha256.begin(), descriptor.sha256.end(), [](unsigned char byte) {
            return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
        })) {
        return std::nullopt;
    }
    return descriptor;
}

[[nodiscard]] std::optional<std::string> current_generation(const std::filesystem::path& session) {
    std::error_code error;
    const std::filesystem::path path = session / "current";
    const std::filesystem::file_status status = std::filesystem::status(path, error);
    if (error == std::errc::no_such_file_or_directory) { return std::nullopt; }
    if (error) { throw CheckpointUnavailable("checkpoint current pointer is unavailable"); }
    if (!std::filesystem::exists(status)) { return std::nullopt; }
    if (!std::filesystem::is_regular_file(status)) {
        throw CheckpointCorruption("checkpoint current pointer is not a regular file");
    }
    std::string value = read_text_bounded(path, 256);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) { value.pop_back(); }
    if (value.empty() || !valid_relative_path(value) || value.find('/') != std::string::npos ||
        value.starts_with('.')) {
        throw CheckpointCorruption("checkpoint current pointer is invalid");
    }
    return value;
}

} // namespace

class SessionCheckpointStore::Impl : public std::enable_shared_from_this<Impl> {
public:
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::uint32_t> active_generations;
    std::atomic<std::uint64_t> sequence{0};
};

namespace {

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

[[nodiscard]] GenerationInventory inventory_generations(
    const SessionCheckpointStoreOptions& options, const SessionCheckpointStore::Impl& impl,
    const std::optional<std::filesystem::path>& protected_generation = std::nullopt,
    const std::optional<std::filesystem::path>& protected_session = std::nullopt) {
    GenerationInventory inventory;
    const std::filesystem::path tombstones = options.root / ".tombstones";
    for (const auto& entry : std::filesystem::directory_iterator(tombstones)) {
        const std::uint64_t bytes = entry.is_directory()
                                        ? tombstone_generation_bytes(entry.path())
                                        : static_cast<std::uint64_t>(entry.file_size());
        account_bytes(inventory.used, bytes);
        inventory.candidates.push_back({.kind = CandidateKind::Tombstone,
                                        .path = entry.path(),
                                        .digest = {},
                                        .name = {},
                                        .time = entry.last_write_time(),
                                        .bytes = bytes});
    }

    const std::filesystem::path sessions_root = options.root / "sessions";
    for (const auto& session_entry : std::filesystem::directory_iterator(sessions_root)) {
        if (!session_entry.is_directory()) { continue; }
        const std::string digest = session_entry.path().filename().string();
        const std::string active_prefix = digest + "/";
        const bool session_active =
            std::any_of(impl.active_generations.begin(), impl.active_generations.end(),
                        [&](const auto& entry) { return entry.first.starts_with(active_prefix); });
        bool current_readable = true;
        std::optional<std::string> current;
        try {
            current = current_generation(session_entry.path());
        } catch (...) {
            current_readable = false;
        }
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
            const std::string name = entry.path().filename().string();
            const std::uint64_t bytes = directory_bytes(entry.path());
            account_bytes(inventory.used, bytes);
            account_bytes(session_bytes, bytes);
            const bool is_current = current && name == *current;
            if (is_current) { current_time = entry.last_write_time(); }
            if (protected_generation && entry.path() == *protected_generation) { continue; }
            const std::string active_key = digest + "/" + name;
            if (impl.active_generations.contains(active_key)) { continue; }
            if (name.starts_with(".staging-")) {
                inventory.candidates.push_back({.kind = CandidateKind::AbandonedStaging,
                                                .path = entry.path(),
                                                .digest = digest,
                                                .name = name,
                                                .time = entry.last_write_time(),
                                                .bytes = bytes});
            } else if (current_readable && !is_current) {
                inventory.candidates.push_back({.kind = CandidateKind::StaleGeneration,
                                                .path = entry.path(),
                                                .digest = digest,
                                                .name = name,
                                                .time = entry.last_write_time(),
                                                .bytes = bytes});
            }
        }
        if (current_readable && current && current_time && !session_active &&
            (!protected_session || session_entry.path() != *protected_session)) {
            inventory.candidates.push_back({.kind = CandidateKind::CurrentSession,
                                            .path = session_entry.path(),
                                            .digest = digest,
                                            .name = *current,
                                            .time = *current_time,
                                            .bytes = session_bytes});
        }
    }
    std::sort(inventory.candidates.begin(), inventory.candidates.end(),
              [](const GenerationCandidate& left, const GenerationCandidate& right) {
                  if (left.kind != right.kind) { return left.kind < right.kind; }
                  if (left.time != right.time) { return left.time < right.time; }
                  return left.path.generic_string() < right.path.generic_string();
              });
    return inventory;
}

[[nodiscard]] std::filesystem::path unique_tombstone(
    const SessionCheckpointStoreOptions& options, std::string stem) {
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

[[nodiscard]] std::filesystem::path candidate_tombstone(
    const SessionCheckpointStoreOptions& options, const GenerationCandidate& candidate) {
    const char* kind =
        candidate.kind == CandidateKind::CurrentSession ? "--session--" : "--generation--";
    return unique_tombstone(options, candidate.digest + kind + candidate.name);
}

[[nodiscard]] bool cleanup_tombstone(const SessionCheckpointStoreOptions& options,
                                     const std::filesystem::path& path) {
    if (options.tombstone_cleanup) {
        if (!options.tombstone_cleanup(path)) { return false; }
    } else {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (error) { return false; }
    }
    std::error_code error;
    return !std::filesystem::exists(path, error) && !error;
}

void sync_rename_parents(const std::filesystem::path& source,
                         const std::filesystem::path& destination) noexcept {
    try {
        sync_directory(source.parent_path());
        sync_directory(destination.parent_path());
    } catch (...) {}
}

[[nodiscard]] bool enforce_disk_quota_locked(
    const SessionCheckpointStoreOptions& options, const SessionCheckpointStore::Impl& impl,
    const std::optional<std::filesystem::path>& protected_generation = std::nullopt,
    const std::optional<std::filesystem::path>& protected_session = std::nullopt,
    bool reclaim_stale_generations = false) {
    GenerationInventory inventory =
        inventory_generations(options, impl, protected_generation, protected_session);
    for (const GenerationCandidate& candidate : inventory.candidates) {
        const bool unconditional = candidate.kind == CandidateKind::Tombstone ||
                                   candidate.kind == CandidateKind::AbandonedStaging ||
                                   (reclaim_stale_generations &&
                                    candidate.kind == CandidateKind::StaleGeneration);
        if (inventory.used <= options.disk_quota_bytes && !unconditional) { continue; }
        if (candidate.kind == CandidateKind::Tombstone) {
            if (cleanup_tombstone(options, candidate.path)) {
                inventory.used -= candidate.bytes;
            }
            continue;
        }

        std::error_code error;
        const std::uint64_t reclaimed =
            candidate.kind == CandidateKind::CurrentSession
                ? session_generation_bytes(candidate.path)
                : directory_bytes(candidate.path);
        const std::filesystem::path tombstone = candidate_tombstone(options, candidate);
        std::filesystem::rename(candidate.path, tombstone, error);
        if (error) { continue; }
        sync_rename_parents(candidate.path, tombstone);
        if (!cleanup_tombstone(options, tombstone)) { continue; }
        inventory.used -= std::min(inventory.used, reclaimed);
    }
    return inventory.used <= options.disk_quota_bytes;
}

class DirectoryCheckpointReader final : public ContinuationCheckpointReader {
public:
    DirectoryCheckpointReader(std::filesystem::path root,
                              std::map<std::string, std::uint64_t> files,
                              std::shared_ptr<SessionCheckpointStore::Impl> owner,
                              std::string active_key,
                              std::shared_ptr<ContinuationCheckpointReadQueue> read_queue)
        : root_(std::move(root)), files_(std::move(files)), owner_(std::move(owner)),
          active_key_(std::move(active_key)), read_queue_(std::move(read_queue)) {
        // Constructed by SessionCheckpointStore::load while Impl::mutex is held.
        ++owner_->active_generations[active_key_];
    }
    ~DirectoryCheckpointReader() override {
        std::lock_guard lock(owner_->mutex);
        const auto found = owner_->active_generations.find(active_key_);
        if (found == owner_->active_generations.end() || found->second == 0) {
            std::fputs("checkpoint reader ownership invariant failed\n", stderr);
            std::fflush(stderr);
            std::terminate();
        }
        if (--found->second == 0) { owner_->active_generations.erase(found); }
    }

    std::optional<std::uint64_t> file_size(std::string_view path) const override {
        const auto found = files_.find(std::string(path));
        return found == files_.end() ? std::nullopt
                                     : std::optional<std::uint64_t>(found->second);
    }

    bool read_file(std::string_view path, std::uint64_t offset,
                   std::span<std::byte> destination) const override {
        const auto found = files_.find(std::string(path));
        if (found == files_.end() || offset > found->second ||
            destination.size() > found->second - offset) {
            return false;
        }
        if (destination.empty()) { return true; }
#ifdef _WIN32
        if (!read_queue_ || !read_queue_->available()) { return false; }
        try {
            // One batch per reader call: the engine already coalesces page segments into
            // staging-sized windows, and the queue takes many requests per submit. Bound each
            // request to the DirectStorage staging buffer configured for the queue.
            const std::vector<ContinuationCheckpointReadRequest> requests =
                runtime::split_continuation_checkpoint_read(offset, destination,
                                                            kDirectStorageRequestBytes);
            std::unique_ptr<ContinuationCheckpointReadCompletion> completion =
                read_queue_->submit(root_ / found->first, requests);
            completion->wait();
            return true;
        } catch (...) {
            return false;
        }
#else
        std::FILE* file = open_binary_read(root_ / found->first);
        if (file == nullptr) { return false; }
#ifdef _WIN32
        const bool sought = _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
        const bool sought = ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
        const std::size_t count = sought && !destination.empty()
                                      ? std::fread(destination.data(), 1, destination.size(), file)
                                      : 0;
        const bool ok = sought && (destination.empty() || count == destination.size()) &&
                        std::ferror(file) == 0;
        std::fclose(file);
        return ok;
#endif
    }

private:
    std::filesystem::path root_;
    std::map<std::string, std::uint64_t> files_;
    std::shared_ptr<SessionCheckpointStore::Impl> owner_;
    std::string active_key_;
    std::shared_ptr<ContinuationCheckpointReadQueue> read_queue_;
};

void quarantine_generation(const std::filesystem::path& session, const std::string& generation) {
    std::error_code error;
    const std::filesystem::path source = session / "generations" / generation;
    const std::filesystem::path target =
        session / "generations" /
        (generation + ".corrupt-" + std::to_string(unix_milliseconds()));
    std::filesystem::rename(source, target, error);
    error.clear();
    std::filesystem::remove(session / "current", error);
    try {
        sync_directory(session);
    } catch (...) {}
}

} // namespace

std::vector<std::byte> encode_response_store_snapshot(const ResponseStoreSnapshot& snapshot,
                                                      std::size_t byte_limit) {
    if (byte_limit == 0 || snapshot.client_session_sha256.empty() ||
        snapshot.latest_response_id.empty() || snapshot.records.empty()) {
        throw std::invalid_argument("response checkpoint snapshot is empty");
    }
    std::unordered_map<const ResponseContextNode*, std::uint64_t> context_ids;
    std::vector<ResponseContext> contexts;
    const auto add_context = [&](const auto& self, const ResponseContext& context) -> std::uint64_t {
        if (!context) { return 0; }
        const auto found = context_ids.find(context.get());
        if (found != context_ids.end()) { return found->second; }
        (void)self(self, context->parent);
        const std::uint64_t id = static_cast<std::uint64_t>(contexts.size()) + 1;
        context_ids.emplace(context.get(), id);
        contexts.push_back(context);
        return id;
    };
    for (const StoredResponse& response : snapshot.records) { (void)add_context(add_context, response.context); }

    nlohmann::json encoded_contexts = nlohmann::json::array();
    for (const ResponseContext& context : contexts) {
        nlohmann::json turns = nlohmann::json::array();
        for (const ChatTurn& turn : context->turns) { turns.push_back(turn_to_json(turn)); }
        encoded_contexts.push_back(
            {{"id", context_ids.at(context.get())},
             {"parent", context->parent ? context_ids.at(context->parent.get()) : 0},
             {"turns", std::move(turns)}});
    }

    nlohmann::json records = nlohmann::json::array();
    for (const StoredResponse& response : snapshot.records) {
        records.push_back(
            {{"id", response.id},
             {"session_key", response.session_key},
             {"client_session_sha256",
              response.client_session_sha256 ? nlohmann::json(*response.client_session_sha256)
                                             : nlohmann::json(nullptr)},
             {"previous_response_id",
              response.previous_response_id ? nlohmann::json(*response.previous_response_id)
                                            : nlohmann::json(nullptr)},
             {"response", response.response},
             {"input_items", response.input_items},
             {"context", response.context ? context_ids.at(response.context.get()) : 0},
             {"preserve_thinking", response.preserve_thinking}});
    }
    const nlohmann::json root = {{"schema_version", kSessionCheckpointSchemaVersion},
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

std::optional<ResponseStoreSnapshot>
decode_response_store_snapshot(std::span<const std::byte> bytes, std::size_t byte_limit) {
    if (bytes.empty() || bytes.size() > byte_limit) { return std::nullopt; }
    try {
        std::vector<std::uint8_t> cbor(bytes.size());
        std::transform(bytes.begin(), bytes.end(), cbor.begin(),
                       [](std::byte value) { return std::to_integer<std::uint8_t>(value); });
        const nlohmann::json root = nlohmann::json::from_cbor(cbor, true, true);
        if (!root.is_object() || root.at("schema_version").get<std::uint32_t>() !=
                                     kSessionCheckpointSchemaVersion ||
            !root.at("contexts").is_array() || !root.at("records").is_array()) {
            return std::nullopt;
        }
        ResponseStoreSnapshot snapshot;
        snapshot.client_session_sha256 = root.at("client_session_sha256").get<std::string>();
        snapshot.latest_response_id    = root.at("latest_response_id").get<std::string>();
        if (snapshot.client_session_sha256.empty() || snapshot.latest_response_id.empty() ||
            root.at("records").empty()) {
            return std::nullopt;
        }
        std::vector<ResponseContext> contexts(root.at("contexts").size() + 1);
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
            contexts[expected_id] = append_response_context(contexts[parent], std::move(turns));
            ++expected_id;
        }
        std::unordered_set<std::string> ids;
        bool has_latest = false;
        snapshot.records.reserve(root.at("records").size());
        for (const nlohmann::json& encoded : root.at("records")) {
            StoredResponse response;
            response.id                    = encoded.at("id").get<std::string>();
            response.session_key           = encoded.at("session_key").get<std::string>();
            if (!encoded.at("client_session_sha256").is_null()) {
                response.client_session_sha256 =
                    encoded.at("client_session_sha256").get<std::string>();
            }
            if (!encoded.at("previous_response_id").is_null()) {
                response.previous_response_id =
                    encoded.at("previous_response_id").get<std::string>();
            }
            response.response     = encoded.at("response");
            response.input_items  = encoded.at("input_items").get<std::vector<nlohmann::json>>();
            response.preserve_thinking = encoded.at("preserve_thinking").get<bool>();
            const std::size_t context = encoded.at("context").get<std::size_t>();
            if (context >= contexts.size() || response.id.empty() ||
                response.session_key.empty() || !response.response.is_object() ||
                !response.client_session_sha256 ||
                *response.client_session_sha256 != snapshot.client_session_sha256 ||
                !ids.insert(response.id).second) {
                return std::nullopt;
            }
            response.context = contexts[context];
            has_latest       = has_latest || response.id == snapshot.latest_response_id;
            snapshot.records.push_back(std::move(response));
        }
        return has_latest ? std::optional<ResponseStoreSnapshot>(std::move(snapshot))
                          : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

SessionCheckpointStore::SessionCheckpointStore(SessionCheckpointStoreOptions options)
    : options_(std::move(options)), impl_(std::make_shared<Impl>()) {
    if (options_.root.empty() || options_.disk_quota_bytes == 0 || options_.staging_bytes == 0 ||
        options_.io_timeout_ms == 0) {
        throw std::invalid_argument("checkpoint store options must be positive");
    }
#if defined(_WIN32)
    if (!options_.read_queue || !options_.read_queue->available()) {
        throw std::runtime_error("DirectStorage checkpoint read queue is unavailable");
    }
#endif
    std::filesystem::create_directories(options_.root / "sessions");
    std::filesystem::create_directories(options_.root / ".tombstones");
    collect_garbage();
}

std::filesystem::path SessionCheckpointStore::session_path(
    const AuthenticatedCheckpointNamespace& checkpoint_namespace) const {
    return options_.root / "sessions" / namespace_storage_digest(checkpoint_namespace);
}

std::optional<SessionCheckpointSaveResult>
SessionCheckpointStore::save(const AuthenticatedCheckpointNamespace& checkpoint_namespace,
                             const ResponseStoreSnapshot& responses,
                             const nlohmann::json& runtime_fingerprint,
                             const EngineExporter& exporter,
                             bool reclaim_superseded_generation) {
    if (responses.client_session_sha256 != checkpoint_namespace.session_sha256() ||
        !runtime_fingerprint.is_object() ||
        !exporter) {
        return std::nullopt;
    }
    std::lock_guard lock(impl_->mutex);
    const std::filesystem::path session = session_path(checkpoint_namespace);
    const std::filesystem::path generations = session / "generations";
    std::filesystem::create_directories(generations);
    const std::uint64_t ordinal = impl_->sequence.fetch_add(1, std::memory_order_relaxed);
    const std::string generation = std::to_string(unix_milliseconds()) + "-" +
                                   std::to_string(ordinal);
    const std::filesystem::path staging = generations / (".staging-" + generation);
    const std::filesystem::path published = generations / generation;
    std::error_code cleanup_error;
    std::filesystem::remove_all(staging, cleanup_error);
    std::filesystem::create_directories(staging);

    try {
        const std::vector<std::byte> response_bytes =
            encode_response_store_snapshot(responses, options_.staging_bytes);
        DirectoryCheckpointWriter writer(staging, options_.staging_bytes);
        if (!writer.write_store_file("responses.cbor", 0, response_bytes.size(), response_bytes)) {
            throw std::runtime_error("response checkpoint write failed");
        }
        // The exporter holds the Engine execution lock: buffer its writes so the engine pays
        // staging cost only, and fail the save if any deferred disk write failed (ninfer#34).
        BufferedCheckpointWriter buffered(writer, options_.write_buffer_bytes);
        std::optional<ContinuationCheckpointStats> engine = exporter(buffered);
        if (!buffered.finish()) {
            engine.reset();
            throw std::runtime_error("checkpoint payload write failed");
        }
        if (!engine || !valid_checkpoint_stats(*engine)) {
            std::filesystem::remove_all(staging, cleanup_error);
            return std::nullopt;
        }
        std::optional<std::vector<FileDescriptor>> payloads = writer.files();
        if (!payloads) { throw std::runtime_error("checkpoint payload set is incomplete"); }

        nlohmann::json checksum_body = nlohmann::json::array();
        for (const FileDescriptor& file : *payloads) { checksum_body.push_back(descriptor_json(file)); }
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
        nlohmann::json manifest_files = nlohmann::json::array();
        for (const FileDescriptor& file : *payloads) {
            if (file.bytes > std::numeric_limits<std::uint64_t>::max() - payload_bytes) {
                throw std::overflow_error("checkpoint payload byte count overflowed");
            }
            payload_bytes += file.bytes;
            if (file.path.starts_with("engine/")) {
                if (file.bytes >
                    std::numeric_limits<std::uint64_t>::max() - engine_payload_bytes) {
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
            {"tenant_sha256", checkpoint_namespace.tenant_sha256()},
            {"session_sha256", checkpoint_namespace.session_sha256()},
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
        const std::string manifest_text = manifest.dump();
        write_synced(staging / "manifest.json", manifest_text);
        if (!options_.origin_mac_key.empty()) {
            write_synced(staging / "manifest.mac",
                         manifest_origin_mac_hex(options_.origin_mac_key, manifest_text));
        }
        sync_directory(staging);
        // Reclaim before changing current. The staging generation and its session are protected,
        // so quota or cleanup failure leaves the previously published generation untouched.
        const std::uint64_t total_bytes = directory_bytes(staging);
        if (total_bytes > options_.disk_quota_bytes ||
            !enforce_disk_quota_locked(options_, *impl_, staging, session)) {
            std::filesystem::remove_all(staging, cleanup_error);
            return std::nullopt;
        }

        std::filesystem::rename(staging, published);
        sync_directory(generations);
        const std::filesystem::path current_staging =
            session / (".current-" + generation);
        write_synced(current_staging, generation + "\n");
        replace_path(current_staging, session / "current");
        try {
            if (options_.current_pointer_sync) {
                options_.current_pointer_sync(session);
            } else {
                sync_directory(session);
            }
        } catch (...) {
            // The current pointer already names the complete post-save generation. A parent
            // directory sync failure may weaken crash durability, but it cannot turn this
            // committed in-process state transition into a reported rejection.
        }
        try {
            // A successful logical delete must not leave the superseded response body in an
            // inactive generation merely because the store remains below quota. Active readers
            // stay protected; their generation can remain until a later DELETE, quota pass, or
            // whole-session removal, which is part of the documented logical-delete boundary.
            if (reclaim_superseded_generation) {
                (void)enforce_disk_quota_locked(options_, *impl_, published, session, true);
            }
        } catch (...) {}

        return SessionCheckpointSaveResult{
            .generation = generation, .engine = *engine, .bytes = total_bytes};
    } catch (...) {
        std::filesystem::remove_all(staging, cleanup_error);
        throw;
    }
}

SessionCheckpointLoadResult
SessionCheckpointStore::load(const AuthenticatedCheckpointNamespace& checkpoint_namespace,
                             const nlohmann::json& runtime_fingerprint,
                             std::optional<std::string_view> required_response_id) {
    if (!runtime_fingerprint.is_object()) {
        return load_state(SessionCheckpointLoadState::Missing);
    }
    std::lock_guard lock(impl_->mutex);
    const std::filesystem::path session = session_path(checkpoint_namespace);
    std::optional<std::string> generation;
    try {
        generation = current_generation(session);
    } catch (const CheckpointCorruption&) {
        return load_state(SessionCheckpointLoadState::Corrupt);
    } catch (...) {
        return load_state(SessionCheckpointLoadState::Unavailable);
    }
    if (!generation) { return load_state(SessionCheckpointLoadState::Missing); }
    const std::filesystem::path root = session / "generations" / *generation;
    try {
        const std::string manifest_text = read_text_bounded(root / "manifest.json", 16ULL << 20);
        // Verify origin before trusting any manifest content (alphastorm/ninfer#32). A
        // present-but-wrong MAC is tamper evidence and quarantines like corruption. An
        // absent MAC under the strict import posture is a REVERSIBLE policy refusal, not
        // evidence: report Incompatible (clean-replay path) and mutate nothing, so
        // clearing the flag restores the generation. Off-strict, locally-produced legacy
        // generations keep loading.
        switch (manifest_origin_state(root, manifest_text, options_.origin_mac_key)) {
        case ManifestOriginState::Authentic: break;
        case ManifestOriginState::Forged:
            throw CheckpointCorruption("checkpoint manifest origin authentication failed");
        case ManifestOriginState::Unauthenticated:
            if (options_.require_origin_auth) {
                return load_state(SessionCheckpointLoadState::Incompatible);
            }
            break;
        }
        const nlohmann::json manifest = nlohmann::json::parse(manifest_text);
        if (!manifest.is_object() ||
            manifest.at("artifact_type").get<std::string>() != "ninfer_session_checkpoint" ||
            manifest.at("schema_version").get<std::uint32_t>() !=
                kSessionCheckpointSchemaVersion ||
            manifest.at("tenant_sha256").get<std::string>() !=
                checkpoint_namespace.tenant_sha256() ||
            manifest.at("session_sha256").get<std::string>() !=
                checkpoint_namespace.session_sha256() ||
            manifest.at("generation").get<std::string>() != *generation) {
            throw CheckpointCorruption("checkpoint manifest identity is corrupt");
        }
        if (manifest.at("runtime_fingerprint") != runtime_fingerprint) {
            return load_state(SessionCheckpointLoadState::Incompatible);
        }
        if (!manifest.at("files").is_array() || manifest.at("files").empty() ||
            manifest.at("files").size() > 4096) {
            throw CheckpointCorruption("checkpoint manifest file set is corrupt");
        }
        std::map<std::string, std::uint64_t> allowed;
        std::uint64_t verified_bytes        = 0;
        std::uint64_t verified_engine_bytes = 0;
        for (const nlohmann::json& encoded : manifest.at("files")) {
            std::optional<FileDescriptor> expected = descriptor_from_json(encoded);
            if (!expected || !allowed.emplace(expected->path, expected->bytes).second) {
                throw CheckpointCorruption("checkpoint manifest file descriptor is corrupt");
            }
            const FileDescriptor actual = hash_file(root, expected->path, expected->bytes);
            if (actual.sha256 != expected->sha256) {
                throw CheckpointCorruption("checkpoint payload checksum mismatch");
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
            manifest.at("engine_payload_bytes").get<std::uint64_t>() !=
                verified_engine_bytes) {
            throw CheckpointCorruption("checkpoint manifest payload summary is corrupt");
        }
        const auto response_file = allowed.find("responses.cbor");
        if (response_file == allowed.end() || response_file->second > options_.staging_bytes) {
            throw CheckpointCorruption("checkpoint response payload descriptor is corrupt");
        }
        const std::vector<std::byte> response_bytes =
            read_bounded(root / "responses.cbor", options_.staging_bytes);
        std::optional<ResponseStoreSnapshot> responses =
            decode_response_store_snapshot(response_bytes, options_.staging_bytes);
        if (!responses ||
            responses->client_session_sha256 != checkpoint_namespace.session_sha256() ||
            responses->latest_response_id !=
                manifest.at("latest_response_id").get<std::string>() ||
            responses->records.size() != manifest.at("response_records").get<std::size_t>()) {
            throw CheckpointCorruption("checkpoint response state is corrupt");
        }
        if (required_response_id &&
            std::none_of(responses->records.begin(), responses->records.end(),
                         [&](const StoredResponse& response) {
                             return response.id == *required_response_id;
                         })) {
            return load_state(SessionCheckpointLoadState::Missing);
        }
        ContinuationCheckpointStats expected{
            .frontier_tokens = manifest.at("frontier_tokens").get<std::uint32_t>(),
            .restored_tokens = manifest.at("restored_tokens").get<std::uint32_t>(),
            .payload_bytes = manifest.at("engine_payload_bytes").get<std::uint64_t>(),
        };
        if (!valid_checkpoint_stats(expected)) {
            throw CheckpointCorruption("checkpoint engine summary is corrupt");
        }
        const std::uint64_t generation_bytes = options_.generation_size
                                                   ? options_.generation_size(root)
                                                   : directory_bytes(root);
        ResponseStoreSnapshot published_responses = std::move(*responses);
        std::string published_generation           = *generation;
        const std::string active_key =
            session.filename().string() + "/" + published_generation;
        auto reader = std::make_shared<DirectoryCheckpointReader>(
            root, std::move(allowed), impl_, active_key, options_.read_queue);
        return {.state = SessionCheckpointLoadState::Available,
                .checkpoint = VerifiedSessionCheckpoint{.responses =
                                                            std::move(published_responses),
                                                        .engine = std::move(reader),
                                                        .expected_engine = expected,
                                                        .generation =
                                                            std::move(published_generation),
                                                        .bytes = generation_bytes}};
    } catch (const CheckpointCorruption&) {
        quarantine_generation(session, *generation);
        return load_state(SessionCheckpointLoadState::Corrupt);
    } catch (const nlohmann::json::exception&) {
        quarantine_generation(session, *generation);
        return load_state(SessionCheckpointLoadState::Corrupt);
    } catch (...) {
        return load_state(SessionCheckpointLoadState::Unavailable);
    }
}

bool SessionCheckpointStore::covers(
    const AuthenticatedCheckpointNamespace& checkpoint_namespace,
    const nlohmann::json& runtime_fingerprint, std::string_view response_id) const {
    if (!runtime_fingerprint.is_object() || response_id.empty()) { return false; }
    std::lock_guard lock(impl_->mutex);
    try {
        const std::filesystem::path session         = session_path(checkpoint_namespace);
        const std::optional<std::string> generation = current_generation(session);
        if (!generation) { return false; }
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
        const nlohmann::json manifest = nlohmann::json::parse(manifest_text);
        return manifest.is_object() &&
               manifest.at("artifact_type").get<std::string>() == "ninfer_session_checkpoint" &&
               manifest.at("schema_version").get<std::uint32_t>() ==
                   kSessionCheckpointSchemaVersion &&
               manifest.at("tenant_sha256").get<std::string>() ==
                   checkpoint_namespace.tenant_sha256() &&
               manifest.at("session_sha256").get<std::string>() ==
                   checkpoint_namespace.session_sha256() &&
               manifest.at("generation").get<std::string>() == *generation &&
               manifest.at("runtime_fingerprint") == runtime_fingerprint &&
               manifest.at("latest_response_id").get<std::string>() == response_id;
    } catch (...) { return false; }
}

nlohmann::json SessionCheckpointStore::status(
    const AuthenticatedCheckpointNamespace& checkpoint_namespace,
    const nlohmann::json& runtime_fingerprint) const {
    nlohmann::json result = {{"artifact_type", "ninfer_session_checkpoint_status"},
                             {"schema_version", kSessionCheckpointSchemaVersion},
                             {"state", "missing"}};
    if (!runtime_fingerprint.is_object()) { return result; }
    std::lock_guard lock(impl_->mutex);
    try {
        const std::filesystem::path session = session_path(checkpoint_namespace);
        const std::optional<std::string> generation = current_generation(session);
        if (!generation) { return result; }
        const std::filesystem::path root = session / "generations" / *generation;
        const std::string manifest_text = read_text_bounded(root / "manifest.json", 16ULL << 20);
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
        const nlohmann::json manifest = nlohmann::json::parse(manifest_text);
        if (!manifest.is_object() ||
            manifest.at("artifact_type").get<std::string>() != "ninfer_session_checkpoint" ||
            manifest.at("schema_version").get<std::uint32_t>() !=
                kSessionCheckpointSchemaVersion ||
            manifest.at("tenant_sha256").get<std::string>() !=
                checkpoint_namespace.tenant_sha256() ||
            manifest.at("session_sha256").get<std::string>() !=
                checkpoint_namespace.session_sha256() ||
            manifest.at("generation").get<std::string>() != *generation) {
            throw CheckpointCorruption("checkpoint manifest identity is corrupt");
        }
        result["state"] = manifest.at("runtime_fingerprint") == runtime_fingerprint
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
        result["state"] = "unavailable";
        return result;
    }
}

SessionCheckpointEraseResult SessionCheckpointStore::erase(
    const AuthenticatedCheckpointNamespace& checkpoint_namespace) {
    std::lock_guard lock(impl_->mutex);
    const std::filesystem::path session = session_path(checkpoint_namespace);
    const std::string storage_digest = session.filename().string();
    const std::string prefix = storage_digest + "/";
    if (std::any_of(impl_->active_generations.begin(), impl_->active_generations.end(),
                    [&](const auto& entry) { return entry.first.starts_with(prefix); })) {
        return SessionCheckpointEraseResult::Conflict;
    }
    std::filesystem::path tombstone;
    try {
        tombstone = unique_tombstone(options_, storage_digest + "--session");
    } catch (...) { return SessionCheckpointEraseResult::Conflict; }
    std::error_code error;
    std::filesystem::rename(session, tombstone, error);
    if (error) {
        std::error_code status_error;
        const bool source_exists = std::filesystem::exists(session, status_error);
        if (error == std::errc::no_such_file_or_directory ||
            (!status_error && !source_exists)) {
            return SessionCheckpointEraseResult::Missing;
        }
        return SessionCheckpointEraseResult::Conflict;
    }
    sync_rename_parents(session, tombstone);
    (void)cleanup_tombstone(options_, tombstone);
    return SessionCheckpointEraseResult::Erased;
}

void SessionCheckpointStore::collect_garbage() {
    std::lock_guard lock(impl_->mutex);
    (void)enforce_disk_quota_locked(options_, *impl_);
}

SessionCheckpointManager::SessionCheckpointManager(SessionCheckpointStoreOptions options,
                                                   nlohmann::json runtime_fingerprint,
                                                   std::string tenant_sha256,
                                                   SessionCheckpointEngine engine)
    : runtime_fingerprint_(std::move(runtime_fingerprint)),
      tenant_sha256_(std::move(tenant_sha256)), engine_(std::move(engine)) {
    if (!runtime_fingerprint_.is_object() ||
        !AuthenticatedCheckpointNamespace::valid_sha256(tenant_sha256_) || !engine_.checkpoint ||
        !engine_.restore) {
        throw std::invalid_argument("checkpoint manager identity and Engine access must be valid");
    }
    store_ = std::make_unique<SessionCheckpointStore>(std::move(options));
}

SessionCheckpointSaveOutcome SessionCheckpointManager::save(
    std::string_view session_sha256, std::string_view required_response_id,
    ResponseStore& responses) {
    if (!store_) {
        return {.state = SessionCheckpointSaveState::Disabled, .checkpoint = std::nullopt};
    }
    if (!AuthenticatedCheckpointNamespace::valid_sha256(session_sha256) ||
        required_response_id.empty()) {
        return {.state = SessionCheckpointSaveState::Failed, .checkpoint = std::nullopt};
    }
    std::lock_guard lock(mutex_);
    try {
        std::optional<ResponseStoreSnapshot> snapshot = responses.snapshot_session(session_sha256);
        if (!snapshot) {
            return {.state = SessionCheckpointSaveState::Missing, .checkpoint = std::nullopt};
        }
        if (snapshot->latest_response_id != required_response_id) {
            return {.state = SessionCheckpointSaveState::Missing, .checkpoint = std::nullopt};
        }
        const auto latest = std::find_if(
            snapshot->records.begin(), snapshot->records.end(), [&](const StoredResponse& response) {
                return response.id == snapshot->latest_response_id;
            });
        if (latest == snapshot->records.end() || latest->session_key.empty() ||
            latest->session_key.size() > kMaximumContextCacheSessionKeyBytes) {
            return {.state = SessionCheckpointSaveState::Failed, .checkpoint = std::nullopt};
        }
        const AuthenticatedCheckpointNamespace checkpoint_namespace =
            AuthenticatedCheckpointNamespace::authenticated(tenant_sha256_,
                                                            std::string(session_sha256));
        const std::string checkpoint_tag = latest->session_key;
        std::optional<SessionCheckpointSaveResult> saved = store_->save(
            checkpoint_namespace, *snapshot, runtime_fingerprint_,
            [&](ContinuationCheckpointWriter& writer) {
                return engine_.checkpoint(checkpoint_namespace, checkpoint_tag, writer,
                                          store_->options().staging_bytes);
            });
        if (!saved) {
            return {.state = SessionCheckpointSaveState::Failed, .checkpoint = std::nullopt};
        }
        return {.state = SessionCheckpointSaveState::Saved, .checkpoint = std::move(saved)};
    } catch (...) {
        return {.state = SessionCheckpointSaveState::Unavailable, .checkpoint = std::nullopt};
    }
}

SessionCheckpointRestoreState
SessionCheckpointManager::restore(std::string_view session_sha256,
                                  std::string_view required_response_id,
                                  ResponseStore& responses) {
    if (!store_) { return SessionCheckpointRestoreState::Disabled; }
    if (!AuthenticatedCheckpointNamespace::valid_sha256(session_sha256) ||
        required_response_id.empty()) {
        return SessionCheckpointRestoreState::Missing;
    }
    std::lock_guard lock(mutex_);
    return restore_locked(session_sha256, required_response_id, responses);
}

SessionCheckpointRestoreState
SessionCheckpointManager::restore_locked(std::string_view session_sha256,
                                         std::string_view required_response_id,
                                         ResponseStore& responses) {
    try {
        const AuthenticatedCheckpointNamespace checkpoint_namespace =
            AuthenticatedCheckpointNamespace::authenticated(tenant_sha256_,
                                                            std::string(session_sha256));
        SessionCheckpointLoadResult loaded =
            store_->load(checkpoint_namespace, runtime_fingerprint_, required_response_id);
        switch (loaded.state) {
        case SessionCheckpointLoadState::Missing:
            return SessionCheckpointRestoreState::Missing;
        case SessionCheckpointLoadState::Incompatible:
            return SessionCheckpointRestoreState::Incompatible;
        case SessionCheckpointLoadState::Corrupt:
            return SessionCheckpointRestoreState::Corrupt;
        case SessionCheckpointLoadState::Unavailable:
            return SessionCheckpointRestoreState::Unavailable;
        case SessionCheckpointLoadState::Available:
            break;
        }
        if (!loaded.checkpoint) { return SessionCheckpointRestoreState::Failed; }
        VerifiedSessionCheckpoint& checkpoint = *loaded.checkpoint;
        const auto latest = std::find_if(
            checkpoint.responses.records.begin(), checkpoint.responses.records.end(),
            [&](const StoredResponse& response) {
                return response.id == checkpoint.responses.latest_response_id;
            });
        if (latest == checkpoint.responses.records.end() || latest->session_key.empty() ||
            latest->session_key.size() > kMaximumContextCacheSessionKeyBytes) {
            return SessionCheckpointRestoreState::Failed;
        }
        const std::string checkpoint_tag = latest->session_key;
        const bool committed = responses.restore_session(std::move(checkpoint.responses), [&] {
            const std::optional<ContinuationCheckpointStats> restored = engine_.restore(
                checkpoint_namespace, checkpoint_tag, *checkpoint.engine,
                checkpoint.expected_engine, store_->options().staging_bytes);
            return restored && *restored == checkpoint.expected_engine;
        });
        return committed ? SessionCheckpointRestoreState::Restored
                         : SessionCheckpointRestoreState::Failed;
    } catch (...) {
        return SessionCheckpointRestoreState::Unavailable;
    }
}

nlohmann::json SessionCheckpointManager::status(std::string_view session_sha256) {
    if (!store_) {
        return nlohmann::json{{"artifact_type", "ninfer_session_checkpoint_status"},
                              {"state", "disabled"}};
    }
    if (!AuthenticatedCheckpointNamespace::valid_sha256(session_sha256)) {
        return nlohmann::json{{"artifact_type", "ninfer_session_checkpoint_status"},
                              {"state", "missing"}};
    }
    std::lock_guard lock(mutex_);
    try {
        const AuthenticatedCheckpointNamespace checkpoint_namespace =
            AuthenticatedCheckpointNamespace::authenticated(tenant_sha256_,
                                                            std::string(session_sha256));
        return store_->status(checkpoint_namespace, runtime_fingerprint_);
    } catch (...) {
        return nlohmann::json{{"artifact_type", "ninfer_session_checkpoint_status"},
                              {"state", "unavailable"}};
    }
}

bool SessionCheckpointManager::covers(std::string_view session_sha256,
                                      std::string_view response_id) {
    if (!store_) { return false; }
    if (!AuthenticatedCheckpointNamespace::valid_sha256(session_sha256)) { return false; }
    std::lock_guard lock(mutex_);
    try {
        const AuthenticatedCheckpointNamespace checkpoint_namespace =
            AuthenticatedCheckpointNamespace::authenticated(tenant_sha256_,
                                                            std::string(session_sha256));
        return store_->covers(checkpoint_namespace, runtime_fingerprint_, response_id);
    } catch (...) { return false; }
}

SessionCheckpointEraseResult SessionCheckpointManager::erase(
    std::string_view session_sha256) {
    if (!store_) { return SessionCheckpointEraseResult::Missing; }
    if (!AuthenticatedCheckpointNamespace::valid_sha256(session_sha256)) {
        return SessionCheckpointEraseResult::Conflict;
    }
    std::lock_guard lock(mutex_);
    try {
        const AuthenticatedCheckpointNamespace checkpoint_namespace =
            AuthenticatedCheckpointNamespace::authenticated(tenant_sha256_,
                                                            std::string(session_sha256));
        return store_->erase(checkpoint_namespace);
    } catch (...) {
        return SessionCheckpointEraseResult::Conflict;
    }
}

SessionCheckpointEraseResult SessionCheckpointManager::erase_response(
    std::string_view session_sha256, std::string_view response_id, ResponseStore& responses) {
    if (!store_) { return SessionCheckpointEraseResult::Missing; }
    if (!AuthenticatedCheckpointNamespace::valid_sha256(session_sha256) || response_id.empty()) {
        return SessionCheckpointEraseResult::Conflict;
    }
    std::lock_guard lock(mutex_);
    try {
        const AuthenticatedCheckpointNamespace checkpoint_namespace =
            AuthenticatedCheckpointNamespace::authenticated(tenant_sha256_,
                                                            std::string(session_sha256));
        const auto erase_transaction = [&] {
            return responses.erase_for_session_transactionally(
                std::string(response_id), session_sha256,
                [&](const std::optional<ResponseStoreSnapshot>& post_delete) {
                    // Keep the prior durable generation intact on every failed publication. The
                    // live session is pinned against cross-session LRU eviction until this callback
                    // returns and the no-allocation in-memory erase commits.
                    if (!post_delete) {
                        return store_->erase(checkpoint_namespace) !=
                               SessionCheckpointEraseResult::Conflict;
                    }
                    const StoredResponse& latest = post_delete->records.back();
                    if (latest.session_key.empty() ||
                        latest.session_key.size() > kMaximumContextCacheSessionKeyBytes) {
                        return false;
                    }
                    const std::string checkpoint_tag = latest.session_key;
                    return store_->save(
                               checkpoint_namespace, *post_delete, runtime_fingerprint_,
                               [&](ContinuationCheckpointWriter& writer) {
                                   return engine_.checkpoint(
                                       checkpoint_namespace, checkpoint_tag, writer,
                                       store_->options().staging_bytes);
                               }, true)
                        .has_value();
                });
        };
        ResponseStoreTransactionState transaction = erase_transaction();
        if (transaction == ResponseStoreTransactionState::Missing) {
            // Global LRU eviction is a live-cache event, not durable deletion. Restore the
            // authenticated checkpoint while the per-session caller lease and manager mutex are
            // held, then execute the same pinned live+durable delete transaction. This prevents a
            // 404 followed by response resurrection after restart.
            const SessionCheckpointRestoreState restored =
                restore_locked(session_sha256, response_id, responses);
            if (restored == SessionCheckpointRestoreState::Missing) {
                return SessionCheckpointEraseResult::Missing;
            }
            if (restored != SessionCheckpointRestoreState::Restored) {
                return SessionCheckpointEraseResult::Conflict;
            }
            transaction = erase_transaction();
        }
        switch (transaction) {
        case ResponseStoreTransactionState::Committed:
            return SessionCheckpointEraseResult::Erased;
        case ResponseStoreTransactionState::Missing:
            return SessionCheckpointEraseResult::Missing;
        case ResponseStoreTransactionState::Conflict:
            return SessionCheckpointEraseResult::Conflict;
        }
    } catch (...) {
        return SessionCheckpointEraseResult::Conflict;
    }
    return SessionCheckpointEraseResult::Conflict;
}

} // namespace ninfer::serve
