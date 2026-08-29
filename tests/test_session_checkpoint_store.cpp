#include "runtime/contract/checkpoint_sha256.h"
#include "serve/session_checkpoint_store.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ninfer;
using namespace ninfer::serve;
using ninfer::runtime::ContinuationCheckpointStats;
using ninfer::runtime::ContinuationCheckpointWriter;

int check(bool condition, const std::string& message) {
    if (condition) { return 0; }
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path             = std::filesystem::temp_directory_path() /
                           ("ninfer-checkpoint-test-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

class TestReadQueue final : public runtime::ContinuationCheckpointReadQueue {
public:
    class Completion final : public runtime::ContinuationCheckpointReadCompletion {
    public:
        explicit Completion(TestReadQueue& owner) : owner_(owner) {}

        void wait() override { ++owner_.wait_count; }

    private:
        TestReadQueue& owner_;
    };

    [[nodiscard]] bool available() const noexcept override { return is_available; }

    [[nodiscard]] std::string_view backend_name() const noexcept override { return "test-queue"; }

    [[nodiscard]] std::string_view unavailable_reason() const noexcept override { return reason; }

    [[nodiscard]] std::unique_ptr<runtime::ContinuationCheckpointReadCompletion>
    submit(const std::filesystem::path& path,
           std::span<const runtime::ContinuationCheckpointReadRequest> requests) override {
        ++submit_count;
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
    std::string reason;
    std::size_t submit_count = 0;
    std::size_t wait_count   = 0;
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
    turn.reasoning_content                        = "preserved private reasoning";
    turn.shared_cache_boundaries_after_text_bytes = {7, 29};
    return turn;
}

ResponseStoreSnapshot sample_snapshot(char digest_byte = 'a') {
    const std::string digest(64, digest_byte);
    ChatTurn user;
    user.role = ChatRole::User;
    ContentPart text;
    text.kind     = ContentKind::Text;
    text.text     = "marker private text";
    text.type_raw = "input_text";
    user.content.push_back(std::move(text));
    const ResponseContext root  = append_response_context({}, {std::move(user)});
    const ResponseContext child = append_response_context(root, {rich_turn()});

    StoredResponse first;
    first.id                    = "resp_private_first";
    first.session_key           = digest;
    first.client_session_sha256 = digest;
    first.response = {{"id", first.id}, {"object", "response"}, {"status", "completed"}};
    first.input_items.push_back({{"type", "message"}, {"id", "item_private_first"}});
    first.context = root;

    StoredResponse second;
    second.id                    = "resp_private_second";
    second.session_key           = digest;
    second.client_session_sha256 = digest;
    second.previous_response_id  = first.id;
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

nlohmann::json fingerprint(std::string artifact = "artifact-sha-a") {
    return {{"schema", 1},
            {"build_source_sha", "source-sha"},
            {"binary_sha256", "binary-sha"},
            {"artifact_sha256", std::move(artifact)},
            {"model", "qwen3.6"},
            {"tokenizer_template_sha256", "template-sha"},
            {"runtime", {{"kv_block_tokens", 256}, {"mtp_depth", 3}, {"dtype", "bf16"}}}};
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
                entry.path().filename().string().find("--") == std::string::npos;
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

bool write_chunked(ContinuationCheckpointWriter& writer, std::string_view path,
                   std::span<const std::byte> bytes) {
    std::uint64_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t count =
            std::min<std::size_t>(113, bytes.size() - static_cast<std::size_t>(offset));
        if (!writer.write_file(path, offset, bytes.size(), bytes.subspan(offset, count))) {
            return false;
        }
        offset += count;
    }
    return true;
}

int test_sha256_streaming() {
    const std::string input = "abc";
    runtime::Sha256 streaming;
    streaming.update(std::as_bytes(std::span(input.data(), 1)));
    streaming.update(std::as_bytes(std::span(input.data() + 1, input.size() - 1)));
    int failures = 0;
    failures += check(runtime::sha256_hex(streaming.finish()) ==
                          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                      "streaming SHA-256 matches the FIPS abc vector");
    const std::span<const std::byte> empty;
    failures += check(runtime::sha256_hex(runtime::sha256(empty)) ==
                          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                      "SHA-256 matches the FIPS empty vector");
    return failures;
}

int test_codec_round_trip() {
    const ResponseStoreSnapshot original = sample_snapshot();
    const std::vector<std::byte> encoded = encode_response_store_snapshot(original, 1ULL << 20);
    const std::optional<ResponseStoreSnapshot> decoded =
        decode_response_store_snapshot(encoded, 1ULL << 20);
    int failures = 0;
    failures += check(decoded.has_value(), "typed response snapshot decodes");
    if (!decoded) { return failures; }
    failures += check(decoded->records.size() == 2 &&
                          decoded->latest_response_id == original.latest_response_id,
                      "response ids and lineage survive codec");
    const StoredResponse& second = decoded->records.back();
    failures +=
        check(second.previous_response_id == "resp_private_first" && second.preserve_thinking,
              "parent and preserve-thinking state survive codec");
    const std::vector<ChatTurn> turns = flatten_response_context(second.context);
    failures += check(turns.size() == 2 && turns.front().content.size() == 1 &&
                          turns.front().content[0].text == "marker private text",
                      "exact continuation marker survives codec");
    failures += check(turns.back().tool_calls.size() == 1 &&
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
    const std::vector<std::byte> reencoded = encode_response_store_snapshot(*decoded, 1ULL << 20);
    failures += check(encoded == reencoded, "response snapshot has a stable canonical encoding");
    ResponseStore live(4, 1ULL << 20);
    live.put(original.records.front());
    bool duplicate_external_commit = false;
    failures += check(
        !live.restore_session(original,
                              [&] {
                                  duplicate_external_commit = true;
                                  return true;
                              }) &&
            !duplicate_external_commit && live.size() == 1 &&
            live.get_for_session("resp_private_first",
                                 std::optional<std::string>{original.client_session_sha256}) !=
                nullptr &&
            live.get_for_session("resp_private_second",
                                 std::optional<std::string>{original.client_session_sha256}) ==
                nullptr,
        "invalid ResponseStore restore leaves both transaction sides unchanged");

    ResponseStore gated(4, 1ULL << 20);
    bool failed_external_commit = false;
    failures += check(!gated.restore_session(original,
                                             [&] {
                                                 failed_external_commit = true;
                                                 return false;
                                             }) &&
                          failed_external_commit && gated.size() == 0,
                      "failed external restore does not publish Responses state");
    failures += check(
        gated.restore_session(original, [] { return true; }) && gated.size() == 2 &&
            gated.get_for_session("resp_private_first",
                                  std::optional<std::string>{original.client_session_sha256}) !=
                nullptr &&
            gated.get_for_session("resp_private_second",
                                  std::optional<std::string>{original.client_session_sha256}) !=
                nullptr,
        "successful external restore atomically publishes Responses state");
    return failures;
}

int test_transaction_restart_compatibility_and_corruption() {
    TemporaryDirectory temporary;
    const ResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload  = engine_payload();
    SessionCheckpointStore store({.root             = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes    = 1ULL << 20,
                                  .read_queue       = std::make_shared<TestReadQueue>()});
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> std::optional<ContinuationCheckpointStats> {
        const std::string metadata = "engine-metadata-v2";
        if (!write_chunked(writer, "engine/metadata.bin",
                           std::as_bytes(std::span(metadata.data(), metadata.size()))) ||
            !write_chunked(writer, "engine/state-0.bin", payload)) {
            return std::nullopt;
        }
        return ContinuationCheckpointStats{.frontier_tokens = 100000,
                                           .restored_tokens = 97500,
                                           .payload_bytes   = payload.size() + metadata.size()};
    };

    int failures = 0;
    const std::optional<SessionCheckpointSaveResult> saved =
        store.save(responses, fingerprint(), exporter);
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
    SessionCheckpointLoadResult loaded = restarted.load(
        responses.client_session_sha256, fingerprint(), responses.latest_response_id);
    failures += check(loaded.state == SessionCheckpointLoadState::Available && loaded.checkpoint &&
                          loaded.checkpoint->generation == saved->generation,
                      "standalone restart ignores interrupted staging and restores prior current");
    if (!loaded.checkpoint) { return failures; }
    std::vector<std::byte> restored(payload.size());
    failures += check(loaded.checkpoint->engine->read_file("engine/state-0.bin", 0, restored) &&
                          restored == payload &&
                          loaded.checkpoint->expected_engine.restored_tokens == 97500 &&
                          read_queue->submit_count == 1 && read_queue->wait_count == 1,
                      "verified engine reader restores exact payload and token summary");
    ResponseStore response_store(8, 1ULL << 20);
    failures +=
        check(response_store.restore_session(loaded.checkpoint->responses, [] { return true; }),
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
        [&](ContinuationCheckpointWriter& writer) -> std::optional<ContinuationCheckpointStats> {
        (void)write_chunked(writer, "engine/partial.bin", payload);
        return std::nullopt;
    };
    failures += check(!restarted.save(responses, fingerprint(), cancelled),
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
        [&](ContinuationCheckpointWriter& writer) -> std::optional<ContinuationCheckpointStats> {
        if (!write_chunked(writer, "engine/insufficient.bin", payload)) { return std::nullopt; }
        return ContinuationCheckpointStats{
            .frontier_tokens = 100000, .restored_tokens = 94999, .payload_bytes = payload.size()};
    };
    failures += check(!restarted.save(responses, fingerprint(), insufficient_prefix),
                      "generation below the frozen 95 percent prefix floor is not published");
    const nlohmann::json after_insufficient =
        restarted.status(responses.client_session_sha256, fingerprint());
    failures += check(after_insufficient.at("state") == "available" &&
                          after_insufficient.at("generation") == saved->generation,
                      "insufficient-prefix save leaves prior current unchanged");
    const std::optional<SessionCheckpointSaveResult> second =
        restarted.save(responses, fingerprint(), exporter);
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
                  !unavailable.checkpoint && unavailable_status.at("state") == "unavailable" &&
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

    const std::filesystem::path corrupt_file = generation_root / "engine/state-0.bin";
    std::fstream corrupt(corrupt_file, std::ios::in | std::ios::out | std::ios::binary);
    char byte = 0;
    corrupt.read(&byte, 1);
    byte ^= 0x5a;
    corrupt.seekp(0);
    corrupt.write(&byte, 1);
    corrupt.close();
    const SessionCheckpointLoadResult corrupted =
        restarted.load(responses.client_session_sha256, fingerprint());
    failures +=
        check(corrupted.state == SessionCheckpointLoadState::Corrupt && !corrupted.checkpoint,
              "checksum corruption cannot enter ResponseStore or Engine");
    failures += check(!std::filesystem::exists(session / "current") &&
                          !std::filesystem::exists(generation_root) &&
                          std::filesystem::exists(session / "generations" / saved->generation),
                      "corrupt current is quarantined while prior immutable generation survives");
    return failures;
}

int test_active_reader_delete_and_gc() {
    TemporaryDirectory temporary;
    const ResponseStoreSnapshot responses = sample_snapshot();
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
        [&](ContinuationCheckpointWriter& writer) -> std::optional<ContinuationCheckpointStats> {
        if (!write_chunked(writer, "engine/state.bin", payload)) { return std::nullopt; }
        return ContinuationCheckpointStats{
            .frontier_tokens = 4096, .restored_tokens = 4096, .payload_bytes = payload.size()};
    };
    int failures     = 0;
    const auto saved = store.save(responses, fingerprint(), exporter);
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
                                             "engine/state.bin") &&
            store.status(responses.client_session_sha256, fingerprint()).at("state") == "available",
        "refused deletion reports conflict and preserves transcript and Engine state");

    const auto newer = store.save(responses, fingerprint(), exporter);
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
    const std::filesystem::path tombstone =
        temporary.path / ".tombstones" / responses.client_session_sha256;
    std::filesystem::create_directories(tombstone);
    std::ofstream(tombstone / "occupied", std::ios::binary) << "occupied";
    const std::string current_before = read_file_bytes(session / "current");
    const std::string responses_before =
        read_file_bytes(session / "generations" / newer->generation / "responses.cbor");
    const std::string engine_before =
        read_file_bytes(session / "generations" / newer->generation / "engine/state.bin");
    const SessionCheckpointEraseResult rename_conflict =
        store.erase(responses.client_session_sha256);
    SessionCheckpointLoadResult after_conflict =
        store.load(responses.client_session_sha256, fingerprint());
    failures +=
        check(rename_conflict == SessionCheckpointEraseResult::Conflict &&
                  after_conflict.state == SessionCheckpointLoadState::Available &&
                  after_conflict.checkpoint &&
                  after_conflict.checkpoint->generation == newer->generation &&
                  read_file_bytes(session / "current") == current_before &&
                  read_file_bytes(session / "generations" / newer->generation / "responses.cbor") ==
                      responses_before &&
                  read_file_bytes(session / "generations" / newer->generation /
                                  "engine/state.bin") == engine_before,
              "failed session rename reports conflict without changing either checkpoint store");
    after_conflict.checkpoint.reset();
    std::filesystem::remove_all(tombstone);

    refuse_cleanup = true;
    failures += check(
        store.erase(responses.client_session_sha256) == SessionCheckpointEraseResult::Erased &&
            !std::filesystem::exists(session) && std::filesystem::is_directory(tombstone) &&
            store.status(responses.client_session_sha256, fingerprint()).at("state") == "missing" &&
            store.erase(responses.client_session_sha256) == SessionCheckpointEraseResult::Missing,
        "successful rename is deleted even when physical cleanup is deferred");
    refuse_cleanup = false;
    store.collect_garbage();
    failures += check(!std::filesystem::exists(tombstone),
                      "garbage collection removes a deferred session tombstone");
    return failures;
}

int test_store_wide_quota_across_sessions() {
    const ResponseStoreSnapshot first_session  = sample_snapshot('a');
    const ResponseStoreSnapshot second_session = sample_snapshot('b');
    const ResponseStoreSnapshot third_session  = sample_snapshot('c');
    const std::vector<std::byte> payload       = engine_payload();
    const auto exporter =
        [&](ContinuationCheckpointWriter& writer) -> std::optional<ContinuationCheckpointStats> {
        if (!write_chunked(writer, "engine/state.bin", payload)) { return std::nullopt; }
        return ContinuationCheckpointStats{
            .frontier_tokens = 4096, .restored_tokens = 4096, .payload_bytes = payload.size()};
    };

    TemporaryDirectory measurement;
    SessionCheckpointStore measuring_store({
        .root             = measurement.path,
        .disk_quota_bytes = 1ULL << 20,
        .staging_bytes    = 1ULL << 20,
        .read_queue       = std::make_shared<TestReadQueue>(),
    });
    const auto measured = measuring_store.save(first_session, fingerprint(), exporter);
    int failures        = 0;
    failures += check(measured.has_value(), "quota fixture generation size is measurable");
    if (!measured) { return failures; }

    const std::uint64_t quota = measured->bytes * 2;
    TemporaryDirectory temporary;
    SessionCheckpointStore store({.root             = temporary.path,
                                  .disk_quota_bytes = quota,
                                  .staging_bytes    = 1ULL << 20,
                                  .read_queue       = std::make_shared<TestReadQueue>()});
    const auto first  = store.save(first_session, fingerprint(), exporter);
    const auto second = store.save(second_session, fingerprint(), exporter);
    const auto third  = store.save(third_session, fingerprint(), exporter);
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
    const auto protected_saved = protected_store.save(first_session, fingerprint(), exporter);
    SessionCheckpointLoadResult active =
        protected_store.load(first_session.client_session_sha256, fingerprint());
    const auto refused = protected_store.save(second_session, fingerprint(), exporter);
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
    const auto failure_first    = failure_store.save(first_session, fingerprint(), exporter);
    const auto failure_previous = failure_store.save(second_session, fingerprint(), exporter);
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
    const std::string previous_engine = read_file_bytes(previous_generation / "engine/state.bin");

    refuse_cleanup                = true;
    const auto failed_replacement = failure_store.save(second_session, fingerprint(), exporter);
    SessionCheckpointLoadResult previous_loaded =
        failure_store.load(second_session.client_session_sha256, fingerprint());
    const std::filesystem::path deferred_tombstone =
        failure_temporary.path / ".tombstones" / first_session.client_session_sha256;
    failures += check(
        !failed_replacement && previous_loaded.state == SessionCheckpointLoadState::Available &&
            previous_loaded.checkpoint &&
            previous_loaded.checkpoint->generation == failure_previous->generation &&
            read_file_bytes(previous_session / "current") == previous_current &&
            read_file_bytes(previous_generation / "manifest.json") == previous_manifest &&
            read_file_bytes(previous_generation / "responses.cbor") == previous_responses &&
            read_file_bytes(previous_generation / "engine/state.bin") == previous_engine &&
            std::filesystem::is_directory(previous_generation) &&
            std::filesystem::is_directory(deferred_tombstone) &&
            regular_file_bytes(deferred_tombstone / "generations") == failure_first->bytes &&
            retained_generation_bytes(failure_temporary.path) ==
                failure_first->bytes + failure_previous->bytes,
        "cleanup failure rejects save before publication and preserves the prior current bytes");
    previous_loaded.checkpoint.reset();
    refuse_cleanup = false;
    failure_store.collect_garbage();
    failures += check(
        !std::filesystem::exists(deferred_tombstone) &&
            failure_store.status(second_session.client_session_sha256, fingerprint()).at("state") ==
                "available" &&
            retained_generation_bytes(failure_temporary.path) == failure_previous->bytes,
        "deferred tombstones remain quota-accounted and are reclaimed by GC");
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
} // namespace

int main() {
    int failures = 0;
    failures += test_sha256_streaming();
    failures += test_codec_round_trip();
    failures += test_transaction_restart_compatibility_and_corruption();
    failures += test_active_reader_delete_and_gc();
    failures += test_store_wide_quota_across_sessions();
    failures += test_native_read_queue_is_required();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
