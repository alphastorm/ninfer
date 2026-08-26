#include "core/sha256.h"
#include "serve/session_checkpoint_store.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace ninfer;
using namespace ninfer::serve;

int check(bool condition, const std::string& message) {
    if (condition) { return 0; }
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("ninfer-checkpoint-test-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
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
    turn.reasoning_content = "preserved private reasoning";
    turn.shared_cache_boundaries_after_text_bytes = {7, 29};
    return turn;
}

ResponseStoreSnapshot sample_snapshot() {
    const std::string digest(64, 'a');
    ChatTurn user;
    user.role = ChatRole::User;
    ContentPart text;
    text.kind     = ContentKind::Text;
    text.text     = "marker private text";
    text.type_raw = "input_text";
    user.content.push_back(std::move(text));
    const ResponseContext root = append_response_context({}, {std::move(user)});
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
    second.input_items.push_back({{"type", "function_call_output"},
                                  {"call_id", "call_private"},
                                  {"output", "tool bytes private"}});
    second.context           = child;
    second.preserve_thinking = true;
    return {.client_session_sha256 = digest,
            .latest_response_id = second.id,
            .records = {std::move(first), std::move(second)}};
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
    crypto::Sha256 streaming;
    streaming.update(std::as_bytes(std::span(input.data(), 1)));
    streaming.update(std::as_bytes(std::span(input.data() + 1, input.size() - 1)));
    int failures = 0;
    failures += check(
        crypto::sha256_hex(streaming.finish()) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "streaming SHA-256 matches the FIPS abc vector");
    const std::span<const std::byte> empty;
    failures += check(
        crypto::sha256_hex(crypto::sha256(empty)) ==
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
    failures += check(second.previous_response_id == "resp_private_first" &&
                          second.preserve_thinking,
                      "parent and preserve-thinking state survive codec");
    const std::vector<ChatTurn> turns = flatten_response_context(second.context);
    failures += check(turns.size() == 2 && turns.back().tool_calls.size() == 1 &&
                          turns.back().tool_calls[0].arguments_json == "{\"x\":1}",
                      "tool calls and context DAG survive codec");
    failures += check(turns.back().content.size() == 1 &&
                          turns.back().content[0].source.bytes ==
                              std::vector<std::uint8_t>({0, 1, 2, 255}) &&
                          turns.back().reasoning_content == "preserved private reasoning",
                      "media bytes and reasoning survive codec");
    const std::vector<std::byte> reencoded =
        encode_response_store_snapshot(*decoded, 1ULL << 20);
    failures += check(encoded == reencoded, "response snapshot has a stable canonical encoding");
    ResponseStore live(4, 1ULL << 20);
    live.put(original.records.front());
    failures += check(!live.restore_session(original) && live.size() == 1 &&
                          live.get("resp_private_first") != nullptr &&
                          live.get("resp_private_second") == nullptr,
                      "failed ResponseStore restore leaves existing state unchanged");
    return failures;
}

int test_transaction_restart_compatibility_and_corruption() {
    TemporaryDirectory temporary;
    const ResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 1ULL << 20});
    const auto exporter = [&](ContinuationCheckpointWriter& writer)
        -> std::optional<ContinuationCheckpointStats> {
        const std::string metadata = "engine-metadata-v2";
        if (!write_chunked(writer, "engine/metadata.bin",
                           std::as_bytes(std::span(metadata.data(), metadata.size()))) ||
            !write_chunked(writer, "engine/state-0.bin", payload)) {
            return std::nullopt;
        }
        return ContinuationCheckpointStats{.frontier_tokens = 100000,
                                           .restored_tokens = 97500,
                                           .payload_bytes = payload.size() + metadata.size()};
    };

    int failures = 0;
    const std::optional<SessionCheckpointSaveResult> saved =
        store.save(responses, fingerprint(), exporter);
    failures += check(saved.has_value(), "complete generation publishes");
    if (!saved) { return failures; }
    const std::filesystem::path session =
        temporary.path / "sessions" / responses.client_session_sha256;
    failures += check(std::filesystem::is_regular_file(
                          session / "generations" / saved->generation / "manifest.json") &&
                          std::filesystem::is_regular_file(session / "current"),
                      "payload, manifest, and current pointer are published");

    // Simulate process death after staging bytes but before manifest/current publication.
    const std::filesystem::path interrupted = session / "generations" / ".staging-interrupted";
    std::filesystem::create_directories(interrupted);
    std::ofstream(interrupted / "partial.bin", std::ios::binary) << "partial";
    SessionCheckpointStore restarted({.root = temporary.path,
                                      .disk_quota_bytes = 8ULL << 20,
                                      .staging_bytes = 1ULL << 20});
    std::optional<VerifiedSessionCheckpoint> loaded = restarted.load(
        responses.client_session_sha256, fingerprint(), responses.latest_response_id);
    failures += check(loaded.has_value() && loaded->generation == saved->generation,
                      "standalone restart ignores interrupted staging and restores prior current");
    if (!loaded) { return failures; }
    std::vector<std::byte> restored(payload.size());
    failures += check(loaded->engine->read_file("engine/state-0.bin", 0, restored) &&
                          restored == payload &&
                          loaded->expected_engine.restored_tokens == 97500,
                      "verified engine reader restores exact payload and token summary");
    ResponseStore response_store(8, 1ULL << 20);
    failures += check(response_store.restore_session(loaded->responses),
                      "verified response snapshot installs atomically");
    const auto restored_response =
        response_store.get_for_session(responses.latest_response_id,
                                       responses.client_session_sha256);
    failures += check(restored_response && restored_response->previous_response_id ==
                                               "resp_private_first",
                      "restarted ResponseStore exposes exact continuation lineage");

    loaded.reset();
    const nlohmann::json wrong = fingerprint("different-artifact");
    failures += check(!restarted.load(responses.client_session_sha256, wrong),
                      "runtime/model fingerprint mismatch is a cache miss");
    const nlohmann::json incompatible =
        restarted.status(responses.client_session_sha256, wrong);
    failures += check(incompatible.at("state") == "incompatible",
                      "status distinguishes incompatible from corrupt");
    failures += check(restarted.load(responses.client_session_sha256, fingerprint()).has_value(),
                      "compatibility miss does not quarantine a valid generation");

    // An exporter cancellation may write staging bytes, but it cannot replace current.
    const auto cancelled = [&](ContinuationCheckpointWriter& writer)
        -> std::optional<ContinuationCheckpointStats> {
        (void)write_chunked(writer, "engine/partial.bin", payload);
        return std::nullopt;
    };
    failures += check(!restarted.save(responses, fingerprint(), cancelled),
                      "cancelled export is not published");
    const nlohmann::json after_cancel =
        restarted.status(responses.client_session_sha256, fingerprint());
    failures += check(after_cancel.at("state") == "available" &&
                          after_cancel.at("generation") == saved->generation,
                      "cancelled export leaves prior current unchanged");
    const std::string public_status = after_cancel.dump();
    failures += check(public_status.find("resp_private") == std::string::npos &&
                          public_status.find("marker private") == std::string::npos &&
                          public_status.find("call_private") == std::string::npos,
                      "status exposes no response, prompt, tool, or reasoning content");

    const std::optional<SessionCheckpointSaveResult> second =
        restarted.save(responses, fingerprint(), exporter);
    failures += check(second.has_value() && second->generation != saved->generation,
                      "new complete generation replaces current");
    if (!second) { return failures; }
    failures += check(!std::filesystem::exists(interrupted),
                      "successful GC removes an abandoned staging generation");
    const std::filesystem::path corrupt_file =
        session / "generations" / second->generation / "engine/state-0.bin";
    std::fstream corrupt(corrupt_file, std::ios::in | std::ios::out | std::ios::binary);
    char byte = 0;
    corrupt.read(&byte, 1);
    byte ^= 0x5a;
    corrupt.seekp(0);
    corrupt.write(&byte, 1);
    corrupt.close();
    failures += check(!restarted.load(responses.client_session_sha256, fingerprint()),
                      "checksum corruption cannot enter ResponseStore or Engine");
    failures += check(!std::filesystem::exists(session / "current") &&
                          std::filesystem::exists(session / "generations" / saved->generation),
                      "corrupt current is quarantined while prior immutable generation survives");
    return failures;
}

