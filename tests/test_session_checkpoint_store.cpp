#include "core/io/durable_file.h"
#include "core/sha256.h"
#include "runtime/engine/checkpoint_control.h"
#include "serve/session_checkpoint_store.h"
#include "serve/server_identity.h"
#include "checkpoint_write_fault.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
namespace {
struct ParentFlushFault {
    BY_HANDLE_FILE_INFORMATION identity{};
    bool armed = false, fired = false;
    DWORD error = ERROR_WRITE_FAULT;
} parent_flush_fault;

BOOL WINAPI checkpoint_flush(HANDLE handle) {
    const auto disk_error = checkpoint_write_fault::windows_error.load();
    if (disk_error) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileInformationByHandle(handle, &info) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            ++checkpoint_write_fault::fired;
            SetLastError(disk_error);
            return FALSE;
        }
    }
    auto& fault = parent_flush_fault;
    if (fault.armed) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileInformationByHandle(handle, &info) &&
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            info.dwVolumeSerialNumber == fault.identity.dwVolumeSerialNumber &&
            info.nFileIndexHigh == fault.identity.nFileIndexHigh &&
            info.nFileIndexLow == fault.identity.nFileIndexLow) {
            fault.armed = false;
            fault.fired = true;
            SetLastError(fault.error);
            return FALSE;
        }
    }
    static const auto real_flush = reinterpret_cast<decltype(&FlushFileBuffers)>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FlushFileBuffers"));
    if (!real_flush) { std::terminate(); }
    return real_flush(handle);
}
}
// Interpose only the selected parent flush; the real OS performs the current rename.
extern "C" BOOL (WINAPI* __imp_FlushFileBuffers)(HANDLE) = &checkpoint_flush;
#endif

namespace {

using namespace ninfer;
using namespace ninfer::serve;
using ninfer::runtime::DurableCheckpointStats;
using ninfer::runtime::DurableExportResult;
using ninfer::runtime::ContinuationCheckpointWriter;

struct CheckpointLogCapture {
    std::ostringstream output;
    std::streambuf* previous = std::clog.rdbuf(output.rdbuf());
    ~CheckpointLogCapture() { std::clog.rdbuf(previous); }
};

int check(bool condition, const std::string& message) {
    if (condition) { return 0; }
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

int test_checkpoint_failure_diagnostics_are_content_safe() {
    constexpr std::string_view private_text = "private-path origin-secret response-content";
    const auto capture = [&](const std::function<void()>& fail) {
        CheckpointLogCapture log;
        try { fail(); }
        catch (...) {
            log_checkpoint_failure("test_operation", runtime::DurableReason::IoUnavailable);
        }
        return log.output.str();
    };
    int failures = 0;
    for (const auto* category : {&std::system_category(), &std::generic_category()}) {
        const auto text = capture([&] {
            throw std::system_error(5, *category, std::string(private_text));
        });
        const auto value = nlohmann::json::parse(text);
        failures += check(value.size() == 5 && value.at("schema") == "ninfer-checkpoint-io-error/1" &&
                              value.at("operation") == "test_operation" &&
                              value.at("reason") == "io_unavailable" && value.at("error_code") == 5 &&
                              value.at("error_category") ==
                                  (category == &std::system_category() ? "system" : "generic") &&
                              text.find(private_text) == std::string::npos,
                          "checkpoint diagnostic lost OS code or exposed exception content");
    }
    const auto unknown = nlohmann::json::parse(capture([&] {
        throw std::runtime_error(std::string(private_text));
    }));
    failures += check(unknown.at("error_category") == "unavailable" &&
                          unknown.at("error_code").is_null() && unknown.size() == 5,
                      "non-system checkpoint failure invented an OS error code");
    return failures;
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path             = std::filesystem::temp_directory_path() /
                           ("ninfer-checkpoint-test-" + std::to_string(stamp));
#ifdef _WIN32
        // Fixture inspections must not introduce their own MAX_PATH failures when the
        // complete host suite is run with an installed-layout-length TEMP root.
        path = io::extended_local_path(path);
#endif
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() {
#ifdef _WIN32
        try { (void)io::remove_tree(path); } catch (...) {}
#else
        std::error_code error;
        std::filesystem::remove_all(path, error);
#endif
    }

    std::filesystem::path path;
};

class TestReadQueue final : public runtime::ContinuationCheckpointReadQueue {
public:
    class Completion final : public runtime::ContinuationCheckpointReadCompletion {
    public:
        explicit Completion(TestReadQueue& owner) : owner_(owner) {}

        void wait() override {
            if (!done_) {
                done_ = true;
                ++owner_.wait_count;
                --owner_.outstanding;
            }
        }

        ~Completion() override { wait(); }

    private:
        TestReadQueue& owner_;
        bool done_ = false;
    };

    [[nodiscard]] bool available() const noexcept override { return is_available; }

    [[nodiscard]] bool overlaps_batches() const noexcept override { return overlaps; }

    [[nodiscard]] std::string_view backend_name() const noexcept override { return "test-queue"; }

    [[nodiscard]] std::string_view unavailable_reason() const noexcept override { return reason; }

    [[nodiscard]] std::unique_ptr<runtime::ContinuationCheckpointReadCompletion>
    submit(const std::filesystem::path& path,
           std::span<const runtime::ContinuationCheckpointReadRequest> requests) override {
#ifdef _WIN32
        if (path.native().find(L'/') != std::wstring::npos) {
            throw std::runtime_error("checkpoint queue received non-native path separators");
        }
#endif
        ++submit_count;
        if (fail_submit) { throw std::runtime_error("injected checkpoint queue read failure"); }
        if (!overlaps && outstanding != 0) { ++overlap_violations; }
        ++outstanding;
        for (const runtime::ContinuationCheckpointReadRequest& request : requests) {
            std::ifstream input(path, std::ios::binary);
            input.seekg(static_cast<std::streamoff>(request.file_offset));
            input.read(reinterpret_cast<char*>(request.destination.data()),
                       static_cast<std::streamsize>(request.destination.size()));
            if (!input ||
                input.gcount() != static_cast<std::streamsize>(request.destination.size())) {
                throw std::runtime_error(
                    "test checkpoint queue could not read the requested range");
            }
        }
        return std::make_unique<Completion>(*this);
    }

    bool is_available = true;
    bool overlaps     = true;
    bool fail_submit  = false;
    std::string reason;
    std::size_t submit_count       = 0;
    std::size_t wait_count         = 0;
    std::size_t outstanding        = 0;
    std::size_t overlap_violations = 0;
};

ChatTurn rich_turn() {
    ChatTurn turn;
    turn.role = ChatRole::Assistant;
    ContentPart image;
    image.kind              = ContentKind::Image;
    image.type_raw          = "input_image";
    image.source.kind       = product::media_acquire::SourceKind::Bytes;
    image.source.media_type = "image/test";
    image.source.bytes      = {0, 1, 2, 255};
    turn.content.push_back(std::move(image));
    turn.tool_calls.push_back(serve::ToolCall{
        .id = "call_private", .name = "workspace_diff", .arguments_json = "{\"x\":1}"});
    turn.tool_calls.push_back(serve::ToolCall{
        .id = "call_custom", .name = "raw_custom",
        .arguments_json = "literal raw input\n<tool_call>not JSON</tool_call>",
        .kind = ToolKind::Custom});
    turn.reasoning_content = "preserved private reasoning";
    turn.cache_boundary_after = CacheBoundary{.kind = PromptCacheMarkerKind::PrivateLongAnchor};
    turn.private_cache_boundary_after = true;
    turn.tool_result_name = "workspace_diff";
    turn.tool_result_is_error = true;
    turn.content.front().image_resize_policy = ImageResizePolicy::RejectOversized;
    turn.content.front().cache_boundary_after =
        CacheBoundary{.ttl = CacheBoundary::Ttl::OneHour};
    return turn;
}

OpenAIResponseStoreSnapshot sample_snapshot(char digest_byte = 'a') {
    const std::string digest(64, digest_byte);
    ChatTurn user;
    user.role = ChatRole::User;
    ContentPart text;
    text.kind     = ContentKind::Text;
    text.text     = "marker private text";
    text.type_raw = "input_text";
    user.content.push_back(std::move(text));
    const OpenAIResponseContext root  = append_openai_response_context({}, {std::move(user)});
    const OpenAIResponseContext child = append_openai_response_context(root, {rich_turn()});

    StoredOpenAIResponse first;
    first.id                    = "resp_private_first";
    first.session_key           = digest;
    first.client_session_sha256 = digest;
    first.publication_order = 1;
    first.response = {{"id", first.id}, {"object", "response"}, {"status", "completed"}};
    first.input_items.push_back({{"type", "message"}, {"id", "item_private_first"}});
    first.context = root;

    StoredOpenAIResponse second;
    second.id                    = "resp_private_second";
    second.session_key           = digest;
    second.client_session_sha256 = digest;
    second.previous_response_id  = first.id;
    second.publication_order = 2;
    second.response = {{"id", second.id}, {"object", "response"}, {"status", "completed"}};
    second.input_items.push_back(
        {{"type", "function_call_output"},
         {"call_id", "call_private"},
         {"output", "diff --git a/workspace.txt b/workspace.txt\n+exact marker\n"}});
    second.context           = child;
    second.preserve_thinking = true;
    return {.client_session_sha256 = digest,
            .latest_response_id    = second.id,
            .records               = {std::move(first), std::move(second)}};
}

void refresh_config_digest(nlohmann::json& value) {
    const nlohmann::json projection{{"engine", value.at("engine")},
                                     {"frontend", value.at("frontend")},
                                     {"model", value.at("model")},
                                     {"native_layout", value.at("native_layout")},
                                     {"responses", value.at("responses")}};
    const auto text = projection.dump();
    value["deployment"]["effective_config_sha256"] =
        crypto::sha256_hex(crypto::sha256(std::as_bytes(std::span(text.data(), text.size()))));
}

nlohmann::json fingerprint(std::string artifact = "artifact-sha-a") {
    static const auto fixture = [] {
        std::ifstream input(std::filesystem::path(NINFER_SOURCE_DIR) /
                            "tests/fixtures/durable/manifest-text.json", std::ios::binary);
        nlohmann::json manifest;
        input >> manifest;
        return manifest.at("runtime_fingerprint");
    }();
    auto value = fixture;
    if (artifact != "artifact-sha-a") {
        value["model"]["artifact_sha256"] =
            crypto::sha256_hex(crypto::sha256(std::as_bytes(std::span(artifact.data(),
                                                                    artifact.size()))));
        refresh_config_digest(value);
    }
    return value;
}

runtime::DurablePublicationResult publish_fixture(runtime::DurableSessionEpoch,
                                                  const std::function<bool()>& commit) {
    return {.state = commit() ? runtime::DurableExportState::Exported
                              : runtime::DurableExportState::Refused,
             .reason = runtime::DurableReason::None};
}

DurableExportResult exported_fixture(DurableCheckpointStats stats) {
    return {.state = runtime::DurableExportState::Exported, .stats = stats};
}

std::vector<std::byte> engine_payload() {
    std::vector<std::byte> bytes(8193);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>((index * 37U) & 0xffU);
    }
    return bytes;
}

std::uint64_t regular_file_bytes(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) { return 0; }
    if (std::filesystem::is_regular_file(root)) {
        return static_cast<std::uint64_t>(std::filesystem::file_size(root));
    }
    std::uint64_t total = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) { total += static_cast<std::uint64_t>(entry.file_size()); }
    }
    return total;
}

std::uint64_t retained_generation_bytes(const std::filesystem::path& root) {
    std::uint64_t total                  = 0;
    const std::filesystem::path sessions = root / "sessions";
    if (std::filesystem::exists(sessions)) {
        for (const auto& session : std::filesystem::directory_iterator(sessions)) {
            if (session.is_directory()) {
                total += regular_file_bytes(session.path() / "generations");
            }
        }
    }
    const std::filesystem::path tombstones = root / ".tombstones";
    if (std::filesystem::exists(tombstones)) {
        for (const auto& entry : std::filesystem::directory_iterator(tombstones)) {
            const bool whole_session =
                entry.path().filename().string().find("--session--") != std::string::npos;
            total +=
                regular_file_bytes(whole_session ? entry.path() / "generations" : entry.path());
        }
    }
    return total;
}

std::string read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::string> entry_names(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

[[nodiscard]] std::filesystem::path
stored_generation_root(const std::filesystem::path& root, const OpenAIResponseStoreSnapshot& responses,
                       const std::string& generation) {
    return root / "sessions" / responses.client_session_sha256 / "generations" / generation;
}

bool write_chunked(ContinuationCheckpointWriter& writer, std::string_view path,
                   std::span<const std::byte> bytes) {
    std::uint64_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t count =
            std::min<std::size_t>(113, bytes.size() - static_cast<std::size_t>(offset));
        if (!writer.write_file(path, offset, bytes.size(), bytes.subspan(offset, count), 1)) {
            return false;
        }
        offset += count;
    }
    return true;
}

// The store does not interpret native index contents. Single-payload fake exporters use
// an empty index sentinel; the native codec/index and GPU import are separate slice tests.
bool write_state_fixture(ContinuationCheckpointWriter& writer,
                          std::span<const std::byte> bytes) {
    return writer.write_file("engine/index.cbor", 0, 0, {}, 1) &&
           write_chunked(writer, "engine/state/0.bin", bytes);
}

#ifdef _WIN32
// This explicitly named host case requires an elevated administrator or SYSTEM token,
// matching the protected native service root rather than weakening its ACL for the test.
int test_protected_long_root_transaction() {
    TemporaryDirectory temporary;
    auto root = temporary.path / L"protected";
    if (root.native().size() < 155) {
        root += std::wstring(155 - root.native().size(), L'x');
    }
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &raw, nullptr)) {
        throw std::system_error(GetLastError(), std::system_category(),
                                "create protected checkpoint test descriptor");
    }
    const std::unique_ptr<void, decltype(&LocalFree)> descriptor(raw, LocalFree);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), raw, FALSE};
    if (!CreateDirectoryW(root.c_str(), &security)) {
        throw std::system_error(GetLastError(), std::system_category(),
                                "create protected checkpoint test root");
    }
    // Supply the ordinary spelling used by the native launcher, not an already-fixed input.
    const std::filesystem::path ordinary_root(root.native().substr(4));
    int failures = check(ordinary_root.native().size() >= 151,
                         "protected checkpoint root does not reproduce installed path depth");
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    auto queue = std::make_shared<TestReadQueue>();
    SessionCheckpointStore store({.root = ordinary_root,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .origin_mac_key = std::string(32, 'k'),
                                  .read_queue = queue});
    const auto exporter = [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) {
            return {.state = runtime::DurableExportState::Refused};
        }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                  .payload_bytes = payload.size()});
    };
    const auto first = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(first.has_value() && first->dir_sync == "ntfs-write-through",
                      "protected long-root save failed before durable acknowledgement");
    if (!first) { return failures; }
    const auto state = stored_generation_root(root, responses, first->generation) /
                       "engine" / "state" / "0.bin";
    failures += check(state.native().size() - 4 > MAX_PATH &&
                          std::filesystem::file_size(state) == payload.size(),
                      "protected fixture did not exercise metadata beyond MAX_PATH");
    {
        SessionCheckpointStore restarted({.root = ordinary_root,
                                          .disk_quota_bytes = 8ULL << 20,
                                          .staging_bytes = 1ULL << 20,
                                          .origin_mac_key = std::string(32, 'k'),
                                          .read_queue = queue});
        auto loaded = restarted.load(responses.client_session_sha256, fingerprint(),
                                     responses.latest_response_id);
        failures += check(loaded.state == SessionCheckpointLoadState::Available &&
                              loaded.checkpoint.has_value(),
                          "protected long-root restart did not load the published generation");
        if (!loaded.checkpoint) { return failures; }
        std::vector<std::byte> bytes(payload.size());
        failures += check(loaded.checkpoint->engine->read_file("engine/state/0.bin", 0, bytes) &&
                              loaded.checkpoint->engine->verify_complete() && bytes == payload &&
                              queue->submit_count != 0 && queue->outstanding == 0,
                          "protected long-root payload stream did not verify byte-exactly");
    }
    const auto second = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(second && second->generation != first->generation &&
                          second->dir_sync == "ntfs-write-through",
                      "protected long-root resave failed current-pointer replacement");
    failures += check(store.erase(responses.client_session_sha256) ==
                          SessionCheckpointEraseResult::Erased &&
                          store.status(responses.client_session_sha256, fingerprint()).at("state") ==
                              "missing",
                      "protected long-root erase left an available checkpoint");
    return failures;
}
#endif

int test_sha256_streaming() {
    const std::string input = "abc";
    crypto::Sha256 streaming;
    streaming.update(std::as_bytes(std::span(input.data(), 1)));
    streaming.update(std::as_bytes(std::span(input.data() + 1, input.size() - 1)));
    int failures = 0;
    failures += check(crypto::sha256_hex(streaming.finish()) ==
                          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                      "streaming SHA-256 matches the FIPS abc vector");
    const std::span<const std::byte> empty;
    failures += check(crypto::sha256_hex(crypto::sha256(empty)) ==
                          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                      "SHA-256 matches the FIPS empty vector");
    return failures;
}

