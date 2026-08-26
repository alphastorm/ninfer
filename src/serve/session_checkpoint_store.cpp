#include "serve/session_checkpoint_store.h"

#include "core/sha256.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
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

struct FileDescriptor {
    std::string path;
    std::uint64_t bytes = 0;
    std::string sha256;
};

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
    if (error || size > limit || size > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("checkpoint file size is invalid");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::FILE* file = open_binary_read(path);
    if (file == nullptr) { throw std::runtime_error("checkpoint file open failed"); }
    const std::size_t read =
        bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
    const bool failed = read != bytes.size() || std::ferror(file) != 0;
    std::fclose(file);
    if (failed) { throw std::runtime_error("checkpoint file read failed"); }
    return bytes;
}

[[nodiscard]] std::string read_text_bounded(const std::filesystem::path& path, std::size_t limit) {
    const std::vector<std::byte> bytes = read_bounded(path, limit);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] FileDescriptor hash_file(const std::filesystem::path& root,
                                       const std::string& relative,
                                       std::uint64_t expected_bytes) {
    const std::filesystem::path path = root / relative;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) != expected_bytes || error) {
        throw std::runtime_error("checkpoint payload size mismatch");
    }
    std::FILE* file = open_binary_read(path);
    if (file == nullptr) { throw std::runtime_error("checkpoint payload open failed"); }
    crypto::Sha256 hasher;
    std::array<std::byte, 64ULL << 10> buffer{};
    std::uint64_t consumed = 0;
    try {
        while (consumed < expected_bytes) {
            const std::size_t requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), expected_bytes - consumed));
            const std::size_t count = std::fread(buffer.data(), 1, requested, file);
            if (count != requested) { throw std::runtime_error("checkpoint payload read failed"); }
            hasher.update(std::span<const std::byte>(buffer.data(), count));
            consumed += count;
        }
        if (std::fgetc(file) != EOF) { throw std::runtime_error("checkpoint payload grew"); }
        std::fclose(file);
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
    return {{"role", static_cast<std::uint8_t>(turn.role)},
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
    const int role = value.at("role").get<int>();
    if (role < static_cast<int>(ChatRole::System) || role > static_cast<int>(ChatRole::Tool)) {
        return std::nullopt;
    }
    ChatTurn turn;
    turn.role              = static_cast<ChatRole>(role);
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
    if (!std::filesystem::is_regular_file(path, error) || error) { return std::nullopt; }
    std::string value = read_text_bounded(path, 256);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) { value.pop_back(); }
    if (value.empty() || !valid_relative_path(value) || value.find('/') != std::string::npos ||
        value.starts_with('.')) {
        throw std::runtime_error("checkpoint current pointer is invalid");
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

class DirectoryCheckpointReader final : public ContinuationCheckpointReader {
public:
    DirectoryCheckpointReader(std::filesystem::path root,
                              std::map<std::string, std::uint64_t> files,
                              std::shared_ptr<SessionCheckpointStore::Impl> owner,
                              std::string active_key)
        : root_(std::move(root)), files_(std::move(files)), owner_(std::move(owner)),
          active_key_(std::move(active_key)) {
        // Constructed by SessionCheckpointStore::load while Impl::mutex is held.
        ++owner_->active_generations[active_key_];
    }
    ~DirectoryCheckpointReader() override {
        std::lock_guard lock(owner_->mutex);
        const auto found = owner_->active_generations.find(active_key_);
        if (found == owner_->active_generations.end() || found->second == 0) { std::terminate(); }
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
    }

private:
    std::filesystem::path root_;
    std::map<std::string, std::uint64_t> files_;
    std::shared_ptr<SessionCheckpointStore::Impl> owner_;
    std::string active_key_;
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
    if (options_.root.empty() || options_.disk_quota_bytes == 0 || options_.staging_bytes == 0) {
        throw std::invalid_argument("checkpoint store options must be positive");
    }
    std::filesystem::create_directories(options_.root / "sessions");
}

bool SessionCheckpointStore::valid_digest(std::string_view digest) noexcept {
    return digest.size() == 64 &&
           std::all_of(digest.begin(), digest.end(), [](unsigned char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

std::filesystem::path SessionCheckpointStore::session_path(std::string_view digest) const {
    if (!valid_digest(digest)) { throw std::invalid_argument("checkpoint session digest is invalid"); }
    return options_.root / "sessions" / std::string(digest);
}

std::optional<SessionCheckpointSaveResult>
SessionCheckpointStore::save(const ResponseStoreSnapshot& responses,
                             const nlohmann::json& runtime_fingerprint,
                             const EngineExporter& exporter) {
    if (!valid_digest(responses.client_session_sha256) || !runtime_fingerprint.is_object() ||
        !exporter) {
        return std::nullopt;
    }
    std::lock_guard lock(impl_->mutex);
    const std::filesystem::path session = session_path(responses.client_session_sha256);
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
        std::optional<ContinuationCheckpointStats> engine = exporter(writer);
        if (!engine || engine->frontier_tokens == 0 || engine->restored_tokens == 0 ||
            engine->restored_tokens > engine->frontier_tokens) {
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

        std::uint64_t payload_bytes = 0;
        nlohmann::json manifest_files = nlohmann::json::array();
        for (const FileDescriptor& file : *payloads) {
            if (file.bytes > std::numeric_limits<std::uint64_t>::max() - payload_bytes) {
                throw std::overflow_error("checkpoint payload byte count overflowed");
            }
            payload_bytes += file.bytes;
            manifest_files.push_back(descriptor_json(file));
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
        const std::string manifest_text = manifest.dump();
        write_synced(staging / "manifest.json", manifest_text);
        sync_directory(staging);
        const std::uint64_t total_bytes = directory_bytes(staging);
        if (total_bytes > options_.disk_quota_bytes) {
            std::filesystem::remove_all(staging, cleanup_error);
            return std::nullopt;
        }

        std::filesystem::rename(staging, published);
        sync_directory(generations);
        const std::filesystem::path current_staging =
            session / (".current-" + generation);
        write_synced(current_staging, generation + "\n");
        replace_path(current_staging, session / "current");
        sync_directory(session);

        // Do not call the public locking wrapper while the transaction lock is held.
        struct Candidate {
            std::filesystem::path path;
            std::filesystem::file_time_type time;
            std::uint64_t bytes = 0;
            bool abandoned_staging = false;
        };
        std::vector<Candidate> candidates;
        std::uint64_t used = 0;
        const std::filesystem::path sessions_root = options_.root / "sessions";
        for (const auto& session_entry : std::filesystem::directory_iterator(sessions_root)) {
            if (!session_entry.is_directory()) { continue; }
            std::optional<std::string> current;
            try { current = current_generation(session_entry.path()); } catch (...) {}
            const std::filesystem::path dirs = session_entry.path() / "generations";
            if (!std::filesystem::exists(dirs)) { continue; }
            for (const auto& entry : std::filesystem::directory_iterator(dirs)) {
                if (!entry.is_directory()) { continue; }
                const std::string name = entry.path().filename().string();
                const std::uint64_t size = directory_bytes(entry.path());
                if (size > std::numeric_limits<std::uint64_t>::max() - used) {
                    throw std::overflow_error("checkpoint quota accounting overflowed");
                }
                used += size;
                const std::string active_key =
                    session_entry.path().filename().string() + "/" + name;
                if ((name.starts_with(".staging-") || !current || name != *current) &&
                    !impl_->active_generations.contains(active_key)) {
                    candidates.push_back({entry.path(), entry.last_write_time(), size,
                                          name.starts_with(".staging-")});
                }
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& left,
                                                           const Candidate& right) {
            return left.time < right.time;
        });
        for (const Candidate& candidate : candidates) {
            if (used <= options_.disk_quota_bytes && !candidate.abandoned_staging) { continue; }
            std::filesystem::remove_all(candidate.path, cleanup_error);
            if (!cleanup_error) { used -= candidate.bytes; }
            cleanup_error.clear();
        }

        return SessionCheckpointSaveResult{
            .generation = generation, .engine = *engine, .bytes = total_bytes};
    } catch (...) {
        std::filesystem::remove_all(staging, cleanup_error);
        throw;
    }
}

std::optional<VerifiedSessionCheckpoint>
SessionCheckpointStore::load(std::string_view session_sha256,
                             const nlohmann::json& runtime_fingerprint,
                             std::optional<std::string_view> required_response_id) {
    if (!valid_digest(session_sha256) || !runtime_fingerprint.is_object()) { return std::nullopt; }
    std::lock_guard lock(impl_->mutex);
    const std::filesystem::path session = session_path(session_sha256);
    std::optional<std::string> generation;
    try {
        generation = current_generation(session);
    } catch (...) {
        return std::nullopt;
    }
    if (!generation) { return std::nullopt; }
    const std::filesystem::path root = session / "generations" / *generation;
    try {
        const nlohmann::json manifest =
            nlohmann::json::parse(read_text_bounded(root / "manifest.json", 16ULL << 20));
        if (!manifest.is_object() ||
            manifest.at("artifact_type").get<std::string>() != "ninfer_session_checkpoint" ||
            manifest.at("schema_version").get<std::uint32_t>() !=
                kSessionCheckpointSchemaVersion ||
            manifest.at("session_sha256").get<std::string>() != session_sha256 ||
            manifest.at("generation").get<std::string>() != *generation) {
            throw std::runtime_error("checkpoint manifest identity is corrupt");
        }
        if (manifest.at("runtime_fingerprint") != runtime_fingerprint) {
            return std::nullopt;
        }
        if (!manifest.at("files").is_array() || manifest.at("files").empty() ||
            manifest.at("files").size() > 4096) {
            throw std::runtime_error("checkpoint manifest file set is corrupt");
        }
        std::map<std::string, std::uint64_t> allowed;
        std::uint64_t verified_bytes = 0;
        for (const nlohmann::json& encoded : manifest.at("files")) {
            std::optional<FileDescriptor> expected = descriptor_from_json(encoded);
            if (!expected || !allowed.emplace(expected->path, expected->bytes).second) {
                throw std::runtime_error("checkpoint manifest file descriptor is corrupt");
            }
            const FileDescriptor actual = hash_file(root, expected->path, expected->bytes);
            if (actual.sha256 != expected->sha256) {
                throw std::runtime_error("checkpoint payload checksum mismatch");
            }
            if (expected->bytes > std::numeric_limits<std::uint64_t>::max() - verified_bytes) {
                throw std::overflow_error("checkpoint verified byte count overflowed");
            }
            verified_bytes += expected->bytes;
        }
        const auto response_file = allowed.find("responses.cbor");
        if (response_file == allowed.end() || response_file->second > options_.staging_bytes) {
            throw std::runtime_error("checkpoint response payload is unavailable");
        }
        const std::vector<std::byte> response_bytes =
            read_bounded(root / "responses.cbor", options_.staging_bytes);
        std::optional<ResponseStoreSnapshot> responses =
            decode_response_store_snapshot(response_bytes, options_.staging_bytes);
        if (!responses || responses->client_session_sha256 != session_sha256 ||
            responses->latest_response_id !=
                manifest.at("latest_response_id").get<std::string>() ||
            responses->records.size() != manifest.at("response_records").get<std::size_t>()) {
            throw std::runtime_error("checkpoint response state is corrupt");
        }
        if (required_response_id &&
            std::none_of(responses->records.begin(), responses->records.end(),
                         [&](const StoredResponse& response) {
                             return response.id == *required_response_id;
                         })) {
            return std::nullopt;
        }
        ContinuationCheckpointStats expected{
            .frontier_tokens = manifest.at("frontier_tokens").get<std::uint32_t>(),
            .restored_tokens = manifest.at("restored_tokens").get<std::uint32_t>(),
            .payload_bytes = manifest.at("engine_payload_bytes").get<std::uint64_t>(),
        };
        if (expected.frontier_tokens == 0 || expected.restored_tokens == 0 ||
            expected.restored_tokens > expected.frontier_tokens) {
            throw std::runtime_error("checkpoint engine summary is corrupt");
        }
        const std::string active_key = std::string(session_sha256) + "/" + *generation;
        auto reader = std::make_shared<DirectoryCheckpointReader>(
            root, std::move(allowed), impl_, active_key);
        return VerifiedSessionCheckpoint{.responses = std::move(*responses),
                                         .engine = std::move(reader),
                                         .expected_engine = expected,
                                         .generation = *generation,
                                         .bytes = directory_bytes(root)};
    } catch (...) {
        quarantine_generation(session, *generation);
        return std::nullopt;
    }
}

nlohmann::json SessionCheckpointStore::status(
    std::string_view session_sha256, const nlohmann::json& runtime_fingerprint) const {
    nlohmann::json result = {{"artifact_type", "ninfer_session_checkpoint_status"},
                             {"schema_version", kSessionCheckpointSchemaVersion},
                             {"session_sha256", session_sha256},
                             {"state", "missing"}};
    if (!valid_digest(session_sha256) || !runtime_fingerprint.is_object()) { return result; }
    std::lock_guard lock(impl_->mutex);
    try {
        const std::filesystem::path session = session_path(session_sha256);
        const std::optional<std::string> generation = current_generation(session);
        if (!generation) { return result; }
        const std::filesystem::path root = session / "generations" / *generation;
        const nlohmann::json manifest =
            nlohmann::json::parse(read_text_bounded(root / "manifest.json", 16ULL << 20));
        if (manifest.at("artifact_type").get<std::string>() != "ninfer_session_checkpoint" ||
            manifest.at("schema_version").get<std::uint32_t>() !=
                kSessionCheckpointSchemaVersion ||
            manifest.at("session_sha256").get<std::string>() != session_sha256 ||
            manifest.at("generation").get<std::string>() != *generation) {
            result["state"] = "corrupt";
            return result;
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
    } catch (...) {
        result["state"] = "corrupt";
        return result;
    }
}

bool SessionCheckpointStore::erase(std::string_view session_sha256) {
    if (!valid_digest(session_sha256)) { return false; }
    std::lock_guard lock(impl_->mutex);
    const std::string prefix = std::string(session_sha256) + "/";
    if (std::any_of(impl_->active_generations.begin(), impl_->active_generations.end(),
                    [&](const auto& entry) { return entry.first.starts_with(prefix); })) {
        return false;
    }
    std::error_code error;
    const std::uintmax_t removed = std::filesystem::remove_all(session_path(session_sha256), error);
    if (!error) { sync_directory(options_.root / "sessions"); }
    return !error && removed != 0;
}

void SessionCheckpointStore::collect_garbage() {
    std::lock_guard lock(impl_->mutex);
    struct Candidate {
        std::filesystem::path path;
        std::filesystem::file_time_type time;
        std::uint64_t bytes = 0;
        bool abandoned_staging = false;
    };
    std::vector<Candidate> candidates;
    std::uint64_t used = 0;
    const std::filesystem::path sessions_root = options_.root / "sessions";
    for (const auto& session : std::filesystem::directory_iterator(sessions_root)) {
        if (!session.is_directory()) { continue; }
        std::optional<std::string> current;
        try { current = current_generation(session.path()); } catch (...) {}
        const std::filesystem::path generations = session.path() / "generations";
        if (!std::filesystem::exists(generations)) { continue; }
        for (const auto& entry : std::filesystem::directory_iterator(generations)) {
            if (!entry.is_directory()) { continue; }
            const std::string name = entry.path().filename().string();
            const std::uint64_t bytes = directory_bytes(entry.path());
            if (bytes > std::numeric_limits<std::uint64_t>::max() - used) {
                throw std::overflow_error("checkpoint quota accounting overflowed");
            }
            used += bytes;
            const std::string active_key = session.path().filename().string() + "/" + name;
            if ((name.starts_with(".staging-") || !current || name != *current) &&
                !impl_->active_generations.contains(active_key)) {
                candidates.push_back({entry.path(), entry.last_write_time(), bytes,
                                      name.starts_with(".staging-")});
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left,
                                                       const Candidate& right) {
        return left.time < right.time;
    });
    std::error_code error;
    for (const Candidate& candidate : candidates) {
        if (used <= options_.disk_quota_bytes && !candidate.abandoned_staging) { continue; }
        std::filesystem::remove_all(candidate.path, error);
        if (!error) { used -= candidate.bytes; }
        error.clear();
    }
}

} // namespace ninfer::serve