int test_active_reader_delete_and_gc() {
    TemporaryDirectory temporary;
    const ResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 64ULL << 10,
                                  .staging_bytes = 1ULL << 20});
    const auto exporter = [&](ContinuationCheckpointWriter& writer)
        -> std::optional<ContinuationCheckpointStats> {
        if (!write_chunked(writer, "engine/state.bin", payload)) { return std::nullopt; }
        return ContinuationCheckpointStats{.frontier_tokens = 4096,
                                           .restored_tokens = 4096,
                                           .payload_bytes = payload.size()};
    };
    int failures = 0;
    const auto saved = store.save(responses, fingerprint(), exporter);
    failures += check(saved.has_value(), "GC fixture generation saves");
    if (!saved) { return failures; }
    auto loaded = store.load(responses.client_session_sha256, fingerprint());
    failures += check(loaded.has_value() && !store.erase(responses.client_session_sha256),
                      "explicit deletion cannot race an active restore reader");
    loaded.reset();

    const std::filesystem::path session =
        temporary.path / "sessions" / responses.client_session_sha256;
    const std::filesystem::path stale = session / "generations" / "stale-generation";
    std::filesystem::create_directories(stale);
    std::ofstream filler(stale / "large.bin", std::ios::binary);
    std::vector<char> large(128ULL << 10, 'x');
    filler.write(large.data(), large.size());
    filler.close();
    store.collect_garbage();
    failures += check(!std::filesystem::exists(stale) &&
                          std::filesystem::exists(session / "generations" /
                                                  saved->generation) &&
                          std::filesystem::exists(session / "current"),
                      "quota GC removes stale LRU generation but protects current");
    failures += check(store.erase(responses.client_session_sha256) &&
                          store.status(responses.client_session_sha256, fingerprint()).at("state") ==
                              "missing",
                      "authenticated delete primitive removes inactive checkpoint state");
    return failures;
}

} // namespace

int main() {
    int failures = 0;
    failures += test_sha256_streaming();
    failures += test_codec_round_trip();
    failures += test_transaction_restart_compatibility_and_corruption();
    failures += test_active_reader_delete_and_gc();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