int test_codec_round_trip() {
    const OpenAIResponseStoreSnapshot original = sample_snapshot();
    const std::vector<std::byte> encoded = encode_response_store_snapshot(original, 1ULL << 20);
    const std::optional<OpenAIResponseStoreSnapshot> decoded =
        decode_response_store_snapshot(encoded, 1ULL << 20);
    int failures = 0;
    failures += check(decoded.has_value(), "typed response snapshot decodes");
    if (!decoded) { return failures; }
    failures += check(decoded->records.size() == 2 &&
                          decoded->latest_response_id == original.latest_response_id,
                      "response ids and lineage survive codec");
    const StoredOpenAIResponse& second = decoded->records.back();
    failures +=
        check(second.previous_response_id == "resp_private_first" && second.preserve_thinking,
              "parent and preserve-thinking state survive codec");
    const std::vector<ChatTurn> turns = flatten_openai_response_context(second.context);
    failures += check(turns.size() == 2 && turns.front().content.size() == 1 &&
                          turns.front().content[0].text == "marker private text",
                      "exact continuation marker survives codec");
    failures += check(turns.back().tool_calls.size() == 2 &&
                          turns.back().tool_calls[0].arguments_json == "{\"x\":1}",
                      "typed tool call survives codec");
    failures += check(second.input_items == original.records.back().input_items &&
                          second.input_items.front().at("output") ==
                              "diff --git a/workspace.txt b/workspace.txt\n+exact marker\n",
                      "nonempty workspace diff and typed tool result survive byte-exactly");
    failures += check(turns.back().content.size() == 1 &&
                          turns.back().content[0].source.bytes ==
                              std::vector<std::uint8_t>({0, 1, 2, 255}) &&
                          turns.back().reasoning_content == "preserved private reasoning",
                      "media bytes and reasoning survive codec");
    failures += check(turns.back().tool_result_name == "workspace_diff" &&
                          turns.back().tool_result_is_error &&
                          turns.back().private_cache_boundary_after &&
                          turns.back().cache_boundary_after ==
                              original.records.back().context->turns.back().cache_boundary_after &&
                          turns.back().content.front().image_resize_policy ==
                              ImageResizePolicy::RejectOversized &&
                          turns.back().content.front().cache_boundary_after ==
                              original.records.back().context->turns.back().content.front()
                                  .cache_boundary_after &&
                          !decoded->records.front().preserve_thinking.has_value() &&
                          decoded->records.back().publication_order == 2,
                      "upstream cache boundaries, tool-result semantics, thinking null and "
                      "lineage publication order survive codec");
    failures += check(turns.back().tool_calls[0].kind == ToolKind::Function &&
                          turns.back().tool_calls[1].kind == ToolKind::Custom &&
                          turns.back().tool_calls[1].arguments_json ==
                              original.records.back().context->turns.back()
                                  .tool_calls[1].arguments_json,
                      "function/custom kinds and raw custom input survive without JSON wrapping");
    auto custom_result = original;
    ChatTurn tool_result;
    tool_result.role = ChatRole::Tool;
    tool_result.tool_kind = ToolKind::Custom;
    tool_result.tool_call_id = "call_custom";
    tool_result.tool_result_name = "raw_custom";
    tool_result.content.push_back(ContentPart{.kind = ContentKind::Text,
                                              .text = "raw result\n<literal>"});
    custom_result.records.back().context =
        append_openai_response_context(custom_result.records.back().context, {tool_result});
    const auto custom_decoded = decode_response_store_snapshot(
        encode_response_store_snapshot(custom_result, 1ULL << 20), 1ULL << 20);
    const auto custom_turns = custom_decoded
                                  ? flatten_openai_response_context(
                                        custom_decoded->records.back().context)
                                  : std::vector<ChatTurn>{};
    failures += check(!custom_turns.empty() &&
                          custom_turns.back().tool_kind == ToolKind::Custom &&
                          custom_turns.back().content.front().text == "raw result\n<literal>",
                      "custom tool-result history round-trips as raw typed history");
    const std::vector<std::byte> reencoded = encode_response_store_snapshot(*decoded, 1ULL << 20);
    failures += check(encoded == reencoded, "response snapshot has a stable canonical encoding");
    OpenAIResponsesStore live(4, 1ULL << 20);
    live.put(original.records.front());
    StoredOpenAIResponse stale;
    stale.id                    = "resp_private_stale";
    stale.session_key           = original.records.front().session_key;
    stale.client_session_sha256 = original.client_session_sha256;
    stale.response              = nlohmann::json::object();
    live.put(stale);
    const auto live_guard = live.begin_restore(original.client_session_sha256);
    bool repaired_external_commit = false;
    failures += check(
        !live.restore_session(original, live_guard,
                             [&] {
                                 repaired_external_commit = true;
                                 return true;
                             }) &&
            !repaired_external_commit && live.size() == 2 &&
            live.get_for_session("resp_private_first",
                                 std::optional<std::string>{original.client_session_sha256}) !=
                nullptr &&
            live.get_for_session("resp_private_second",
                                 std::optional<std::string>{original.client_session_sha256}) ==
                nullptr &&
            live.get_for_session("resp_private_stale",
                                 std::optional<std::string>{original.client_session_sha256}) !=
                nullptr,
        "restore preserves live records absent from the older disk lineage");

    OpenAIResponsesStore foreign(4, 1ULL << 20);
    StoredOpenAIResponse foreign_owner        = original.records.front();
    foreign_owner.client_session_sha256 = std::string(64, 'e');
    foreign.put(foreign_owner);
    const auto foreign_guard = foreign.begin_restore(original.client_session_sha256);
    bool foreign_commit = false;
    failures += check(!foreign.restore_session(original, foreign_guard,
                                               [&] {
                                                   foreign_commit = true;
                                                   return true;
                                               }) &&
                          !foreign_commit && foreign.size() == 1,
                      "incoming id owned by another session stays a fail-closed collision");

    OpenAIResponsesStore partial(4, 1ULL << 20);
    partial.put(original.records.back());
    const auto partial_guard = partial.begin_restore(original.client_session_sha256);
    bool partial_commit = false;
    failures += check(!partial.restore_session(original, partial_guard,
                                               [&] {
                                                   partial_commit = true;
                                                   return false;
                                               }) &&
                          partial_commit && partial.size() == 1 &&
                          partial.get_for_session(
                              "resp_private_second",
                              std::optional<std::string>{original.client_session_sha256}) !=
                              nullptr,
                      "failed external commit preserves the partial session exactly");

    OpenAIResponsesStore mixed(4, 1ULL << 20);
    StoredOpenAIResponse unrelated;
    unrelated.id                    = "resp_unrelated";
    unrelated.session_key           = "other-session";
    unrelated.client_session_sha256 = std::string(64, 'e');
    unrelated.response              = nlohmann::json::object();
    mixed.put(unrelated);
    const auto mixed_guard = mixed.begin_restore(original.client_session_sha256);
    failures += check(
        mixed.restore_session(original, mixed_guard, [] { return true; }) && mixed.size() == 3 &&
            mixed.get_for_session("resp_unrelated",
                                  std::optional<std::string>{std::string(64, 'e')}) != nullptr,
        "unrelated sessions survive a target-session restore");

    OpenAIResponsesStore gated(4, 1ULL << 20);
    const auto gated_guard = gated.begin_restore(original.client_session_sha256);
    bool failed_external_commit = false;
    failures += check(!gated.restore_session(original, gated_guard,
                                             [&] {
                                                 failed_external_commit = true;
                                                 return false;
                                             }) &&
                          failed_external_commit && gated.size() == 0,
                      "failed external restore does not publish Responses state");
    failures += check(
        gated.restore_session(original, gated_guard, [] { return true; }) && gated.size() == 2 &&
            gated.get_for_session("resp_private_first",
                                  std::optional<std::string>{original.client_session_sha256}) !=
                nullptr &&
            gated.get_for_session("resp_private_second",
                                  std::optional<std::string>{original.client_session_sha256}) !=
                nullptr,
        "successful external restore atomically publishes Responses state");
    return failures;
}

int test_export_refusal_diagnostics() {
    using Reason = runtime::SessionCheckpointSkipReason;
    using ExportReason = runtime::ContinuationExportSkipReason;
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    SessionCheckpointStore store({.root = temporary.path,
                                  .read_queue = std::make_shared<TestReadQueue>()});
    int failures = 0;
    for (const Reason gate : {Reason::SessionNotIndexed, Reason::TagMismatch}) {
        runtime::SessionCheckpointSkipDetail skip;
        skip.attempted_tag = responses.latest_response_id;
        const auto saved = store.save(responses, fingerprint(), [&](ContinuationCheckpointWriter&) -> DurableExportResult {
            skip.reason = gate;
            if (gate == Reason::TagMismatch) { skip.catalogued_tag = "resp_catalogued"; }
            return DurableExportResult{.state = runtime::DurableExportState::Refused};
        }, publish_fixture, &skip);
        const auto message = format_session_checkpoint_skip(
            "checkpoint save refused: ", responses.client_session_sha256, skip);
        failures += check(!saved && skip.reason == Reason::ProgramRejected &&
                              skip.first_reason == gate,
                          "store preserves the first engine gate and legacy refusal accounting");
        failures += check(message.view().find(runtime::session_checkpoint_skip_reason_name(gate)) !=
                                  std::string_view::npos &&
                              message.view().find(responses.client_session_sha256.substr(0, 12)) !=
                                  std::string_view::npos &&
                              message.view().find(responses.latest_response_id) != std::string_view::npos,
                          "formatted store refusal retains the engine gate, session, and attempted tag");
        if (gate == Reason::TagMismatch) {
            failures += check(message.view().find(skip.catalogued_tag) != std::string_view::npos,
                              "masked tag mismatch still logs the catalogued tag");
        }
    }

    runtime::SessionCheckpointSkipDetail program_skip;
    const auto program_saved = store.save(responses, fingerprint(), [&](ContinuationCheckpointWriter&) -> DurableExportResult {
        program_skip.reason = Reason::ProgramRejected;
        runtime::ContinuationExportSkipDetail::record(
            &program_skip.export_detail, ExportReason::EndpointNotRetained);
        return DurableExportResult{.state = runtime::DurableExportState::Refused};
    }, publish_fixture, &program_skip);
    const auto program_message = format_session_checkpoint_skip(
        "checkpoint save refused: ", responses.client_session_sha256, program_skip);
    failures += check(!program_saved && program_skip.reason == Reason::ProgramRejected &&
                          program_skip.export_detail.reason == ExportReason::EndpointNotRetained &&
                          program_message.view().find(runtime::continuation_export_skip_reason_name(
                              ExportReason::EndpointNotRetained)) != std::string_view::npos,
                      "store retains and formats a named program refusal");

    runtime::SessionCheckpointSkipDetail stats_skip;
    const auto invalid_stats = store.save(responses, fingerprint(), [](ContinuationCheckpointWriter&) -> DurableExportResult {
        return exported_fixture({.frontier_tokens = 100, .restored_tokens = 94, .payload_bytes = 64});
    }, publish_fixture, &stats_skip);
    failures += check(!invalid_stats && stats_skip.reason == Reason::ProgramRejected &&
                          stats_skip.export_detail.reason == ExportReason::StoreStatsInvalid,
                      "invalid returned statistics name the store gate without changing accounting");

    OpenAIResponseStoreSnapshot invalid = responses;
    invalid.client_session_sha256 = "invalid";
    runtime::SessionCheckpointSkipDetail input_skip;
    bool exporter_called = false;
    const auto invalid_input = store.save(invalid, fingerprint(), [&](ContinuationCheckpointWriter&) -> DurableExportResult {
        exporter_called = true;
        return DurableExportResult{.state = runtime::DurableExportState::Refused};
    }, publish_fixture, &input_skip);
    const auto input_message = format_session_checkpoint_skip(
        "checkpoint save refused: ", invalid.client_session_sha256, input_skip);
    failures += check(!invalid_input && !exporter_called && input_skip.reason == Reason::None &&
                          input_skip.export_detail.reason == ExportReason::StoreInputInvalid &&
                          input_message.view().find(runtime::continuation_export_skip_reason_name(
                              ExportReason::StoreInputInvalid)) != std::string_view::npos,
                      "invalid input is diagnosed without entering export or changing its category");
    return failures;
}

int test_kv_layout_runtime_fingerprint_rejection() {
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .read_queue = std::make_shared<TestReadQueue>()});
    auto identity = fingerprint();
    const auto saved = store.save(responses, identity, [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 100000,
                                           .restored_tokens = 97500,
                                           .payload_bytes = payload.size()});
    }, publish_fixture);
    int failures = check(saved.has_value(), "BF16 checkpoint publishes");
    if (!saved) { return failures; }
    for (const auto storage : {"int8", "fp8", "rk4v4"}) {
        auto changed = identity;
        changed["engine"]["kv_dtype"] = storage;
        refresh_config_digest(changed);
        const auto incompatible = store.load(responses.client_session_sha256, changed);
        failures += check(incompatible.state == SessionCheckpointLoadState::Incompatible &&
                              !incompatible.checkpoint,
                          "checkpoint cannot be loaded under a different KV layout");
    }
    auto compatible = store.load(responses.client_session_sha256, identity);
    failures += check(compatible.state == SessionCheckpointLoadState::Available &&
                          compatible.checkpoint && compatible.checkpoint->generation == saved->generation,
                      "wrong KV profile does not quarantine the compatible checkpoint");
    compatible.checkpoint.reset();
    auto rk4 = identity;
    rk4["engine"]["kv_dtype"] = "rk4v4";
    rk4["native_layout"]["pools"][0]["storage_type"] = "rk4v4_h256_lloyd_g64_s4_g32_v1";
    refresh_config_digest(rk4);
    const auto packed = store.save(responses, rk4, [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) return DurableExportResult{.state = runtime::DurableExportState::Refused};
        return exported_fixture({.frontier_tokens = 100000, .restored_tokens = 97500,
                                 .payload_bytes = payload.size()});
    }, publish_fixture);
    failures += check(packed.has_value(), "rk4v4 fingerprint is supported");
    if (packed) {
        auto same = store.load(responses.client_session_sha256, rk4);
        failures += check(same.state == SessionCheckpointLoadState::Available && same.checkpoint,
                          "rk4v4 checkpoint restores under the identical format");
        same.checkpoint.reset();
        for (const auto* format : {"bf16", "int8_group64", "rk4v4_h256_lloyd_g64_s4_g32_v2"}) {
            auto changed = rk4;
            changed["native_layout"]["pools"][0]["storage_type"] = format;
            refresh_config_digest(changed);
            const auto refused = store.load(responses.client_session_sha256, changed);
            failures += check(refused.state == SessionCheckpointLoadState::Incompatible && !refused.checkpoint,
                              "native codec identity alone refuses a cross-format restore");
        }
    }
    auto unsupported = identity;
    unsupported["responses"]["codec"] = "ninfer-responses-cbor/999";
    refresh_config_digest(unsupported);
    runtime::SessionCheckpointSkipDetail skip;
    bool attempted = false;
    failures += check(!store.save(
                          responses, unsupported,
                          [&](ContinuationCheckpointWriter&) {
                              attempted = true;
                              return exported_fixture({});
                          }, publish_fixture, &skip) &&
                          !attempted && skip.durable_reason == runtime::DurableReason::UnsupportedCodec &&
                          store.load(responses.client_session_sha256, unsupported).state ==
                              SessionCheckpointLoadState::Incompatible,
                      "the actual Responses codec authority rejects unsupported identity labels");
    return failures;
}

int test_transaction_restart_compatibility_and_corruption() {
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload  = engine_payload();
    SessionCheckpointStore store({.root             = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes    = 1ULL << 20,
                                  .read_queue       = std::make_shared<TestReadQueue>()});
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        const std::string metadata = "engine-metadata-v3";
        if (!write_chunked(writer, "engine/index.cbor",
                           std::as_bytes(std::span(metadata.data(), metadata.size()))) ||
            !write_chunked(writer, "engine/state/0.bin", payload)) {
            return DurableExportResult{.state = runtime::DurableExportState::Refused};
        }
        return exported_fixture({.frontier_tokens = 100000,
                                           .restored_tokens = 97500,
                                           .payload_bytes   = payload.size() + metadata.size()});
    };

    int failures = 0;
    const std::optional<SessionCheckpointSaveResult> saved =
        store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(saved.has_value(), "complete generation publishes");
    if (!saved) { return failures; }
    const std::filesystem::path session =
        temporary.path / "sessions" / responses.client_session_sha256;
    failures += check(std::filesystem::is_regular_file(session / "generations" / saved->generation /
                                                       "manifest.json") &&
                          std::filesystem::is_regular_file(session / "current"),
                      "payload, manifest, and current pointer are published");

    // Simulate process death after staging bytes but before manifest/current publication.
    const std::filesystem::path interrupted = session / "generations" / ".staging-interrupted";
    std::filesystem::create_directories(interrupted);
    std::ofstream(interrupted / "partial.bin", std::ios::binary) << "partial";
    auto read_queue = std::make_shared<TestReadQueue>();
    SessionCheckpointStore restarted({.root             = temporary.path,
                                      .disk_quota_bytes = 8ULL << 20,
                                      .staging_bytes    = 1ULL << 20,
                                      .read_queue       = read_queue});
    OpenAIResponsesStore response_store(8, 1ULL << 20);
    const auto response_guard = response_store.begin_restore(responses.client_session_sha256);
    SessionCheckpointLoadResult loaded = restarted.load(
        responses.client_session_sha256, fingerprint(), responses.latest_response_id);
    failures += check(loaded.state == SessionCheckpointLoadState::Available && loaded.checkpoint &&
                          loaded.checkpoint->generation == saved->generation,
                      "standalone restart ignores interrupted staging and restores prior current");
    if (!loaded.checkpoint) { return failures; }
    failures += check(!loaded.checkpoint->engine->verify_complete(),
                      "an unread engine set cannot commit an import");
    // Load issues no payload reads: every engine byte is hashed once, as the engine streams it.
    const std::size_t verification_submits = read_queue->submit_count;
    const std::size_t verification_waits   = read_queue->wait_count;
    std::vector<std::byte> restored(payload.size());
    failures += check(
        loaded.checkpoint->engine->read_file("engine/state/0.bin", 0, restored) &&
            restored == payload && loaded.checkpoint->expected_engine.restored_tokens == 97500 &&
            verification_submits == 0 && read_queue->submit_count == 1 &&
            read_queue->wait_count == verification_waits + 1,
        "engine reader restores exact payload and token summary with one streamed read");
    failures += check(!loaded.checkpoint->engine->verify_complete(),
                      "reading state without the engine index cannot commit an import");
    std::vector<std::byte> metadata(18);
    failures += check(loaded.checkpoint->engine->read_file("engine/index.cbor", 0, metadata) &&
                          loaded.checkpoint->engine->verify_complete(),
                      "all declared engine bytes must verify before import commit");
    failures +=
        check(response_store.restore_session(loaded.checkpoint->responses, response_guard,
                                             [&] {
                                                 return loaded.checkpoint->engine
                                                     ->verify_complete();
                                             }),
              "verified response snapshot installs atomically");
    const auto restored_response = response_store.get_for_session(responses.latest_response_id,
                                                                  responses.client_session_sha256);
    failures +=
        check(restored_response && restored_response->previous_response_id == "resp_private_first",
              "restarted ResponseStore exposes exact continuation lineage");

    loaded.checkpoint.reset();
    const nlohmann::json wrong = fingerprint("different-artifact");
    const SessionCheckpointLoadResult wrong_fingerprint =
        restarted.load(responses.client_session_sha256, wrong);
    failures += check(wrong_fingerprint.state == SessionCheckpointLoadState::Incompatible &&
                          !wrong_fingerprint.checkpoint,
                      "runtime/model fingerprint mismatch is a cache miss");
    const nlohmann::json incompatible = restarted.status(responses.client_session_sha256, wrong);
    failures += check(incompatible.at("state") == "incompatible",
                      "status distinguishes incompatible from corrupt");
    SessionCheckpointLoadResult compatible =
        restarted.load(responses.client_session_sha256, fingerprint());
    failures += check(compatible.state == SessionCheckpointLoadState::Available &&
                          compatible.checkpoint.has_value(),
                      "compatibility miss does not quarantine a valid generation");
    compatible.checkpoint.reset();

    // An exporter cancellation may write staging bytes, but it cannot replace current.
    const auto cancelled =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        (void)write_chunked(writer, "engine/partial.bin", payload);
        return DurableExportResult{.state = runtime::DurableExportState::Refused};
    };
    failures += check(!restarted.save(responses, fingerprint(), cancelled, publish_fixture),
                      "cancelled export is not published");
    const nlohmann::json after_cancel =
        restarted.status(responses.client_session_sha256, fingerprint());
    failures += check(after_cancel.at("state") == "available" &&
                          after_cancel.at("read_backend") == "test-queue" &&
                          after_cancel.at("generation") == saved->generation,
                      "cancelled export leaves prior current unchanged");
    const std::string public_status = after_cancel.dump();
    failures += check(public_status.find(responses.client_session_sha256) == std::string::npos &&
                          public_status.find("resp_private") == std::string::npos &&
                          public_status.find("marker private") == std::string::npos &&
                          public_status.find("call_private") == std::string::npos,
                      "status does not expose raw session identity, response, prompt, tool, or "
                      "reasoning content");

    const auto insufficient_prefix =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_chunked(writer, "engine/insufficient.bin", payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 100000, .restored_tokens = 94999, .payload_bytes = payload.size()});
    };
    failures += check(!restarted.save(responses, fingerprint(), insufficient_prefix, publish_fixture),
                      "generation below the frozen 95 percent prefix floor is not published");
    const nlohmann::json after_insufficient =
        restarted.status(responses.client_session_sha256, fingerprint());
    failures += check(after_insufficient.at("state") == "available" &&
                          after_insufficient.at("generation") == saved->generation,
                      "insufficient-prefix save leaves prior current unchanged");
    const std::optional<SessionCheckpointSaveResult> second =
        restarted.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(second.has_value() && second->generation != saved->generation,
                      "new complete generation replaces current");
    if (!second) { return failures; }
    failures += check(!std::filesystem::exists(interrupted),
                      "successful GC removes an abandoned staging generation");
    const std::filesystem::path generation_root = session / "generations" / second->generation;
    const std::filesystem::path manifest        = generation_root / "manifest.json";
    const std::filesystem::path hidden_manifest = generation_root / "manifest.temporarily-missing";
    std::filesystem::rename(manifest, hidden_manifest);
    const SessionCheckpointLoadResult unavailable =
        restarted.load(responses.client_session_sha256, fingerprint());
    const nlohmann::json unavailable_status =
        restarted.status(responses.client_session_sha256, fingerprint());
    failures +=
        check(unavailable.state == SessionCheckpointLoadState::Unavailable &&
                  !unavailable.checkpoint && unavailable_status.at("state") == "disabled" &&
                  std::filesystem::is_regular_file(session / "current") &&
                  std::filesystem::is_directory(generation_root),
              "transient manifest loss preserves current and returns unavailable");
    std::filesystem::rename(hidden_manifest, manifest);
    SessionCheckpointLoadResult retry =
        restarted.load(responses.client_session_sha256, fingerprint());
    failures += check(retry.state == SessionCheckpointLoadState::Available && retry.checkpoint &&
                          retry.checkpoint->generation == second->generation,
                      "checkpoint succeeds when the transient filesystem failure clears");
    retry.checkpoint.reset();

    const std::filesystem::path corrupt_file = generation_root / "engine" / "state" / "0.bin";
    std::fstream corrupt(corrupt_file, std::ios::in | std::ios::out | std::ios::binary);
    char byte = 0;
    corrupt.read(&byte, 1);
    byte ^= 0x5a;
    corrupt.seekp(0);
    corrupt.write(&byte, 1);
    corrupt.close();
    // A flipped payload byte is caught by the streamed verification, not at load: the reader
    // refuses the payload, the engine import fails closed, and the next load quarantines.
    SessionCheckpointLoadResult corrupted =
        restarted.load(responses.client_session_sha256, fingerprint());
    failures += check(corrupted.state == SessionCheckpointLoadState::Available &&
                          corrupted.checkpoint.has_value(),
                      "a size-preserving flip is not visible before the payload is streamed");
    if (!corrupted.checkpoint) { return failures; }
    std::vector<std::byte> refused(payload.size());
    failures += check(!corrupted.checkpoint->engine->read_file("engine/state/0.bin", 0, refused),
                      "checksum corruption is refused as the engine streams the payload");
    corrupted.checkpoint.reset();
    const SessionCheckpointLoadResult quarantined =
        restarted.load(responses.client_session_sha256, fingerprint());
    failures += check(quarantined.state == SessionCheckpointLoadState::Corrupt &&
                          !quarantined.checkpoint,
                      "checksum corruption cannot enter ResponseStore or Engine");
    failures += check(!std::filesystem::exists(session / "current") &&
                          !std::filesystem::exists(generation_root) &&
                          std::filesystem::exists(session / "generations" / saved->generation),
                      "corrupt current is quarantined while prior immutable generation survives");
    return failures;
}

int test_active_reader_delete_and_gc() {
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload  = engine_payload();
    bool refuse_cleanup                   = false;
    SessionCheckpointStore store({.root              = temporary.path,
                                  .disk_quota_bytes  = 64ULL << 10,
                                  .staging_bytes     = 1ULL << 20,
                                  .read_queue        = std::make_shared<TestReadQueue>(),
                                  .tombstone_cleanup = [&](const std::filesystem::path& path) {
                                      if (refuse_cleanup) { return false; }
                                      std::error_code error;
                                      std::filesystem::remove_all(path, error);
                                      return !error;
                                  }});
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096, .payload_bytes = payload.size()});
    };
    int failures     = 0;
    const auto saved = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(saved.has_value(), "GC fixture generation saves");
    if (!saved) { return failures; }
    const std::filesystem::path session =
        temporary.path / "sessions" / responses.client_session_sha256;
    SessionCheckpointLoadResult loaded = store.load(responses.client_session_sha256, fingerprint());
    const SessionCheckpointEraseResult conflict = store.erase(responses.client_session_sha256);
    failures += check(
        loaded.state == SessionCheckpointLoadState::Available && loaded.checkpoint &&
            conflict == SessionCheckpointEraseResult::Conflict &&
            std::filesystem::is_regular_file(session / "generations" / saved->generation /
                                             "responses.cbor") &&
            std::filesystem::is_regular_file(session / "generations" / saved->generation /
                                             "engine" / "state" / "0.bin") &&
            store.status(responses.client_session_sha256, fingerprint()).at("state") == "available",
        "refused deletion reports conflict and preserves transcript and Engine state");

    const auto newer = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(newer.has_value() && newer->generation != saved->generation,
                      "new current publishes while the prior generation has an active reader");
    if (!newer) { return failures; }

    const std::filesystem::path stale = session / "generations" / "stale-generation";
    std::filesystem::create_directories(stale);
    std::ofstream filler(stale / "large.bin", std::ios::binary);
    std::vector<char> large(128ULL << 10, 'x');
    filler.write(large.data(), large.size());
    filler.close();
    store.collect_garbage();
    failures +=
        check(!std::filesystem::exists(stale) &&
                  std::filesystem::exists(session / "generations" / saved->generation) &&
                  std::filesystem::exists(session / "generations" / newer->generation) &&
                  std::filesystem::exists(session / "current"),
              "quota GC removes stale LRU data but protects active and current generations");
    loaded.checkpoint.reset();
    refuse_cleanup                = true;
    const auto session_tombstones = [&] {
        std::vector<std::filesystem::path> found;
        for (const auto& entry :
             std::filesystem::directory_iterator(temporary.path / ".tombstones")) {
            const std::string name = entry.path().filename().string();
            if (name.starts_with(responses.client_session_sha256 + "--session--")) {
                found.push_back(entry.path());
            }
        }
        return found;
    };
    const SessionCheckpointEraseResult first_erase = store.erase(responses.client_session_sha256);
    const auto resaved                             = store.save(responses, fingerprint(), exporter, publish_fixture);
    const SessionCheckpointEraseResult second_erase = store.erase(responses.client_session_sha256);
    const std::vector<std::filesystem::path> deferred = session_tombstones();
    failures += check(
        first_erase == SessionCheckpointEraseResult::Erased && resaved &&
            second_erase == SessionCheckpointEraseResult::Erased && deferred.size() == 2 &&
            !std::filesystem::exists(session) &&
            store.status(responses.client_session_sha256, fingerprint()).at("state") == "missing" &&
            store.erase(responses.client_session_sha256) == SessionCheckpointEraseResult::Missing,
        "deferred tombstone blocked re-save or a later session delete");
    refuse_cleanup = false;
    store.collect_garbage();
    failures += check(session_tombstones().empty(),
                      "garbage collection did not remove deferred session tombstones");
    return failures;
}

// A first request without a response id consults may_hold before taking any lock, so it must
// never rule out a session that saved and must rule out one that never published, including one
// whose refused first save left its session directory behind.
int test_may_hold_prefilter() {
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload  = engine_payload();
    SessionCheckpointStore store({.root             = temporary.path,
                                  .disk_quota_bytes = 64ULL << 10,
                                  .staging_bytes    = 1ULL << 20,
                                  .read_queue       = std::make_shared<TestReadQueue>()});
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096, .payload_bytes = payload.size()});
    };
    int failures = 0;
    failures += check(!store.may_hold(responses.client_session_sha256) &&
                          !store.may_hold("not-a-session-digest"),
                      "a session that never saved was reported as holding a checkpoint");
    const auto refused_export =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        (void)write_chunked(writer, "engine/partial.bin", payload);
        return DurableExportResult{.state = runtime::DurableExportState::Refused};
    };
    failures += check(!store.save(responses, fingerprint(), refused_export, publish_fixture) &&
                          std::filesystem::exists(temporary.path / "sessions" /
                                                  responses.client_session_sha256) &&
                          !store.may_hold(responses.client_session_sha256),
                      "a refused first save left a session that may_hold reported as restorable");
    const auto saved = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(saved.has_value() && store.may_hold(responses.client_session_sha256),
                      "a saved session was ruled out before its restore");
    return failures;
}

int test_store_wide_quota_across_sessions() {
    const OpenAIResponseStoreSnapshot first_session  = sample_snapshot('a');
    const OpenAIResponseStoreSnapshot second_session = sample_snapshot('b');
    const OpenAIResponseStoreSnapshot third_session  = sample_snapshot('c');
    const std::vector<std::byte> payload       = engine_payload();
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096, .payload_bytes = payload.size()});
    };

    TemporaryDirectory measurement;
    SessionCheckpointStore measuring_store({
        .root             = measurement.path,
        .disk_quota_bytes = 1ULL << 20,
        .staging_bytes    = 1ULL << 20,
        .read_queue       = std::make_shared<TestReadQueue>(),
    });
    const auto measured = measuring_store.save(first_session, fingerprint(), exporter, publish_fixture);
    int failures        = 0;
    failures += check(measured.has_value(), "quota fixture generation size is measurable");
    if (!measured) { return failures; }

    const std::uint64_t quota = measured->bytes * 2;
    TemporaryDirectory temporary;
    SessionCheckpointStore store({.root             = temporary.path,
                                  .disk_quota_bytes = quota,
                                  .staging_bytes    = 1ULL << 20,
                                  .read_queue       = std::make_shared<TestReadQueue>()});
    const auto first  = store.save(first_session, fingerprint(), exporter, publish_fixture);
    const auto second = store.save(second_session, fingerprint(), exporter, publish_fixture);
    const auto third  = store.save(third_session, fingerprint(), exporter, publish_fixture);
    failures +=
        check(first && second && third, "three sessions publish under a two-generation cap");
    if (!first || !second || !third) { return failures; }

    const nlohmann::json first_status =
        store.status(first_session.client_session_sha256, fingerprint());
    const nlohmann::json second_status =
        store.status(second_session.client_session_sha256, fingerprint());
    const nlohmann::json third_status =
        store.status(third_session.client_session_sha256, fingerprint());
    failures +=
        check(first_status.at("state") == "missing" && second_status.at("state") == "available" &&
                  third_status.at("state") == "available",
              "deterministic quota eviction removes the oldest inactive current session");
    failures += check(retained_generation_bytes(temporary.path) <= quota,
                      "resident generations stay within the store-wide disk quota");
    failures += check(
        !std::filesystem::exists(temporary.path / "sessions" / first_session.client_session_sha256 /
                                 "current") &&
            std::filesystem::is_regular_file(temporary.path / "sessions" /
                                             second_session.client_session_sha256 / "current") &&
            std::filesystem::is_regular_file(temporary.path / "sessions" /
                                             third_session.client_session_sha256 / "current"),
        "eviction removes its current pointer while retained sessions keep valid pointers");
    TemporaryDirectory protected_temporary;
    SessionCheckpointStore protected_store({
        .root             = protected_temporary.path,
        .disk_quota_bytes = measured->bytes,
        .staging_bytes    = 1ULL << 20,
        .read_queue       = std::make_shared<TestReadQueue>(),
    });
    const auto protected_saved = protected_store.save(first_session, fingerprint(), exporter, publish_fixture);
    SessionCheckpointLoadResult active =
        protected_store.load(first_session.client_session_sha256, fingerprint());
    const auto refused = protected_store.save(second_session, fingerprint(), exporter, publish_fixture);
    failures +=
        check(protected_saved && active.checkpoint && !refused &&
                  protected_store.status(first_session.client_session_sha256, fingerprint())
                          .at("state") == "available" &&
                  protected_store.status(second_session.client_session_sha256, fingerprint())
                          .at("state") == "missing" &&
                  retained_generation_bytes(protected_temporary.path) <= measured->bytes,
              "active reader ownership rejects admission without exceeding the quota");

    TemporaryDirectory failure_temporary;
    bool refuse_cleanup = false;
    SessionCheckpointStore failure_store(
        {.root              = failure_temporary.path,
         .disk_quota_bytes  = quota,
         .staging_bytes     = 1ULL << 20,
         .read_queue        = std::make_shared<TestReadQueue>(),
         .tombstone_cleanup = [&](const std::filesystem::path& path) {
             if (refuse_cleanup) { return false; }
             std::error_code error;
             std::filesystem::remove_all(path, error);
             return !error;
         }});
    const auto failure_first    = failure_store.save(first_session, fingerprint(), exporter, publish_fixture);
    const auto failure_previous = failure_store.save(second_session, fingerprint(), exporter, publish_fixture);
    failures += check(failure_first && failure_previous,
                      "quota failure fixture publishes two current sessions");
    if (!failure_first || !failure_previous) { return failures; }
    const std::filesystem::path previous_session =
        failure_temporary.path / "sessions" / second_session.client_session_sha256;
    const std::filesystem::path previous_generation =
        previous_session / "generations" / failure_previous->generation;
    const std::string previous_current   = read_file_bytes(previous_session / "current");
    const std::string previous_manifest  = read_file_bytes(previous_generation / "manifest.json");
    const std::string previous_responses = read_file_bytes(previous_generation / "responses.cbor");
    const std::string previous_engine =
        read_file_bytes(previous_generation / "engine" / "state" / "0.bin");

    // Re-saving at the same size fits the tolerated publish transient and evicts nobody. A
    // successor that grows past it plans reclamation, but cleanup happens only after
    // publication. Cleanup failure must not turn that committed save into a refusal.
    std::vector<std::byte> grown = payload;
    grown.insert(grown.end(), payload.begin(), payload.begin() + payload.size() / 2);
    const auto grown_exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, grown)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096, .payload_bytes = grown.size()});
    };
    refuse_cleanup                = true;
    const auto replacement = failure_store.save(second_session, fingerprint(), grown_exporter, publish_fixture);
    SessionCheckpointLoadResult previous_loaded =
        failure_store.load(second_session.client_session_sha256, fingerprint());
    std::filesystem::path deferred_tombstone;
    for (const auto& entry :
         std::filesystem::directory_iterator(failure_temporary.path / ".tombstones")) {
        if (entry.path().filename().string().starts_with(second_session.client_session_sha256 +
                                                         "--generation--")) {
            deferred_tombstone = entry.path();
            break;
        }
    }
    failures += check(
        replacement && previous_loaded.state == SessionCheckpointLoadState::Available &&
            previous_loaded.checkpoint &&
            previous_loaded.checkpoint->generation == replacement->generation &&
            read_file_bytes(previous_session / "current") != previous_current &&
            std::filesystem::is_directory(deferred_tombstone) &&
            read_file_bytes(deferred_tombstone / "manifest.json") == previous_manifest &&
            read_file_bytes(deferred_tombstone / "responses.cbor") == previous_responses &&
            read_file_bytes(deferred_tombstone / "engine" / "state" / "0.bin") == previous_engine &&
            failure_store.status(first_session.client_session_sha256, fingerprint()).at("state") ==
                "available" &&
            retained_generation_bytes(failure_temporary.path) ==
                failure_first->bytes + failure_previous->bytes + replacement->bytes,
        "cleanup failure after publication keeps its acknowledgement and unrelated current");
    previous_loaded.checkpoint.reset();
    failures += check(!failure_store.save(second_session, fingerprint(), grown_exporter, publish_fixture),
                      "stuck cleanup refuses the next transient instead of accumulating excess");
    refuse_cleanup = false;
    failure_store.collect_garbage();
    failures += check(
        !std::filesystem::exists(deferred_tombstone) &&
            failure_store.status(second_session.client_session_sha256, fingerprint()).at("state") ==
                "available" &&
            retained_generation_bytes(failure_temporary.path) == replacement->bytes,
        "deferred tombstones remain quota-accounted and are reclaimed by GC");
    return failures;
}

// Oldest-first reclamation let a run of short sessions reclaim every long session's checkpoint
// (26 short OMP sessions filled an RTX 5090's 24 GiB store). A short session is reclaimed first,
// though it is newer than the long session the save displaces.
int test_quota_reclaims_short_sessions_first() {
    const OpenAIResponseStoreSnapshot long_first   = sample_snapshot('a');
    const OpenAIResponseStoreSnapshot short_middle = sample_snapshot('b');
    const OpenAIResponseStoreSnapshot long_last    = sample_snapshot('c');
    const std::vector<std::byte> payload     = engine_payload();
    const auto exporter                      = [&payload](std::uint32_t frontier) {
        return [&payload, frontier](ContinuationCheckpointWriter& writer)
                   -> DurableExportResult {
            if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
            return exported_fixture({.frontier_tokens = frontier,
                                               .restored_tokens = frontier,
                                               .payload_bytes   = payload.size()});
        };
    };

    TemporaryDirectory measurement;
    SessionCheckpointStore measuring_store({
        .root             = measurement.path,
        .disk_quota_bytes = 1ULL << 20,
        .staging_bytes    = 1ULL << 20,
        .read_queue       = std::make_shared<TestReadQueue>(),
    });
    const auto measured = measuring_store.save(long_first, fingerprint(), exporter(40'000), publish_fixture);
    int failures = check(measured.has_value(), "short-first fixture generation is measurable");
    if (!measured) { return failures; }

    TemporaryDirectory temporary;
    SessionCheckpointStore store({.root                 = temporary.path,
                                  .disk_quota_bytes     = measured->bytes * 2,
                                  .short_session_tokens = 32'768,
                                  .staging_bytes        = 1ULL << 20,
                                  .read_queue           = std::make_shared<TestReadQueue>()});
    const bool saved = store.save(long_first, fingerprint(), exporter(40'000), publish_fixture).has_value() &&
                       store.save(short_middle, fingerprint(), exporter(4'096), publish_fixture).has_value() &&
                       store.save(long_last, fingerprint(), exporter(40'000), publish_fixture).has_value();
    failures += check(saved, "three sessions publish under a two-generation cap");
    const auto state = [&](const OpenAIResponseStoreSnapshot& session) {
        return store.status(session.client_session_sha256, fingerprint()).at("state");
    };
    failures += check(state(long_first) == "available" && state(short_middle) == "missing" &&
                          state(long_last) == "available",
                      "quota reclamation took an older long session before a short one");
    return failures;
}

int test_quota_stale_publication_preserves_unrelated_currents() {
    TemporaryDirectory measurement;
    const auto a = sample_snapshot('a');
    const auto b = sample_snapshot('b');
    const auto c = sample_snapshot('c');
    const auto payload = engine_payload();
    const auto exporter = [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) {
            return DurableExportResult{.state = runtime::DurableExportState::Refused};
        }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                 .payload_bytes = payload.size()});
    };
    SessionCheckpointStore measure({.root = measurement.path, .disk_quota_bytes = 1ULL << 20,
        .staging_bytes = 1ULL << 20, .read_queue = std::make_shared<TestReadQueue>()});
    const auto size = measure.save(a, fingerprint(), exporter, publish_fixture);
    if (!size) { return check(false, "quota regression fixture failed"); }
    TemporaryDirectory temporary;
    SessionCheckpointStore store({.root = temporary.path, .disk_quota_bytes = 2 * size->bytes,
        .staging_bytes = 1ULL << 20, .read_queue = std::make_shared<TestReadQueue>()});
    const auto first = store.save(a, fingerprint(), exporter, publish_fixture);
    const auto second = store.save(b, fingerprint(), exporter, publish_fixture);
    if (!first || !second) { return check(false, "quota regression could not fill quota"); }
    int failures = 0;
    for (const auto reason : {runtime::DurableReason::StalePublication,
                              runtime::DurableReason::IoUnavailable}) {
        runtime::SessionCheckpointSkipDetail skip;
        const auto refused = store.save(c, fingerprint(), exporter,
            [&](runtime::DurableSessionEpoch, const std::function<bool()>&) {
                return runtime::DurablePublicationResult{
                    reason == runtime::DurableReason::StalePublication
                        ? runtime::DurableExportState::Stale : runtime::DurableExportState::Refused,
                    reason};
            }, &skip);
        failures += check(!refused && skip.durable_reason == reason &&
            store.status(a.client_session_sha256, fingerprint()).value("generation", "") == first->generation &&
            store.status(b.client_session_sha256, fingerprint()).value("generation", "") == second->generation,
            "failed quota publication deleted an unrelated current");
    }
    return failures;
}

int test_replay_only_survivors_are_durable_without_native_authority() {
    TemporaryDirectory temporary;
    auto survivors = sample_snapshot();
    survivors.records.pop_back();
    survivors.latest_response_id = survivors.records.front().id;
    survivors.replay_only = true;
    const std::string key(32, 'r');
    const SessionCheckpointStoreOptions options{.root = temporary.path,
        .disk_quota_bytes = 1ULL << 20, .staging_bytes = 1ULL << 20,
        .origin_mac_key = key, .read_queue = std::make_shared<TestReadQueue>()};
    std::string generation;
    int failures = 0;
    {
        SessionCheckpointStore store(options);
        const auto saved = store.save_responses(survivors, fingerprint());
        if (!saved) { return check(false, "replay-only survivor publication failed"); }
        generation = saved->generation;
        failures += check(saved->engine.payload_bytes == 0 &&
                          saved->engine.frontier_tokens == 0 && saved->origin_authenticated,
                          "replay-only save fabricated native export evidence");
    }
    SessionCheckpointStore restarted(options);
    const auto loaded = restarted.load(survivors.client_session_sha256, fingerprint(),
                                       survivors.latest_response_id);
    failures += check(loaded.state == SessionCheckpointLoadState::Available &&
        loaded.checkpoint && loaded.checkpoint->responses.replay_only &&
        loaded.checkpoint->responses.records.size() == 1 && !loaded.checkpoint->engine &&
        loaded.checkpoint->origin_authenticated &&
        !restarted.covers(survivors.client_session_sha256, fingerprint(), survivors.latest_response_id),
        "restart lost replay-only survivors or granted them native release authority");
    const auto directory = stored_generation_root(temporary.path, survivors, generation);
    const auto manifest = nlohmann::json::parse(read_file_bytes(directory / "manifest.json"));
    failures += check(manifest.at("mode") == "responses_only" && manifest.at("files").size() == 2 &&
                      manifest.at("engine_payload_bytes") == 0 &&
                      !std::filesystem::exists(directory / "engine"),
                      "replay-only generation contains a fabricated native checkpoint");
    const auto missing = restarted.load(survivors.client_session_sha256, fingerprint(),
                                        "resp_private_second");
    failures += check(missing.state == SessionCheckpointLoadState::Missing &&
                      missing.reason == runtime::DurableReason::RequiredResponseMissing,
                      "deleted endpoint reappeared after replay-only restart");
    auto native = survivors;
    native.replay_only = false;
    runtime::SessionCheckpointSkipDetail skip;
    failures += check(!restarted.save_responses(native, fingerprint(), &skip) &&
                      skip.durable_reason == runtime::DurableReason::InvalidManifest,
                      "native snapshot silently became replay-only");
    std::filesystem::remove(directory / "manifest.mac");
    const auto unauthenticated = restarted.load(survivors.client_session_sha256, fingerprint());
    failures += check(unauthenticated.state == SessionCheckpointLoadState::Incompatible &&
                      unauthenticated.reason == runtime::DurableReason::OriginRequired,
                      "replay-only tombstone was accepted without origin authentication");
    return failures;
}

template <class Control, class Validator>
void install_pressure_validator(Control& control, Validator validator) {
    // The baseline has no proof hook; its existing settle path remains exercised.
    if constexpr (requires { control.set_validator(validator); }) {
        control.set_validator(std::move(validator));
    }
}

template <class Victim>
runtime::DurableReason pressure_reason(const Victim& victim) {
    if constexpr (requires { victim.reason; }) { return victim.reason; }
    return runtime::DurableReason::None;
}

template <class Control>
void retire_pressure_pins(Control& control) {
    if constexpr (requires { control.retire_release_pins(); }) { control.retire_release_pins(); }
}

int test_ordinary_pressure_preserves_same_size_corrupt_victim() {
    using namespace runtime;
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path, .disk_quota_bytes = 1ULL << 20,
        .staging_bytes = 1ULL << 20, .origin_mac_key = std::string(32, 'k'),
        .read_queue = std::make_shared<TestReadQueue>()});
    const auto saved = store.save(responses, fingerprint(), [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) {
            return DurableExportResult{.state = DurableExportState::Refused};
        }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                 .payload_bytes = payload.size()});
    }, publish_fixture);
    if (!saved) { return check(false, "pressure corruption fixture failed"); }
    auto bytes = read_file_bytes(stored_generation_root(temporary.path, responses, saved->generation) /
                                 "engine" / "state" / "0.bin");
    bytes[bytes.size() / 2] ^= 1;
    std::ofstream(stored_generation_root(temporary.path, responses, saved->generation) /
                  "engine" / "state" / "0.bin", std::ios::binary | std::ios::trunc) << bytes;
    struct Oracle final : ReclaimableSessionOracle {
        SessionCheckpointStore& store;
        explicit Oracle(SessionCheckpointStore& value) : store(value) {}
        std::shared_ptr<const ContinuationCheckpointReader> pin_recoverable(
            std::string_view session, std::string_view tag) const noexcept override {
            try {
                auto loaded = store.load(session, fingerprint(), tag);
                if (!loaded.checkpoint) { return {}; }
                return std::move(loaded.checkpoint->engine);
            } catch (...) { return {}; }
        }
    };
    struct Handler final : PressureCheckpointHandler {
        void save_before_eviction(std::span<PressureCheckpointVictim> victims) noexcept override {
            for (auto& victim : victims) { victim.outcome = PressureCheckpointOutcome::Settled; }
        }
    };
    CheckpointControl control;
    control.set_oracle(std::make_shared<Oracle>(store));
    control.set_handler(std::make_shared<Handler>());
    unsigned validations = 0;
    install_pressure_validator(control, [&](const ContinuationCheckpointReader& reader) {
        ++validations;
        std::vector<std::byte> streamed(payload.size());
        return reader.read_file("engine/state/0.bin", 0, streamed) && reader.verify_complete()
            ? DurableReason::None : reader.failure_reason();
    });
    int failures = check(store.covers(responses.client_session_sha256, fingerprint(),
                                      responses.latest_response_id),
                         "same-size corruption fixture must pass metadata-only covers");
    PressureCheckpointVictim victim{responses.client_session_sha256, responses.latest_response_id};
    const bool release_allowed = control.settle({&victim, 1});
    failures += check(!release_allowed && validations == 1 &&
                      victim.outcome == PressureCheckpointOutcome::Pending &&
                      pressure_reason(victim) == DurableReason::PayloadDigestMismatch,
                      "ordinary pressure released the last resident copy on metadata-only coverage");
    if (failures) { return failures; }
    const auto replacement = store.save(responses, fingerprint(), [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) {
            return DurableExportResult{.state = DurableExportState::Refused};
        }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                 .payload_bytes = payload.size()});
    }, publish_fixture);
    if (!replacement) { return check(false, "replacement save after corruption failed"); }
    control.notify_progress();
    PressureCheckpointVictim repaired{responses.client_session_sha256, responses.latest_response_id};
    failures += check(control.settle({&repaired, 1}) && validations == 2 &&
                      store.erase(responses.client_session_sha256) == SessionCheckpointEraseResult::Conflict,
                      "validated ordinary release failed to pin its generation through native release");
    retire_pressure_pins(control);
    failures += check(store.erase(responses.client_session_sha256) == SessionCheckpointEraseResult::Erased,
                      "ordinary release pin outlived its worker turn");
    return failures;
}

int test_load_scan_failure_does_not_deadlock() {
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload  = engine_payload();
    SessionCheckpointStore store({
        .root             = temporary.path,
        .disk_quota_bytes = 8ULL << 20,
        .staging_bytes    = 1ULL << 20,
        .read_queue       = std::make_shared<TestReadQueue>(),
        .generation_size  = [](const std::filesystem::path&) -> std::uint64_t {
            throw std::runtime_error("injected directory scan failure");
        },
    });
    const auto saved = store.save(responses, fingerprint(), [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 4096,
        .restored_tokens = 4096,
        .payload_bytes   = payload.size(),});
    }, publish_fixture);
    if (!saved) { return check(false, "directory-scan fixture did not save"); }
    const SessionCheckpointLoadResult loaded =
        store.load(responses.client_session_sha256, fingerprint());
    return check(loaded.state == SessionCheckpointLoadState::Unavailable && !loaded.checkpoint,
                 "post-verification directory scan did not fail without deadlocking");
}

// A session whose checkpoint exceeds half the quota must still be able to save a successor:
// the superseded current generation is tolerated as a publish transient and reclaimed after
// the current pointer swaps (alphastorm/ninfer#25).
int test_large_session_resaves_within_quota() {
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    std::vector<std::byte> payload(48ULL << 10);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::byte>((index * 41U) & 0xffU);
    }
    SessionCheckpointStore store({.root             = temporary.path,
                                  .disk_quota_bytes = 64ULL << 10,
                                  .staging_bytes    = 1ULL << 20,
                                  .read_queue       = std::make_shared<TestReadQueue>()});
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096, .payload_bytes = payload.size()});
    };
    int failures = 0;
    const std::filesystem::path session =
        temporary.path / "sessions" / responses.client_session_sha256;

    const auto first = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(first.has_value(), "first oversized-session generation saves");
    if (!first) { return failures; }
    failures += check(first->bytes * 2 > (64ULL << 10),
                      "fixture generation must exceed half the quota for this regression");

    ninfer::runtime::SessionCheckpointSkipDetail skip;
    const auto second = store.save(responses, fingerprint(), exporter, publish_fixture, &skip);
    failures += check(second.has_value() && second->generation != first->generation,
                      "successor save must tolerate the superseded generation as a transient");
    if (!second) { return failures; }
    failures += check(!std::filesystem::exists(session / "generations" / first->generation),
                      "superseded generation is reclaimed after publish");
    SessionCheckpointLoadResult loaded = store.load(responses.client_session_sha256, fingerprint());
    failures += check(loaded.state == SessionCheckpointLoadState::Available &&
                          loaded.checkpoint.has_value(),
                      "reclaimed store still loads the successor generation");
    loaded.checkpoint.reset();

    // A single generation larger than the whole quota is still refused, with the named reason.
    std::vector<std::byte> oversized(96ULL << 10);
    const auto refuse_exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, oversized)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 4096,
                                           .restored_tokens = 4096,
                                           .payload_bytes   = oversized.size()});
    };
    ninfer::runtime::SessionCheckpointSkipDetail oversize_skip;
    const auto refused = store.save(responses, fingerprint(), refuse_exporter, publish_fixture, &oversize_skip);
    failures += check(!refused.has_value() &&
                          oversize_skip.reason ==
                              ninfer::runtime::SessionCheckpointSkipReason::QuotaExceeded,
                      "generation larger than the quota is refused with the named reason");
    failures += check(store.status(responses.client_session_sha256, fingerprint())
                              .at("state") == "available",
                      "refused oversized save leaves the published successor untouched");
    return failures;
}

// Re-saving one session must not cost another session its only checkpoint: the superseded
// generation is the publish transient, and the post-publish pass reclaims it first. Deleting the
// other session instead lost every session evicted from engine memory whenever a resident one
// was saved again under quota pressure (alphastorm/omp-ninfer#46).
int test_resave_keeps_other_sessions_within_quota() {
    const OpenAIResponseStoreSnapshot first_session  = sample_snapshot('a');
    const OpenAIResponseStoreSnapshot second_session = sample_snapshot('b');
    const std::vector<std::byte> payload       = engine_payload();
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096, .payload_bytes = payload.size()});
    };

    TemporaryDirectory measurement;
    SessionCheckpointStore measuring_store({
        .root             = measurement.path,
        .disk_quota_bytes = 1ULL << 20,
        .staging_bytes    = 1ULL << 20,
        .read_queue       = std::make_shared<TestReadQueue>(),
    });
    const auto measured = measuring_store.save(first_session, fingerprint(), exporter, publish_fixture);
    int failures        = 0;
    failures += check(measured.has_value(), "quota fixture generation size is measurable");
    if (!measured) { return failures; }

    // Two sessions fit; the transient of re-saving one of them does not.
    const std::uint64_t quota = measured->bytes * 5 / 2;
    TemporaryDirectory temporary;
    SessionCheckpointStore store({.root             = temporary.path,
                                  .disk_quota_bytes = quota,
                                  .staging_bytes    = 1ULL << 20,
                                  .read_queue       = std::make_shared<TestReadQueue>()});
    const auto first   = store.save(first_session, fingerprint(), exporter, publish_fixture);
    const auto second  = store.save(second_session, fingerprint(), exporter, publish_fixture);
    const auto resaved = store.save(first_session, fingerprint(), exporter, publish_fixture);
    failures += check(first && second && resaved, "both sessions publish and the first re-saves");
    if (!first || !second || !resaved) { return failures; }
    failures += check(
        store.status(second_session.client_session_sha256, fingerprint()).at("state") ==
            "available",
        "re-saving one session evicted another session's only checkpoint");
    failures += check(!std::filesystem::exists(temporary.path / "sessions" /
                                               first_session.client_session_sha256 /
                                               "generations" / first->generation),
                      "the superseded generation outlived the re-save");
    failures += check(retained_generation_bytes(temporary.path) <= quota,
                      "re-save left the store above its disk quota");
    return failures;
}

// A reclamation that fails must not turn into more evictions: through a broken cleanup path each
// evicted session loses its only checkpoint without freeing a byte. Whether the failure hits the
// superseded generation after publish or a stuck tombstone before it, the other session keeps its
// checkpoint, and usage stays within one generation of the quota (council
// CR-20260924-ninfer-durable-evict-r1, daybreak-blue R1).
int test_failed_reclamation_keeps_other_sessions() {
    const OpenAIResponseStoreSnapshot first_session  = sample_snapshot('a');
    const OpenAIResponseStoreSnapshot second_session = sample_snapshot('b');
    const std::vector<std::byte> payload       = engine_payload();
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096, .payload_bytes = payload.size()});
    };
    TemporaryDirectory measurement;
    SessionCheckpointStore measuring_store({
        .root             = measurement.path,
        .disk_quota_bytes = 1ULL << 20,
        .staging_bytes    = 1ULL << 20,
        .read_queue       = std::make_shared<TestReadQueue>(),
    });
    const auto measured = measuring_store.save(first_session, fingerprint(), exporter, publish_fixture);
    int failures        = 0;
    failures += check(measured.has_value(), "quota fixture generation size is measurable");
    if (!measured) { return failures; }
    const std::uint64_t quota = measured->bytes * 5 / 2;

    // Cleanup refuses exactly the paths this predicate names; everything else is deleted.
    struct Fixture {
        TemporaryDirectory temporary;
        std::function<bool(const std::filesystem::path&)> stuck;
        SessionCheckpointStore store;
        explicit Fixture(std::uint64_t quota)
            : store({.root              = temporary.path,
                     .disk_quota_bytes  = quota,
                     .staging_bytes     = 1ULL << 20,
                     .read_queue        = std::make_shared<TestReadQueue>(),
                     .tombstone_cleanup = [this](const std::filesystem::path& path) {
                         if (stuck && stuck(path)) { return false; }
                         std::error_code error;
                         std::filesystem::remove_all(path, error);
                         return !error;
                     }}) {}
    };

    {
        Fixture fixture(quota);
        const auto first  = fixture.store.save(first_session, fingerprint(), exporter, publish_fixture);
        const auto second = fixture.store.save(second_session, fingerprint(), exporter, publish_fixture);
        failures += check(first && second, "post-publish fixture publishes two sessions");
        if (!first || !second) { return failures; }
        const std::string superseded = first_session.client_session_sha256 + "--generation--";
        fixture.stuck                = [&](const std::filesystem::path& path) {
            return path.filename().string().starts_with(superseded);
        };
        const auto resaved = fixture.store.save(first_session, fingerprint(), exporter, publish_fixture);
        failures += check(resaved.has_value(),
                          "re-save is acknowledged although its superseded generation is stuck");
        failures += check(
            fixture.store.status(second_session.client_session_sha256, fingerprint())
                    .at("state") == "available",
            "a stuck superseded generation evicted another session's only checkpoint");
        failures += check(retained_generation_bytes(fixture.temporary.path) <=
                              quota + measured->bytes,
                          "usage left more than one generation above the quota");
    }

    {
        Fixture fixture(quota);
        const auto first  = fixture.store.save(first_session, fingerprint(), exporter, publish_fixture);
        const auto second = fixture.store.save(second_session, fingerprint(), exporter, publish_fixture);
        failures += check(first && second, "stuck-tombstone fixture publishes two sessions");
        if (!first || !second) { return failures; }
        const std::filesystem::path stuck = fixture.temporary.path / ".tombstones" / "stuck";
        std::filesystem::create_directories(stuck);
        std::ofstream(stuck / "held", std::ios::binary) << "held open by another process";
        fixture.stuck = [&](const std::filesystem::path& path) { return path == stuck; };
        ninfer::runtime::SessionCheckpointSkipDetail skip;
        const auto resaved = fixture.store.save(first_session, fingerprint(), exporter, publish_fixture, &skip);
        failures += check(!resaved.has_value() &&
                              skip.reason ==
                                  ninfer::runtime::SessionCheckpointSkipReason::QuotaExceeded,
                          "a stuck tombstone must refuse the re-save that no longer fits");
        failures += check(
            fixture.store.status(second_session.client_session_sha256, fingerprint())
                    .at("state") == "available",
            "a stuck tombstone evicted another session's only checkpoint");
        SessionCheckpointLoadResult kept =
            fixture.store.load(first_session.client_session_sha256, fingerprint());
        failures += check(kept.state == SessionCheckpointLoadState::Available && kept.checkpoint &&
                              kept.checkpoint->generation == first->generation,
                          "the refused re-save left the session's previous checkpoint in place");
    }
    return failures;
}

// A queue that cannot hold two batches in flight (Windows DirectStorage: one status slot, one
// fence) must be drained between the reader's batches; issuing the next batch early used to be
// caught as a failed read and surfaced to the client as previous_response_not_found.
int test_serialising_read_queue_streams_batches_in_order() {
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    std::vector<std::byte> payload((32ULL << 20) + (5ULL << 20) + 4093);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::byte>((index * 131U + (index >> 12)) & 0xffU);
    }
    auto read_queue      = std::make_shared<TestReadQueue>();
    read_queue->overlaps = false;
    SessionCheckpointStore store({.root             = temporary.path,
                                  .disk_quota_bytes = 128ULL << 20,
                                  .staging_bytes    = 1ULL << 20,
                                  .read_queue       = read_queue});
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096, .payload_bytes = payload.size()});
    };
    int failures     = 0;
    const auto saved = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(saved.has_value(), "multi-batch payload saves");
    if (!saved) { return failures; }
    SessionCheckpointLoadResult loaded = store.load(responses.client_session_sha256, fingerprint());
    failures += check(loaded.state == SessionCheckpointLoadState::Available &&
                          loaded.checkpoint.has_value(),
                      "multi-batch generation loads");
    if (!loaded.checkpoint) { return failures; }
    std::vector<std::byte> restored(payload.size());
    const bool read = loaded.checkpoint->engine->read_file("engine/state/0.bin", 0, restored);
    failures += check(read && restored == payload,
                      "serialising queue restores the exact multi-batch payload");
    failures += check(read_queue->submit_count == 2 && read_queue->overlap_violations == 0,
                      "reader issues one batch at a time on a queue that cannot overlap");
    return failures;
}

// Reclamation failure after the current pointer swaps must never fail the acknowledged save:
// publication is durable, eviction is best effort, and the next save's own enforcement pass
// withdraws the transient tolerance while cleanup keeps failing (council CR-20260831 R1).
int test_post_publish_reclaim_failure_keeps_save_acknowledged() {
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    std::vector<std::byte> payload(48ULL << 10);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::byte>((index * 59U) & 0xffU);
    }
    bool cleanup_throws = false;
    SessionCheckpointStore store({.root              = temporary.path,
                                  .disk_quota_bytes  = 64ULL << 10,
                                  .staging_bytes     = 1ULL << 20,
                                  .read_queue        = std::make_shared<TestReadQueue>(),
                                  .tombstone_cleanup = [&](const std::filesystem::path& path) {
                                      if (cleanup_throws) {
                                          throw std::runtime_error("injected cleanup outage");
                                      }
                                      std::error_code error;
                                      std::filesystem::remove_all(path, error);
                                      return !error;
                                  }});
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096, .payload_bytes = payload.size()});
    };
    int failures = 0;
    const std::filesystem::path session =
        temporary.path / "sessions" / responses.client_session_sha256;

    const auto first = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(first.has_value(), "baseline generation saves");
    if (!first) { return failures; }

    cleanup_throws  = true;
    const auto second = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(second.has_value() && second->generation != first->generation,
                      "save is acknowledged even though post-publish reclamation throws");
    if (!second) { return failures; }
    failures += check(std::filesystem::exists(session / "generations" / second->generation),
                      "published successor generation is durable");
    SessionCheckpointLoadResult loaded = store.load(responses.client_session_sha256, fingerprint());
    failures += check(loaded.state == SessionCheckpointLoadState::Available &&
                          loaded.checkpoint.has_value() &&
                          loaded.checkpoint->generation == second->generation,
                      "store serves the acknowledged successor after the cleanup outage");
    loaded.checkpoint.reset();

    // While cleanup keeps failing, the tolerance is withdrawn: the next save must refuse
    // instead of letting usage creep past the cap one tolerated save at a time.
    ninfer::runtime::SessionCheckpointSkipDetail skip;
    const auto third = store.save(responses, fingerprint(), exporter, publish_fixture, &skip);
    failures += check(!third.has_value() &&
                          skip.reason ==
                              ninfer::runtime::SessionCheckpointSkipReason::QuotaExceeded,
                      "unhealthy reclamation withdraws the transient tolerance fail-closed");

    // Once the outage clears, the same save succeeds again and the store converges.
    cleanup_throws   = false;
    const auto fourth = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(fourth.has_value(),
                      "save recovers once reclamation is healthy again");
    return failures;
}

int test_native_read_queue_is_required() {
    TemporaryDirectory temporary;
    bool missing_rejected = false;
    try {
        SessionCheckpointStore missing(
            {.root = temporary.path, .disk_quota_bytes = 1ULL << 20, .staging_bytes = 1ULL << 20});
    } catch (const std::invalid_argument&) { missing_rejected = true; }

    auto unavailable          = std::make_shared<TestReadQueue>();
    unavailable->is_available = false;
    unavailable->reason       = "injected unavailable queue";
    bool unavailable_rejected = false;
    try {
        SessionCheckpointStore rejected({.root             = temporary.path,
                                         .disk_quota_bytes = 1ULL << 20,
                                         .staging_bytes    = 1ULL << 20,
                                         .read_queue       = unavailable});
    } catch (const std::invalid_argument& error) {
        unavailable_rejected =
            std::string_view(error.what()).find(unavailable->reason) != std::string_view::npos;
    }
    return check(missing_rejected && unavailable_rejected,
                 "checkpoint store accepted a missing or unavailable native read queue");
}

int test_post_verification_replacement_fails_closed() {
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload  = engine_payload();
    SessionCheckpointStore store({.root             = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes    = 1ULL << 20,
                                  .read_queue       = std::make_shared<TestReadQueue>()});
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        const std::string metadata = "engine-metadata-v3";
        if (!write_chunked(writer, "engine/index.cbor",
                           std::as_bytes(std::span(metadata.data(), metadata.size()))) ||
            !write_chunked(writer, "engine/state/0.bin", payload)) {
            return DurableExportResult{.state = runtime::DurableExportState::Refused};
        }
        return exported_fixture({.frontier_tokens = 100000,
                                           .restored_tokens = 97500,
                                           .payload_bytes   = payload.size() + metadata.size()});
    };
    int failures     = 0;
    const auto saved = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(saved.has_value(), "tamper fixture publishes");
    if (!saved) { return failures; }

    // alphastorm/ninfer#21: every backend read reopens the pathname, so the bytes the engine
    // receives are the bytes that get hashed. A same-size replacement inside the checkpoint root
    // after load must fail that streamed hash instead of feeding the engine.
    SessionCheckpointLoadResult loaded = store.load(
        responses.client_session_sha256, fingerprint(), responses.latest_response_id);
    failures += check(loaded.state == SessionCheckpointLoadState::Available &&
                          loaded.checkpoint.has_value(),
                      "tamper fixture loads before replacement");
    if (!loaded.checkpoint) { return failures; }
    std::vector<std::byte> tampered = payload;
    tampered.front() ^= std::byte{0x5a};
    const std::filesystem::path state_path = temporary.path / "sessions" /
                                             responses.client_session_sha256 / "generations" /
                                             saved->generation / "engine" / "state" / "0.bin";
    {
        std::ofstream swap(state_path, std::ios::binary | std::ios::trunc);
        swap.write(reinterpret_cast<const char*>(tampered.data()),
                   static_cast<std::streamsize>(tampered.size()));
    }
    std::vector<std::byte> victim(payload.size());
    failures += check(!loaded.checkpoint->engine->read_file("engine/state/0.bin", 0, victim),
                      "post-verification replacement fails the streamed re-hash closed");
    failures += check(loaded.checkpoint->engine->failure_reason() ==
                          runtime::DurableReason::PayloadDigestMismatch,
                      "delivered-byte mismatch reports integrity, not transient I/O");
    failures += check(!loaded.checkpoint->engine->read_file("engine/state/0.bin", 0, victim),
                      "a failed payload stays refused for the reader's lifetime");
    std::vector<std::byte> skewed(1);
    failures += check(!loaded.checkpoint->engine->read_file("engine/index.cbor", 1, skewed),
                      "out-of-sequence coverage is refused");
    std::vector<std::byte> intact(18);
    failures += check(loaded.checkpoint->engine->read_file("engine/index.cbor", 0, intact),
                      "untouched payloads keep reading through the same reader");
    loaded.checkpoint.reset();

    // Council CR-20260831-fanout43 R3: the streamed mismatch is corruption evidence, not a
    // transient read error. Status must stop advertising the generation, and the next load
    // must quarantine it instead of re-verifying the tampered bytes as current.
    const nlohmann::json seen = store.status(responses.client_session_sha256, fingerprint());
    failures += check(seen.at("state") == "corrupt",
                      "status reports the streamed mismatch before any new load");
    const SessionCheckpointLoadResult after_tamper = store.load(
        responses.client_session_sha256, fingerprint(), responses.latest_response_id);
    failures += check(after_tamper.state == SessionCheckpointLoadState::Corrupt &&
                          !after_tamper.checkpoint.has_value(),
                      "the corrupted generation is quarantined on the next load");
    const nlohmann::json gone = store.status(responses.client_session_sha256, fingerprint());
    failures += check(gone.at("state") == "missing",
                      "quarantine removes the corrupt generation from the current pointer");
    return failures;
}

int test_buffered_write_backpressure_round_trips() {
    // ninfer#34: exporter writes flow through a bounded in-memory queue drained off the
    // engine lock. A capacity of one byte forces every chunk through the oversized-chunk
    // escape and the backpressure wait; the published bytes must stay identical.
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload  = engine_payload();
    SessionCheckpointStore store({.root               = temporary.path,
                                  .disk_quota_bytes   = 8ULL << 20,
                                  .staging_bytes      = 1ULL << 20,
                                  .write_buffer_bytes = 1,
                                  .read_queue         = std::make_shared<TestReadQueue>()});
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        const std::string metadata = "engine-metadata-v3";
        if (!write_chunked(writer, "engine/index.cbor",
                           std::as_bytes(std::span(metadata.data(), metadata.size()))) ||
            !write_chunked(writer, "engine/state/0.bin", payload)) {
            return DurableExportResult{.state = runtime::DurableExportState::Refused};
        }
        return exported_fixture({.frontier_tokens = 100000,
                                           .restored_tokens = 97500,
                                           .payload_bytes   = payload.size() + metadata.size()});
    };
    int failures     = 0;
    const auto saved = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(saved.has_value(), "buffered save publishes under byte-level backpressure");
    if (!saved) { return failures; }
    SessionCheckpointLoadResult loaded = store.load(
        responses.client_session_sha256, fingerprint(), responses.latest_response_id);
    failures += check(loaded.state == SessionCheckpointLoadState::Available &&
                          loaded.checkpoint.has_value(),
                      "buffered save round-trips through verification");
    if (!loaded.checkpoint) { return failures; }
    std::vector<std::byte> restored(payload.size());
    failures += check(loaded.checkpoint->engine->read_file("engine/state/0.bin", 0, restored) &&
                          restored == payload,
                      "buffered writes preserve exact payload bytes");
    failures += check(store.covers(responses.client_session_sha256, fingerprint(),
                                   responses.latest_response_id),
                      "catalogued generation covers its own newest response");
    failures += check(!store.covers(responses.client_session_sha256, fingerprint(),
                                    "resp_not_yet_checkpointed"),
                      "an uncheckpointed response id is not covered");
    loaded.checkpoint.reset();
    return failures;
}

int test_buffered_deferred_failure_fails_before_publication() {
    // Review CR-20260831-ckptdecouple R4/U2: a deferred disk write that fails in the drain
    // thread - after the buffered writer already acknowledged the chunk to the exporter -
    // must fail the save at finish() and leave the prior published generation untouched.
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload  = engine_payload();
    SessionCheckpointStore store({.root             = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes    = 1ULL << 20,
                                  .read_queue       = std::make_shared<TestReadQueue>()});
    int failures = 0;
    const auto good =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 100000,
                                           .restored_tokens = 97500,
                                           .payload_bytes   = payload.size()});
    };
    failures += check(store.save(responses, fingerprint(), good, publish_fixture).has_value(),
                      "baseline generation publishes");
    const auto poisoned =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        (void)write_state_fixture(writer, payload);
        // Acknowledged at enqueue time, rejected later by the drain-side writer
        // (offset beyond total). The exporter deliberately ignores every return value
        // and reports success, so only finish() can stop publication.
        const std::string tail = "poison";
        (void)writer.write_file("engine/state/1.bin", 8, 4, std::as_bytes(std::span(tail.data(), tail.size())), 1);
        return exported_fixture({.frontier_tokens = 100001,
                                           .restored_tokens = 97501,
                                           .payload_bytes   = payload.size()});
    };
    bool poisoned_save_failed = false;
    try {
        poisoned_save_failed = !store.save(responses, fingerprint(), poisoned, publish_fixture).has_value();
    } catch (const std::exception&) {
        // save() rethrows I/O failures after cleaning its staging; either signal is the
        // required "failed before publication" outcome.
        poisoned_save_failed = true;
    }
    failures += check(poisoned_save_failed,
                      "deferred drain failure fails the save despite exporter-reported success");
    SessionCheckpointLoadResult loaded = store.load(
        responses.client_session_sha256, fingerprint(), responses.latest_response_id);
    failures += check(loaded.state == SessionCheckpointLoadState::Available &&
                          loaded.checkpoint.has_value(),
                      "prior published generation survives the failed save");
    if (!loaded.checkpoint) { return failures; }
    std::vector<std::byte> restored(payload.size());
    failures += check(loaded.checkpoint->engine->read_file("engine/state/0.bin", 0, restored) &&
                          restored == payload,
                      "surviving generation still serves the baseline bytes");
    loaded.checkpoint.reset();
    return failures;
}

#if defined(_WIN32) || defined(__linux__)
int test_disk_full_capacity_preserves_generation() {
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .origin_mac_key = std::string(32, 'k'),
                                  .read_queue = std::make_shared<TestReadQueue>()});
    const auto exporter = [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) {
            return DurableExportResult{.state = runtime::DurableExportState::Refused};
        }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                  .payload_bytes = payload.size()});
    };
    const auto baseline = store.save(responses, fingerprint(), exporter, publish_fixture);
    if (!baseline) { return check(false, "disk-full baseline did not publish"); }
    struct Fault {
        int generic;
        unsigned long windows;
        runtime::DurableReason reason;
    };
    const Fault faults[] = {
        {ENOSPC, 0, runtime::DurableReason::Capacity},
#ifndef _WIN32
        {EDQUOT, 0, runtime::DurableReason::Capacity},
#endif
        {EIO, 0, runtime::DurableReason::IoUnavailable},
#ifdef _WIN32
        {0, ERROR_DISK_FULL, runtime::DurableReason::Capacity},
        {0, ERROR_HANDLE_DISK_FULL, runtime::DurableReason::Capacity},
        {0, ERROR_DISK_QUOTA_EXCEEDED, runtime::DurableReason::Capacity},
        {0, ERROR_ACCESS_DENIED, runtime::DurableReason::IoUnavailable},
#endif
    };
    int failures = 0;
    for (const auto& fault : faults) {
        runtime::SessionCheckpointSkipDetail skip;
        bool threw = false, published = false;
        std::optional<SessionCheckpointSaveResult> saved;
        std::string diagnostic;
        {
            CheckpointLogCapture log;
            checkpoint_write_fault::Scope injection(0);
            try {
                saved = store.save(responses, fingerprint(),
                    [&](ContinuationCheckpointWriter& writer) {
                        // The Responses file is already durable. Fail in the native drain,
                        // after enqueue acknowledgement, and deliberately return a coarse
                        // exporter reason: it must not erase the first disk-capacity cause.
                        checkpoint_write_fault::generic_error = fault.generic;
                        checkpoint_write_fault::windows_error = fault.windows;
                        (void)write_state_fixture(writer, payload);
                        return DurableExportResult{.state = runtime::DurableExportState::Refused,
                                                    .reason = runtime::DurableReason::IoUnavailable};
                    },
                    [&](runtime::DurableSessionEpoch epoch, const std::function<bool()>& commit) {
                        published = true;
                        return publish_fixture(epoch, commit);
                    }, &skip);
            } catch (...) { threw = true; }
            diagnostic = log.output.str();
        }
        failures += check(checkpoint_write_fault::fired != 0 && !saved && !published &&
                              skip.durable_reason == fault.reason &&
                              (fault.reason != runtime::DurableReason::Capacity || !threw),
                          "disk failure lost its exact reason or reached publication");
        const auto record = nlohmann::json::parse(diagnostic);
        failures += check(record.at("operation") == "payload_write" &&
                              std::string_view(record.at("reason").get_ref<const std::string&>()) ==
                                  runtime::durable_reason_name(fault.reason) &&
                              record.at("error_category") == (fault.generic ? "generic" : "system") &&
                              record.at("error_code") == (fault.generic ? fault.generic : fault.windows),
                          "disk-failure diagnostic lost its capacity classification or OS code");
        auto loaded = store.load(responses.client_session_sha256, fingerprint());
        failures += check(loaded.state == SessionCheckpointLoadState::Available &&
                              loaded.checkpoint && loaded.checkpoint->generation == baseline->generation,
                          "disk-full refusal replaced or quarantined the prior generation");
        if (loaded.checkpoint) {
            std::vector<std::byte> bytes(payload.size());
            failures += check(loaded.checkpoint->engine->read_file("engine/state/0.bin", 0, bytes) &&
                                  loaded.checkpoint->engine->verify_complete() && bytes == payload,
                              "disk-full refusal damaged the prior generation's payload");
        }
    }
    return failures;
}

// omp-ninfer#89 L6: manifest, MAC and current-pointer writes carry the OS code of a failed
// close, so a disk-full or quota-exhausted close is the ordinary Capacity refusal and any
// other error stays a thrown IoUnavailable system_error. Each failure precedes the current
// rename, so neither the attempted generation nor its pointer file may remain (L8).
int test_metadata_close_failure_keeps_os_classification() {
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .origin_mac_key = std::string(32, 'k'),
                                  .read_queue = std::make_shared<TestReadQueue>()});
    const auto exporter = [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) {
            return DurableExportResult{.state = runtime::DurableExportState::Refused};
        }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                  .payload_bytes = payload.size()});
    };
    const auto baseline = store.save(responses, fingerprint(), exporter, publish_fixture);
    if (!baseline) { return check(false, "metadata-close baseline did not publish"); }
    const auto session = temporary.path / "sessions" / responses.client_session_sha256;
    struct Fault {
        const char* file;
        int error;
        runtime::DurableReason reason;
    };
    const Fault faults[] = {
        {"manifest.json", ENOSPC, runtime::DurableReason::Capacity},
        {"manifest.mac", ENOSPC, runtime::DurableReason::Capacity},
        {".current-", ENOSPC, runtime::DurableReason::Capacity},
#ifndef _WIN32
        {"manifest.json", EDQUOT, runtime::DurableReason::Capacity},
#endif
        {"manifest.json", EIO, runtime::DurableReason::IoUnavailable},
        {".current-", EIO, runtime::DurableReason::IoUnavailable},
    };
    int failures = 0;
    for (const auto& fault : faults) {
        runtime::SessionCheckpointSkipDetail skip;
        std::optional<SessionCheckpointSaveResult> saved;
        std::error_code thrown;
        bool threw_other = false;
        unsigned refused_closes = 0;
        {
            checkpoint_write_fault::CloseScope injection(fault.error, fault.file);
            try {
                saved = store.save(responses, fingerprint(), exporter, publish_fixture, &skip);
            } catch (const std::system_error& error) {
                thrown = error.code();
            } catch (...) { threw_other = true; }
            refused_closes = checkpoint_write_fault::fired;
        }
        const std::string label =
            std::string(fault.file) + " close errno " + std::to_string(fault.error);
        const bool capacity = fault.reason == runtime::DurableReason::Capacity;
        failures += check(refused_closes == 1 && !saved && !threw_other &&
                              skip.durable_reason == fault.reason &&
                              (capacity ? !thrown
                                        : thrown == std::error_code(fault.error,
                                                                    std::generic_category())),
                          label + " lost the OS classification of the failed close");
        failures += check(entry_names(session / "generations") ==
                                  std::vector<std::string>{baseline->generation} &&
                              entry_names(session) ==
                                  std::vector<std::string>{"current", "generations"},
                          label + " left the unpublished generation or its pointer file");
        const auto loaded = store.load(responses.client_session_sha256, fingerprint());
        failures += check(loaded.state == SessionCheckpointLoadState::Available &&
                              loaded.checkpoint &&
                              loaded.checkpoint->generation == baseline->generation,
                          label + " replaced the published generation");
    }
    return failures;
}
#endif

// omp-ninfer#89 L8: only the current-pointer rename makes a generation visible. A failure
// replace_path reports before attempting that rename leaves current untouched, so the save
// discards the attempted generation and its pointer file at once rather than leaving a stale
// generation for quota pressure. Once the rename was attempted the generation is retained
// for reconciliation.
int test_pointer_rename_attempt_decides_generation_retention() {
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .read_queue = std::make_shared<TestReadQueue>()});
    const auto exporter = [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                  .payload_bytes = payload.size()});
    };
    const auto baseline = store.save(responses, fingerprint(), exporter, publish_fixture);
    if (!baseline) { return check(false, "pointer-rename baseline did not publish"); }
    const auto session = temporary.path / "sessions" / responses.client_session_sha256;
    runtime::CheckpointControl control;
    control.session(responses.client_session_sha256)->tag = responses.latest_response_id;
    // The real epoch gate turns a publication exception into Refused/IoUnavailable.
    const auto gate = [&](std::function<void()> before_commit) {
        return [&, before_commit](runtime::DurableSessionEpoch epoch,
                                  const std::function<bool()>& commit) {
            before_commit();
            return control.publish(responses.client_session_sha256, responses.latest_response_id,
                                   epoch, commit);
        };
    };
    int failures = 0;

    // The staged pointer vanishes, so replace_path refuses its source before any rename.
    runtime::SessionCheckpointSkipDetail refused_skip;
    const auto refused = store.save(responses, fingerprint(), exporter, gate([&] {
        for (const auto& name : entry_names(session)) {
            if (name.starts_with(".current-")) { std::filesystem::remove(session / name); }
        }
    }), &refused_skip);
    failures += check(!refused && refused_skip.durable_reason == runtime::DurableReason::IoUnavailable,
                      "pre-rename publication failure was acknowledged or lost its reason");
    failures += check(entry_names(session / "generations") ==
                              std::vector<std::string>{baseline->generation} &&
                          entry_names(session) == std::vector<std::string>{"current", "generations"},
                      "pre-rename publication failure retained the unpublished generation");
    {
        const auto loaded = store.load(responses.client_session_sha256, fingerprint());
        failures += check(loaded.state == SessionCheckpointLoadState::Available &&
                              loaded.checkpoint &&
                              loaded.checkpoint->generation == baseline->generation,
                          "pre-rename publication failure changed the published generation");
    }

    // An occupied directory named current makes the attempted rename itself fail.
    std::filesystem::remove(session / "current");
    std::filesystem::create_directories(session / "current" / "occupied");
    const auto before = entry_names(session / "generations");
    runtime::SessionCheckpointSkipDetail attempted_skip;
    const auto attempted = store.save(responses, fingerprint(), exporter, gate([] {}),
                                      &attempted_skip);
    const auto retained = entry_names(session / "generations");
    failures += check(!attempted &&
                          attempted_skip.durable_reason == runtime::DurableReason::IoUnavailable &&
                          retained.size() == before.size() + 1 &&
                          std::includes(retained.begin(), retained.end(), before.begin(),
                                        before.end()) &&
                          entry_names(session) == std::vector<std::string>{"current", "generations"},
                      "attempted current rename did not retain its possibly visible generation");
    return failures;
}

// A quarantine requested while the save lease still pins its just-published generation only
// detaches current; the lease finishes it on release. That release follows the save's
// return, so the lease owns the generation name instead of borrowing the save's.
int test_save_lease_finishes_quarantine_requested_after_publication() {
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .read_queue = std::make_shared<TestReadQueue>()});
    const auto exporter = [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                  .payload_bytes = payload.size()});
    };
    const auto generations =
        temporary.path / "sessions" / responses.client_session_sha256 / "generations";
    std::string published;
    bool deferred = false;
    const auto saved = store.save(responses, fingerprint(), exporter,
        [&](runtime::DurableSessionEpoch epoch, const std::function<bool()>& commit) {
            const auto result = publish_fixture(epoch, commit);
            // Corruption evidence for the generation current now names, while the lease pins it.
            const auto names = entry_names(generations);
            published = names.size() == 1 ? names.front() : std::string();
            deferred = store.quarantine(responses.client_session_sha256, published,
                                        runtime::DurableReason::PayloadDigestMismatch) &&
                       store.status(responses.client_session_sha256, fingerprint())
                               .at("state") == "missing" &&
                       entry_names(generations) == std::vector<std::string>{published};
            return result;
        });
    int failures = check(saved && saved->generation == published && deferred,
                         "pinned published generation did not defer its quarantine");
    const auto names = entry_names(generations);
    failures += check(names.size() == 1 && names.front().starts_with(published + ".corrupt-"),
                      "save lease release skipped the quarantine deferred for its generation");
    return failures;
}

// directory_bytes supplies both store usage and each reclamation candidate's bytes, so no
// substitute size is safe for an entry whose status cannot be determined. Quota accounting
// fails closed with the OS code instead of skipping the entry; saving resumes once it can.
int test_unmeasurable_generation_entry_fails_quota_closed() {
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .read_queue = std::make_shared<TestReadQueue>()});
    const auto exporter = [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                  .payload_bytes = payload.size()});
    };
    const auto baseline = store.save(responses, fingerprint(), exporter, publish_fixture);
    if (!baseline) { return check(false, "unmeasurable-entry baseline did not publish"); }
    const auto root = stored_generation_root(temporary.path, responses, baseline->generation);
    // The link exists, but the status of what it names cannot be determined.
    std::filesystem::create_symlink("loop", root / "loop");
    std::error_code scanned;
    try {
        store.collect_garbage();
    } catch (const std::system_error& error) { scanned = error.code(); }
#ifdef _WIN32
    const bool reported = scanned && scanned.category() == std::system_category();
#else
    const bool reported = scanned == std::errc::too_many_symbolic_link_levels;
#endif
    int failures = check(reported, "quota scan skipped an entry whose status is unavailable");
    bool refused = false;
    std::optional<SessionCheckpointSaveResult> successor;
    try {
        successor = store.save(responses, fingerprint(), exporter, publish_fixture);
    } catch (const std::system_error&) { refused = true; }
    failures += check(refused && !successor &&
                          entry_names(root.parent_path()) ==
                              std::vector<std::string>{baseline->generation},
                      "save published over a store whose usage it could not measure");
    std::filesystem::remove(root / "loop");
    failures += check(store.save(responses, fingerprint(), exporter, publish_fixture).has_value(),
                      "measurable store did not resume saving");
    return failures;
}


int test_hmac_sha256_rfc4231_vectors() {
    int failures = 0;
    const std::string key1(20, static_cast<char>(0x0b));
    const std::string msg1 = "Hi There";
    failures += check(crypto::sha256_hex(crypto::hmac_sha256(
                          std::as_bytes(std::span(key1.data(), key1.size())),
                          std::as_bytes(std::span(msg1.data(), msg1.size())))) ==
                          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
                      "HMAC-SHA256 matches RFC 4231 test case 1");
    const std::string key2 = "Jefe";
    const std::string msg2 = "what do ya want for nothing?";
    failures += check(crypto::sha256_hex(crypto::hmac_sha256(
                          std::as_bytes(std::span(key2.data(), key2.size())),
                          std::as_bytes(std::span(msg2.data(), msg2.size())))) ==
                          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
                      "HMAC-SHA256 matches RFC 4231 test case 2");
    return failures;
}

int test_manifest_origin_authentication() {
    // alphastorm/ninfer#32: the digest chain authenticates consistency; the MAC keyed outside
    // the checkpoint root authenticates origin. A coherent in-root rewrite must not survive.
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload  = engine_payload();
    const std::string mac_key(32, static_cast<char>(0x42));
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 100000,
                                           .restored_tokens = 97500,
                                           .payload_bytes   = payload.size()});
    };
    int failures = 0;
    std::string generation;
    {
        SessionCheckpointStore store({.root             = temporary.path,
                                      .disk_quota_bytes = 8ULL << 20,
                                      .staging_bytes    = 1ULL << 20,
                                      .origin_mac_key   = mac_key,
                                      .read_queue       = std::make_shared<TestReadQueue>()});
        const auto saved = store.save(responses, fingerprint(), exporter, publish_fixture);
        failures += check(saved.has_value(), "keyed save publishes");
        if (!saved) { return failures; }
        generation = saved->generation;
        const std::filesystem::path root = stored_generation_root(temporary.path, responses,
                                                                  generation);
        failures += check(std::filesystem::is_regular_file(root / "manifest.mac"),
                          "save writes the origin MAC beside the manifest");
        SessionCheckpointLoadResult loaded = store.load(
            responses.client_session_sha256, fingerprint(), responses.latest_response_id);
        failures += check(loaded.state == SessionCheckpointLoadState::Available,
                          "authentic generation loads under its own key");
        failures += check(loaded.reason == runtime::DurableReason::None &&
                              loaded.checkpoint && loaded.checkpoint->origin_authenticated &&
                              saved->origin_authenticated,
                          "authenticated publication and load retain exact origin evidence");
        loaded.checkpoint.reset();
        failures += check(store.covers(responses.client_session_sha256, fingerprint(),
                                       responses.latest_response_id),
                          "authentic generation covers its newest response");
    }
    const std::filesystem::path root =
        stored_generation_root(temporary.path, responses, generation);
    {
        SessionCheckpointStore no_key({.root = temporary.path,
                                       .disk_quota_bytes = 8ULL << 20,
                                       .staging_bytes = 1ULL << 20,
                                       .read_queue = std::make_shared<TestReadQueue>()});
        const auto unavailable = no_key.load(responses.client_session_sha256, fingerprint());
        failures += check(unavailable.state == SessionCheckpointLoadState::Unavailable &&
                              unavailable.reason == runtime::DurableReason::OriginKeyUnavailable &&
                              std::filesystem::is_regular_file(root / "manifest.json"),
                          "present MAC without a verification key preserves the generation");
    }
    {
        // Same root, different key: a valid-looking store produced elsewhere is forged here.
        SessionCheckpointStore foreign({.root             = temporary.path,
                                        .disk_quota_bytes = 8ULL << 20,
                                        .staging_bytes    = 1ULL << 20,
                                        .origin_mac_key   = std::string(32, static_cast<char>(0x43)),
                                        .read_queue       = std::make_shared<TestReadQueue>()});
        failures += check(!foreign.covers(responses.client_session_sha256, fingerprint(),
                                          responses.latest_response_id),
                          "a skip decision never trusts a foreign-keyed catalogue");
        const SessionCheckpointLoadResult loaded = foreign.load(
            responses.client_session_sha256, fingerprint(), responses.latest_response_id);
        failures += check(loaded.state == SessionCheckpointLoadState::Corrupt &&
                              loaded.reason == runtime::DurableReason::OriginMacMismatch,
                          "foreign-keyed generation quarantines as tampering");
    }
    return failures;
}

int test_manifest_origin_compatibility_window() {
    // Legacy generations without a MAC load through the window, strict mode ends it, and a
    // coherent manifest rewrite without the key fails even when its digests are consistent.
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload  = engine_payload();
    const std::string mac_key(32, static_cast<char>(0x42));
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 100000,
                                           .restored_tokens = 97500,
                                           .payload_bytes   = payload.size()});
    };
    int failures = 0;
    std::string generation;
    {
        // Legacy producer: no key configured, no MAC written.
        SessionCheckpointStore legacy({.root             = temporary.path,
                                       .disk_quota_bytes = 8ULL << 20,
                                       .staging_bytes    = 1ULL << 20,
                                       .read_queue       = std::make_shared<TestReadQueue>()});
        const auto saved = legacy.save(responses, fingerprint(), exporter, publish_fixture);
        failures += check(saved.has_value(), "legacy save publishes without a MAC");
        if (!saved) { return failures; }
        generation = saved->generation;
    }
    const std::filesystem::path root =
        stored_generation_root(temporary.path, responses, generation);
    failures += check(!std::filesystem::exists(root / "manifest.mac"),
                      "legacy generation has no origin MAC");
    {
        SessionCheckpointStore window({.root             = temporary.path,
                                       .disk_quota_bytes = 8ULL << 20,
                                       .staging_bytes    = 1ULL << 20,
                                       .origin_mac_key   = mac_key,
                                       .read_queue       = std::make_shared<TestReadQueue>()});
        SessionCheckpointLoadResult loaded = window.load(
            responses.client_session_sha256, fingerprint(), responses.latest_response_id);
        failures += check(loaded.state == SessionCheckpointLoadState::Available,
                          "unMAC'd legacy generation loads through the compatibility window");
        loaded.checkpoint.reset();
    }
    {
        SessionCheckpointStore strict({.root               = temporary.path,
                                       .disk_quota_bytes   = 8ULL << 20,
                                       .staging_bytes      = 1ULL << 20,
                                       .origin_mac_key     = mac_key,
                                       .require_origin_auth = true,
                                       .read_queue         = std::make_shared<TestReadQueue>()});
        // Strict refusal is a reversible policy denial: Incompatible (clean-replay path),
        // never quarantine (council CR-20260831-originauth grok R2).
        const SessionCheckpointLoadResult refused = strict.load(
            responses.client_session_sha256, fingerprint(), responses.latest_response_id);
        failures += check(refused.state == SessionCheckpointLoadState::Incompatible &&
                              refused.reason == runtime::DurableReason::OriginRequired,
                          "strict origin auth refuses reversibly without quarantining");
        failures += check(std::filesystem::is_regular_file(root / "manifest.json"),
                          "strict refusal leaves the generation untouched");
    }
    {
        // Clearing the flag restores the generation - the refusal was policy, not evidence.
        SessionCheckpointStore window_again({.root             = temporary.path,
                                             .disk_quota_bytes = 8ULL << 20,
                                             .staging_bytes    = 1ULL << 20,
                                             .origin_mac_key   = mac_key,
                                             .read_queue       = std::make_shared<TestReadQueue>()});
        SessionCheckpointLoadResult restored = window_again.load(
            responses.client_session_sha256, fingerprint(), responses.latest_response_id);
        failures += check(restored.state == SessionCheckpointLoadState::Available,
                          "clearing strict mode restores the window-era generation");
        restored.checkpoint.reset();
    }
    return failures;
}

int test_quota_and_erase_refuse_ancestor_symlink_swaps() {
    TemporaryDirectory temporary;
    TemporaryDirectory outside;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .read_queue = std::make_shared<TestReadQueue>()});
    const auto exporter = [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) {
            return DurableExportResult{.state = runtime::DurableExportState::Refused,
                                        .reason = runtime::DurableReason::IoUnavailable};
        }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                  .payload_bytes = payload.size()});
    };
    const auto saved = store.save(responses, fingerprint(), exporter, publish_fixture);
    int failures = check(saved.has_value(), "ancestor-swap fixture publishes");
    if (!saved) { return failures; }
    const std::string marker = "outside-root sentinel";
    const auto outside_file = outside.path / "sentinel.txt";
    std::ofstream(outside_file, std::ios::binary) << marker;

    const auto tombstones = temporary.path / ".tombstones";
    const auto held_tombstones = temporary.path / ".tombstones-held";
    std::filesystem::rename(tombstones, held_tombstones);
    std::filesystem::create_directory_symlink(outside.path, tombstones);
    try {
        store.collect_garbage();
    } catch (const std::exception&) {
        // A rejected inventory is also fail-closed; no outside entry may be reclaimed.
    }
    failures += check(read_file_bytes(outside_file) == marker,
                      "tombstone ancestor swap cannot make GC remove outside-root data");
    std::filesystem::remove(tombstones);
    std::filesystem::rename(held_tombstones, tombstones);

    const auto external_session = outside.path / responses.client_session_sha256;
    std::filesystem::create_directory(external_session);
    std::ofstream(external_session / "current", std::ios::binary) << marker;
    const auto sessions = temporary.path / "sessions";
    const auto held_sessions = temporary.path / "sessions-held";
    std::filesystem::rename(sessions, held_sessions);
    std::filesystem::create_directory_symlink(outside.path, sessions);
    failures += check(store.erase(responses.client_session_sha256) ==
                              SessionCheckpointEraseResult::Conflict &&
                          read_file_bytes(external_session / "current") == marker,
                      "session ancestor swap cannot rename or erase an outside-root lineage");
    std::filesystem::remove(sessions);
    std::filesystem::rename(held_sessions, sessions);
    failures += check(store.status(responses.client_session_sha256, fingerprint()).at("state") ==
                          "available",
                      "refused path mutations preserve the original current for retry");
    return failures;
}

int test_v3_manifest_validation_and_publication_guard() {
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .read_queue = std::make_shared<TestReadQueue>()});
    const auto exporter = [&](ContinuationCheckpointWriter& writer) {
        if (!writer.write_file("engine/index.cbor", 0, 0, {}, 1) ||
            !writer.write_file("engine/state/0.bin", 0, payload.size(), payload, 8192)) {
            return DurableExportResult{.state = runtime::DurableExportState::Refused,
                                        .reason = runtime::DurableReason::IoUnavailable};
        }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                  .payload_bytes = payload.size()});
    };
    int failures = 0;
    const std::vector<std::function<std::string(nlohmann::json)>> invalid = {
        [](auto manifest) { manifest["unexpected"] = true; return manifest.dump(); },
        [](auto manifest) { manifest["files"][0].erase("alignment"); return manifest.dump(); },
        [](auto manifest) { manifest["files"][0]["alignment"] = 3; return manifest.dump(); },
        [](auto manifest) { manifest["files"][0]["alignment"] = 8192; return manifest.dump(); },
        [](auto manifest) {
            manifest["frontier_tokens"] = 1ULL << 32;
            return manifest.dump();
        },
        [](auto manifest) {
            manifest["files"][2]["path"] = "engine/state/00.bin";
            return manifest.dump();
        },
        [](auto manifest) { manifest["files"].erase(1); return manifest.dump(); },
        [](auto manifest) {
            manifest["frontier_tokens"] = 4096.0;
            return manifest.dump();
        },
        [](auto manifest) { return " " + manifest.dump(); },
        [](auto manifest) {
            auto text = manifest.dump();
            text.insert(1, "\"schema_version\":3,");
            return text;
        },
    };
    for (const auto& mutate : invalid) {
        const auto saved = store.save(responses, fingerprint(), exporter, publish_fixture);
        failures += check(saved.has_value(), "canonical manifest mutation fixture publishes");
        if (!saved) { return failures; }
        const auto root = stored_generation_root(temporary.path, responses, saved->generation);
        const auto path = root / "manifest.json";
        const auto replacement = mutate(nlohmann::json::parse(read_file_bytes(path)));
        std::ofstream(path, std::ios::binary | std::ios::trunc) << replacement;
        failures += check(store.load(responses.client_session_sha256, fingerprint()).state ==
                              SessionCheckpointLoadState::Corrupt,
                          "noncanonical or invalid v3 manifest is quarantined before import");
    }
    const auto saved = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(saved.has_value(), "aligned descriptor fixture publishes");
    if (!saved) { return failures; }
    auto loaded = store.load(responses.client_session_sha256, fingerprint());
    if (!loaded.checkpoint) {
        return failures + check(false, "aligned descriptor fixture loads");
    }
    const auto descriptor = loaded.checkpoint->engine->file_descriptor("engine/state/0.bin");
    failures += check(descriptor && descriptor->alignment == 8192 &&
                          descriptor->bytes == payload.size() &&
                          descriptor->sha256 == crypto::sha256_hex(crypto::sha256(payload)),
                      "reader exposes exact native-index descriptor agreement");
    loaded.checkpoint.reset();

    runtime::SessionCheckpointSkipDetail skip;
    bool attempted_publish = false;
    const auto stale = store.save(
        responses, fingerprint(), exporter,
        [&](runtime::DurableSessionEpoch, const std::function<bool()>&) {
            attempted_publish = true;
            return runtime::DurablePublicationResult{.state = runtime::DurableExportState::Stale,
                                                     .reason = runtime::DurableReason::StalePublication};
        }, &skip);
    failures += check(!stale && attempted_publish &&
                          skip.export_state == runtime::DurableExportState::Stale &&
                          skip.durable_reason == runtime::DurableReason::StalePublication &&
                          store.status(responses.client_session_sha256, fingerprint())
                                  .at("generation") == saved->generation,
                      "final epoch refusal preserves prior current and exact stale reason");
    const auto message = format_session_checkpoint_skip(
        "checkpoint save refused: ", responses.client_session_sha256, skip);
    failures += check(message.view().find("stale_publication") != std::string_view::npos,
                      "bounded save diagnostics render the authoritative K7 refusal");
    const auto root = stored_generation_root(temporary.path, responses, saved->generation);
    auto checksum_text = read_file_bytes(root / "checksums.json");
    checksum_text.front() ^= 1;
    std::ofstream(root / "checksums.json", std::ios::binary | std::ios::trunc) << checksum_text;
    failures += check(store.load(responses.client_session_sha256, fingerprint()).state ==
                          SessionCheckpointLoadState::Corrupt,
                      "checksum inventory bytes are authenticated against the manifest");
    return failures;
}

int test_schema_v3_and_stream_completion() {
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .read_queue = std::make_shared<TestReadQueue>()});
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 4096,
                                           .restored_tokens = 4096,
                                           .payload_bytes = payload.size()});
    };
    const auto saved = store.save(responses, fingerprint(), exporter, publish_fixture);
    int failures = check(saved.has_value(), "v3 stream-completion fixture publishes");
    if (!saved) { return failures; }
#ifdef _WIN32
    failures += check(saved->dir_sync == "ntfs-write-through",
                      "Windows acknowledgement reports write-through, not directory fsync");
#else
    failures += check(saved->dir_sync == "fsync",
                      "POSIX acknowledgement reports directory fsync");
#endif
    auto loaded = store.load(responses.client_session_sha256, fingerprint());
    failures += check(loaded.checkpoint.has_value(), "v3 streaming fixture loads");
    if (!loaded.checkpoint) { return failures; }
    auto& reader = *loaded.checkpoint->engine;
    std::vector<std::byte> bytes(payload.size());
    failures += check(!reader.verify_complete() &&
                          reader.read_file("engine/state/0.bin", 0, std::span(bytes).first(113)) &&
                          !reader.verify_complete() &&
                          reader.read_file("engine/state/0.bin", 113, std::span(bytes).subspan(113)) &&
                          reader.verify_complete() && bytes == payload,
                      "only complete sequential verified bytes make an import committable");
    failures += check(!reader.read_file("engine/state/0.bin", 0, bytes) &&
                          !reader.verify_complete(),
                      "overlapping consumption poisons import finalization");
    loaded.checkpoint.reset();

    const auto root = stored_generation_root(temporary.path, responses, saved->generation);
    const auto manifest_path = root / "manifest.json";
    const auto original_text = read_file_bytes(manifest_path);
    for (const std::uint64_t version : {2ULL, 4ULL, 0xffff'ffff'ffff'ffffULL}) {
        auto manifest = nlohmann::json::parse(original_text);
        manifest["schema_version"] = version;
        const auto replacement = manifest.dump();
        std::ofstream(manifest_path, std::ios::binary | std::ios::trunc) << replacement;
        const auto incompatible = store.load(responses.client_session_sha256, fingerprint());
        failures += check(incompatible.state == SessionCheckpointLoadState::Incompatible &&
                              incompatible.reason == runtime::DurableReason::IncompatibleSchema &&
                              store.status(responses.client_session_sha256, fingerprint())
                                      .at("state") == "incompatible" &&
                              read_file_bytes(manifest_path) == replacement &&
                              std::filesystem::is_regular_file(root.parent_path().parent_path() /
                                                                "current"),
                          "old and unknown schemas are reversible incompatibilities");
    }
    std::ofstream(manifest_path, std::ios::binary | std::ios::trunc) << original_text;
    {
        SessionCheckpointStore bounded({.root = temporary.path,
                                        .disk_quota_bytes = 8ULL << 20,
                                        .staging_bytes = 64,
                                        .read_queue = std::make_shared<TestReadQueue>()});
        const auto refused = bounded.load(responses.client_session_sha256, fingerprint());
        failures += check(refused.state == SessionCheckpointLoadState::Incompatible &&
                              refused.reason == runtime::DurableReason::StagingLimit &&
                              read_file_bytes(manifest_path) == original_text,
                          "a reduced staging budget refuses without quarantining valid bytes");
    }
    const auto incomplete =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        (void)writer.write_file("engine/index.cbor", 0, payload.size() + 1, payload, 1);
        return exported_fixture({.frontier_tokens = 4096,
                                           .restored_tokens = 4096,
                                           .payload_bytes = payload.size() + 1});
    };
    bool rejected = false;
    try {
        rejected = !store.save(responses, fingerprint(), incomplete, publish_fixture);
    } catch (const std::runtime_error&) { rejected = true; }
    failures += check(rejected &&
                          store.status(responses.client_session_sha256, fingerprint())
                                  .at("generation") == saved->generation,
                      "unfinished writer cannot publish over the prior current generation");
    return failures;
}

#ifdef _WIN32
int test_current_parent_flush_failure_retains_visible_generation(DWORD error = ERROR_WRITE_FAULT) {
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    const SessionCheckpointStoreOptions options{
        .root = temporary.path, .disk_quota_bytes = 8ULL << 20,
        .staging_bytes = 1ULL << 20, .read_queue = std::make_shared<TestReadQueue>()};
    SessionCheckpointStore store(options);
    const auto exporter = [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                 .payload_bytes = payload.size()});
    };
    const auto first = store.save(responses, fingerprint(), exporter, publish_fixture);
    if (!first) { return check(false, "parent-flush fixture did not publish"); }
    const auto session_path = temporary.path / "sessions" / responses.client_session_sha256;
    const HANDLE directory = CreateFileW(session_path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        return check(false, "parent-flush fixture directory did not open");
    }
    parent_flush_fault = {};
    parent_flush_fault.error = error;
    const bool inspected = GetFileInformationByHandle(directory, &parent_flush_fault.identity);
    CloseHandle(directory);
    if (!inspected) { return check(false, "parent-flush fixture identity unavailable"); }
    runtime::CheckpointControl control;
    control.session(responses.client_session_sha256)->tag = responses.latest_response_id;
    runtime::SessionCheckpointSkipDetail skip;
    std::optional<SessionCheckpointSaveResult> uncertain;
    std::string diagnostic;
    {
        CheckpointLogCapture log;
        uncertain = store.save(responses, fingerprint(), exporter,
            [&](runtime::DurableSessionEpoch epoch, const std::function<bool()>& commit) {
                parent_flush_fault.armed = true;
                const auto result = control.publish(responses.client_session_sha256,
                                                    responses.latest_response_id, epoch, commit);
                parent_flush_fault.armed = false;
                return result;
            }, &skip);
        diagnostic = log.output.str();
    }
    const auto reason = error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL ||
                                error == ERROR_DISK_QUOTA_EXCEEDED
                            ? runtime::DurableReason::Capacity : runtime::DurableReason::IoUnavailable;
    int failures = check(parent_flush_fault.fired && !uncertain && skip.durable_reason == reason,
                         "post-rename parent-flush failure lost its unacknowledged failure reason");
    const auto failure = nlohmann::json::parse(diagnostic);
    failures += check(failure.at("operation") == "publish_current" &&
                          std::string_view(failure.at("reason").get_ref<const std::string&>()) ==
                              runtime::durable_reason_name(reason) &&
                          failure.at("error_category") == "system" &&
                          failure.at("error_code") == error && failure.size() == 5,
                      "current-publication diagnostic lost the underlying Win32 flush error");
    const auto visible = read_file_bytes(session_path / "current");
    failures += check(!visible.empty() && visible != first->generation + "\n",
                      "flush fault must occur after the real current replacement");
    SessionCheckpointStore restarted(options);
    auto reconciled = restarted.load(responses.client_session_sha256, fingerprint());
    failures += check(reconciled.state == SessionCheckpointLoadState::Available &&
                          reconciled.checkpoint &&
                          reconciled.checkpoint->generation + "\n" == visible,
                      "uncertain publication must retain the generation named by actual current");
    if (reconciled.checkpoint) {
        std::vector<std::byte> restored(payload.size());
        failures += check(reconciled.checkpoint->engine->read_file("engine/state/0.bin", 0, restored) &&
                              reconciled.checkpoint->engine->verify_complete() && restored == payload,
                          "reconciled uncertain publication must retain its complete payload");
    }
    return failures;
}
#endif

int test_export_and_publication_callbacks_are_catalog_unlocked() {
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .read_queue = std::make_shared<TestReadQueue>()});
    const auto exporter = [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                  .payload_bytes = payload.size()});
    };
    const auto initial = store.save(responses, fingerprint(), exporter, publish_fixture);
    int failures = check(initial.has_value(), "catalog callback fixture publishes");
    if (!initial) { return failures; }
    bool exporter_entered = false;
    bool publisher_entered = false;
    const auto replacement = store.save(
        responses, fingerprint(),
        [&](ContinuationCheckpointWriter& writer) {
            exporter_entered = true;
            failures += check(store.covers(responses.client_session_sha256, fingerprint(),
                                            responses.latest_response_id),
                              "export can query catalog without lock inversion");
            store.collect_garbage();
            failures += check(store.erase(responses.client_session_sha256) ==
                                  SessionCheckpointEraseResult::Conflict,
                              "save lease pins session while export runs off catalog lock");
            bool nested_exported = false;
            runtime::SessionCheckpointSkipDetail skip;
            const auto busy = store.save(
                responses, fingerprint(),
                [&](ContinuationCheckpointWriter&) {
                    nested_exported = true;
                    return DurableExportResult{};
                }, publish_fixture, &skip);
            failures += check(!busy && !nested_exported &&
                                  skip.durable_reason == runtime::DurableReason::TransactionBusy,
                              "reentrant save refuses transiently instead of waiting on Engine");
            return exporter(writer);
        },
        [&](runtime::DurableSessionEpoch epoch, const std::function<bool()>& commit) {
            publisher_entered = true;
            failures += check(store.status(responses.client_session_sha256, fingerprint())
                                      .at("generation") == initial->generation,
                              "publication gate can inspect catalog before committing");
            store.collect_garbage();
            const auto result = publish_fixture(epoch, commit);
            failures += check(store.status(responses.client_session_sha256, fingerprint())
                                      .at("state") == "available",
                              "publication callback returns with catalog ownership released");
            return result;
        });
    failures += check(replacement && exporter_entered && publisher_entered &&
                          replacement->generation != initial->generation,
                      "pinned staged generation survives concurrent catalog operations");
    return failures;
}

int test_reader_failure_reasons_and_fresh_retries() {
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    auto queue = std::make_shared<TestReadQueue>();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .read_queue = queue});
    const auto exporter = [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                  .payload_bytes = payload.size()});
    };
    const auto saved = store.save(responses, fingerprint(), exporter, publish_fixture);
    int failures = check(saved.has_value(), "typed reader fixture publishes");
    if (!saved) { return failures; }
    auto loaded = store.load(responses.client_session_sha256, fingerprint());
    if (!loaded.checkpoint) { return failures + check(false, "typed reader fixture loads"); }
    const auto& original = *loaded.checkpoint->engine;
    std::vector<std::byte> bytes(payload.size());
    failures += check(original.failure_reason() == runtime::DurableReason::None &&
                          original.read_file("engine/state/0.bin", 0, std::span(bytes).first(113)) &&
                          !original.verify_complete(),
                      "partial consumption is not an I/O or integrity failure");
    auto fresh = original.fresh_reader();
    failures += check(fresh && fresh->failure_reason() == runtime::DurableReason::None &&
                          fresh->read_file("engine/state/0.bin", 0, bytes) &&
                          fresh->verify_complete() && bytes == payload &&
                          !original.verify_complete(),
                      "fresh retry shares descriptors but resets single-pass consumption");
    queue->fail_submit = true;
    failures += check(!original.read_file("engine/state/0.bin", 113,
                                          std::span(bytes).subspan(113)) &&
                          original.failure_reason() == runtime::DurableReason::IoUnavailable,
                      "queue exception reports transient unavailability");
    queue->fail_submit = false;
    fresh = original.fresh_reader();
    failures += check(fresh && fresh->failure_reason() == runtime::DurableReason::None &&
                          fresh->read_file("engine/state/0.bin", 0, bytes) &&
                          fresh->verify_complete() &&
                          store.status(responses.client_session_sha256, fingerprint())
                                  .at("state") == "available",
                      "I/O retry gets a fresh verifier without quarantining the generation");
    fresh = original.fresh_reader();
    std::byte one{};
    failures += check(fresh &&
                          !fresh->read_file("engine/state/0.bin", payload.size(),
                                            std::span(&one, 1)) &&
                          fresh->failure_reason() == runtime::DurableReason::PayloadSizeMismatch,
                      "out-of-bounds native index request reports a size mismatch");
    fresh = original.fresh_reader();
    failures += check(fresh &&
                          !fresh->read_file("engine/state/1.bin", 0, std::span(&one, 1)) &&
                          fresh->failure_reason() == runtime::DurableReason::InvalidIndex,
                      "undeclared native payload request reports invalid index");
    return failures;
}

int test_native_corruption_quarantine_is_generation_bound() {
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .read_queue = std::make_shared<TestReadQueue>()});
    const auto exporter = [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                  .payload_bytes = payload.size()});
    };
    const auto first = store.save(responses, fingerprint(), exporter, publish_fixture);
    int failures = check(first.has_value(), "native quarantine fixture publishes");
    if (!first) { return failures; }
    auto older = store.load(responses.client_session_sha256, fingerprint());
    if (!older.checkpoint) { return failures + check(false, "old generation is pinned"); }
    for (const auto reason : {runtime::DurableReason::Capacity,
                              runtime::DurableReason::FingerprintMismatch,
                              runtime::DurableReason::IoUnavailable,
                              runtime::DurableReason::StalePublication}) {
        failures += check(!store.quarantine(responses.client_session_sha256,
                                             first->generation, reason) &&
                              store.covers(responses.client_session_sha256, fingerprint(),
                                            responses.latest_response_id),
                          "non-integrity outcome cannot quarantine native data");
    }
    const auto second = store.save(responses, fingerprint(), exporter, publish_fixture);
    if (!second) { return failures + check(false, "newer current publishes with older pin"); }
    const auto older_root =
        stored_generation_root(temporary.path, responses, first->generation);
    failures += check(store.quarantine(responses.client_session_sha256, first->generation,
                                        runtime::DurableReason::InvalidIndex) &&
                          store.status(responses.client_session_sha256, fingerprint())
                                  .at("generation") == second->generation &&
                          std::filesystem::is_directory(older_root),
                      "native corruption keeps newer current and active-reader payload paths");
    std::vector<std::byte> bytes(payload.size());
    failures += check(older.checkpoint->engine->read_file("engine/state/0.bin", 0, bytes) &&
                          bytes == payload && !older.checkpoint->engine->fresh_reader() &&
                          older.checkpoint->engine->failure_reason() ==
                              runtime::DurableReason::InvalidIndex,
                      "existing lease can retire reads but known corruption cannot be retried");
    older.checkpoint.reset();
    failures += check(!std::filesystem::exists(older_root) &&
                          store.status(responses.client_session_sha256, fingerprint())
                                  .at("generation") == second->generation,
                      "last old reader finalizes only the corrupt generation quarantine");
    auto current = store.load(responses.client_session_sha256, fingerprint());
    if (!current.checkpoint) { return failures + check(false, "current generation is pinned"); }
    const auto current_root =
        stored_generation_root(temporary.path, responses, second->generation);
    failures += check(store.quarantine(responses.client_session_sha256, second->generation,
                                        runtime::DurableReason::PayloadSizeMismatch) &&
                          store.status(responses.client_session_sha256, fingerprint())
                                  .at("state") == "missing" &&
                          std::filesystem::is_directory(current_root),
                      "current quarantine detaches pointer without breaking a live reader");
    current.checkpoint.reset();
    failures += check(!std::filesystem::exists(current_root),
                      "last current reader finalizes the deferred directory quarantine");
    return failures;
}

int test_legacy_schema_bad_mac_is_refused_without_mutation() {
    TemporaryDirectory temporary;
    const auto responses = sample_snapshot();
    const auto payload = engine_payload();
    auto queue = std::make_shared<TestReadQueue>();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20,
                                  .origin_mac_key = std::string(32, static_cast<char>(0x42)),
                                  .require_origin_auth = true,
                                  .read_queue = queue});
    const auto exporter = [&](ContinuationCheckpointWriter& writer) {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{}; }
        return exported_fixture({.frontier_tokens = 4096, .restored_tokens = 4096,
                                  .payload_bytes = payload.size()});
    };
    const auto saved = store.save(responses, fingerprint(), exporter, publish_fixture);
    int failures = check(saved.has_value(), "strict legacy-schema fixture publishes");
    if (!saved) { return failures; }
    const auto root = stored_generation_root(temporary.path, responses, saved->generation);
    const auto original_manifest = read_file_bytes(root / "manifest.json");
    const auto original_mac = read_file_bytes(root / "manifest.mac");
    const auto tree = [&] {
        std::vector<std::pair<std::string, std::string>> entries;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(temporary.path)) {
            std::string relative = entry.path().lexically_relative(temporary.path).generic_string();
            if (entry.is_directory()) {
                entries.emplace_back(relative + "/", "");
            } else {
                entries.emplace_back(std::move(relative), read_file_bytes(entry.path()));
            }
        }
        std::sort(entries.begin(), entries.end());
        return entries;
    };
    std::vector<std::string> legacy;
    for (const int version : {0, 1, 2, -1}) {
        auto manifest = nlohmann::json::parse(original_manifest);
        manifest["schema_version"] = version;
        legacy.push_back(manifest.dump());
    }
    auto escaped_key = legacy[2];
    const auto key_position = escaped_key.rfind("\"schema_version\"");
    escaped_key.replace(key_position, std::string_view("\"schema_version\"").size(),
                         "\"\\u0073chema_version\"");
    legacy.push_back(std::move(escaped_key));
    legacy.push_back(" \n" + legacy[2]);
    for (const auto& manifest : legacy) {
        std::ofstream(root / "manifest.json", std::ios::binary | std::ios::trunc) << manifest;
        std::ofstream(root / "manifest.mac", std::ios::binary | std::ios::trunc)
            << std::string(64, '0');
        const auto before = tree();
        const auto submits = queue->submit_count;
        const auto loaded = store.load(responses.client_session_sha256, fingerprint());
        failures += check(loaded.state == SessionCheckpointLoadState::Incompatible &&
                              loaded.reason == runtime::DurableReason::IncompatibleSchema &&
                              !loaded.checkpoint &&
                              store.status(responses.client_session_sha256, fingerprint())
                                      .at("state") == "incompatible" &&
                              queue->submit_count == submits && tree() == before,
                          "legacy integer schema refuses before bad MAC without changing any root entry");
    }

    // A prior streaming fault must not bypass the schema-only guard on a later load.
    std::ofstream(root / "manifest.json", std::ios::binary | std::ios::trunc) << original_manifest;
    std::ofstream(root / "manifest.mac", std::ios::binary | std::ios::trunc) << original_mac;
    auto live = store.load(responses.client_session_sha256, fingerprint());
    if (!live.checkpoint) { return failures + check(false, "strict v3 fixture restores"); }
    auto changed = payload;
    changed.front() ^= std::byte{1};
    {
        std::ofstream file(root / "engine" / "state" / "0.bin",
                            std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(changed.data()),
                    static_cast<std::streamsize>(changed.size()));
    }
    std::vector<std::byte> bytes(payload.size());
    failures += check(!live.checkpoint->engine->read_file("engine/state/0.bin", 0, bytes),
                      "legacy-guard retry fixture records a streamed integrity fault");
    std::ofstream(root / "manifest.json", std::ios::binary | std::ios::trunc) << legacy[2];
    std::ofstream(root / "manifest.mac", std::ios::binary | std::ios::trunc)
        << std::string(64, '0');
    const auto before = tree();
    const auto refused = store.load(responses.client_session_sha256, fingerprint());
    live.checkpoint.reset();
    failures += check(refused.state == SessionCheckpointLoadState::Incompatible &&
                          refused.reason == runtime::DurableReason::IncompatibleSchema &&
                          tree() == before,
                      "remembered corruption cannot quarantine a subsequently observed legacy envelope");
    return failures;
}

int test_manifest_origin_transient_tag_failure_preserves_current() {
    // Council CR-20260831-originauth R2 (all three reviewers): a transient inability to
    // read manifest.mac is a filesystem fault, never forgery - load must report
    // Unavailable, keep current, and succeed on retry once readable again.
    TemporaryDirectory temporary;
    const OpenAIResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload  = engine_payload();
    const std::string mac_key(32, static_cast<char>(0x42));
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> DurableExportResult {
        if (!write_state_fixture(writer, payload)) { return DurableExportResult{.state = runtime::DurableExportState::Refused}; }
        return exported_fixture({.frontier_tokens = 100000,
                                           .restored_tokens = 97500,
                                           .payload_bytes   = payload.size()});
    };
    SessionCheckpointStore store({.root             = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes    = 1ULL << 20,
                                  .origin_mac_key   = mac_key,
                                  .read_queue       = std::make_shared<TestReadQueue>()});
    int failures     = 0;
    const auto saved = store.save(responses, fingerprint(), exporter, publish_fixture);
    failures += check(saved.has_value(), "keyed save publishes");
    if (!saved) { return failures; }
    const std::filesystem::path root =
        stored_generation_root(temporary.path, responses, saved->generation);
    const std::filesystem::path tag  = root / "manifest.mac";
    const std::filesystem::path away = root / "manifest.mac.away";
    // Simulate a transient fault: replace the tag with a directory so the open fails
    // (unreadable), without changing any authenticated bytes.
    std::filesystem::rename(tag, away);
    std::filesystem::create_directory(tag);
    SessionCheckpointLoadResult faulted = store.load(
        responses.client_session_sha256, fingerprint(), responses.latest_response_id);
    failures += check(faulted.state == SessionCheckpointLoadState::Unavailable,
                      "transient tag fault reports unavailable, not forgery");
    failures += check(std::filesystem::is_regular_file(root / "manifest.json") &&
                          std::filesystem::exists(
                              root.parent_path().parent_path() / "current"),
                      "transient tag fault preserves the generation and current pointer");
    std::filesystem::remove(tag);
    std::filesystem::rename(away, tag);
    SessionCheckpointLoadResult retried = store.load(
        responses.client_session_sha256, fingerprint(), responses.latest_response_id);
    failures += check(retried.state == SessionCheckpointLoadState::Available,
                      "retry succeeds after the tag becomes readable again");
    retried.checkpoint.reset();
    return failures;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        const std::string_view test = argv[1];
        if (test == "quota_stale") { return test_quota_stale_publication_preserves_unrelated_currents(); }
        if (test == "ordinary_corrupt") { return test_ordinary_pressure_preserves_same_size_corrupt_victim(); }
#if defined(_WIN32) || defined(__linux__)
        if (test == "disk_full_capacity") {
            int failures = test_disk_full_capacity_preserves_generation();
            failures += test_metadata_close_failure_keeps_os_classification();
#ifdef _WIN32
            failures += test_current_parent_flush_failure_retains_visible_generation(ERROR_DISK_FULL);
            failures += test_current_parent_flush_failure_retains_visible_generation(ERROR_HANDLE_DISK_FULL);
            failures += test_current_parent_flush_failure_retains_visible_generation(ERROR_DISK_QUOTA_EXCEEDED);
#endif
            return failures;
        }
#endif
#ifdef _WIN32
        if (test == "current_flush") { return test_current_parent_flush_failure_retains_visible_generation(); }
        if (test == "protected_long_root") { return test_protected_long_root_transaction(); }
#endif
        return 2;
    }
    int failures = 0;
    failures += test_checkpoint_failure_diagnostics_are_content_safe();
    failures += test_quota_stale_publication_preserves_unrelated_currents();
    failures += test_ordinary_pressure_preserves_same_size_corrupt_victim();
    failures += test_replay_only_survivors_are_durable_without_native_authority();
    failures += test_sha256_streaming();
    failures += test_codec_round_trip();
    failures += test_export_refusal_diagnostics();
    failures += test_transaction_restart_compatibility_and_corruption();
    failures += test_kv_layout_runtime_fingerprint_rejection();
    failures += test_active_reader_delete_and_gc();
    failures += test_may_hold_prefilter();
    failures += test_store_wide_quota_across_sessions();
    failures += test_quota_reclaims_short_sessions_first();
    failures += test_load_scan_failure_does_not_deadlock();
    failures += test_native_read_queue_is_required();
    failures += test_large_session_resaves_within_quota();
    failures += test_resave_keeps_other_sessions_within_quota();
    failures += test_failed_reclamation_keeps_other_sessions();
    failures += test_serialising_read_queue_streams_batches_in_order();
    failures += test_post_publish_reclaim_failure_keeps_save_acknowledged();
    failures += test_post_verification_replacement_fails_closed();
    failures += test_buffered_write_backpressure_round_trips();
    failures += test_buffered_deferred_failure_fails_before_publication();
#if defined(_WIN32) || defined(__linux__)
    failures += test_disk_full_capacity_preserves_generation();
    failures += test_metadata_close_failure_keeps_os_classification();
#endif
    failures += test_pointer_rename_attempt_decides_generation_retention();
    failures += test_save_lease_finishes_quarantine_requested_after_publication();
    failures += test_unmeasurable_generation_entry_fails_quota_closed();
    failures += test_hmac_sha256_rfc4231_vectors();
    failures += test_manifest_origin_authentication();
    failures += test_manifest_origin_compatibility_window();
    failures += test_quota_and_erase_refuse_ancestor_symlink_swaps();
    failures += test_v3_manifest_validation_and_publication_guard();
    failures += test_schema_v3_and_stream_completion();
#ifdef _WIN32
    failures += test_current_parent_flush_failure_retains_visible_generation();
    failures += test_current_parent_flush_failure_retains_visible_generation(ERROR_DISK_FULL);
    failures += test_current_parent_flush_failure_retains_visible_generation(ERROR_HANDLE_DISK_FULL);
    failures += test_current_parent_flush_failure_retains_visible_generation(ERROR_DISK_QUOTA_EXCEEDED);
#endif
    failures += test_export_and_publication_callbacks_are_catalog_unlocked();
    failures += test_reader_failure_reasons_and_fresh_retries();
    failures += test_native_corruption_quarantine_is_generation_bound();
    failures += test_legacy_schema_bad_mac_is_refused_without_mutation();
    failures += test_manifest_origin_transient_tag_failure_preserves_current();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
