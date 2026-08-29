#include "core/sha256.h"
#include "serve/server_identity.h"
#include "serve/session_checkpoint_store.h"

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
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace ninfer;
using namespace ninfer::serve;
using ninfer::runtime::AuthenticatedCheckpointNamespace;
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
    turn.role = "assistant";
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

ResponseStoreSnapshot sample_snapshot(char digest_byte = 'a') {
    const std::string digest(64, digest_byte);
    ChatTurn user;
    user.role = "user";
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
    second.input_items.push_back(
        {{"type", "function_call_output"},
         {"call_id", "call_private"},
         {"output", "diff --git a/workspace.txt b/workspace.txt\n+exact marker\n"}});
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

AuthenticatedCheckpointNamespace checkpoint_namespace(const ResponseStoreSnapshot& snapshot,
                                                       char tenant_byte = '1') {
    return AuthenticatedCheckpointNamespace::authenticated(std::string(64, tenant_byte),
                                                           snapshot.client_session_sha256);
}

std::string namespace_storage_digest(const AuthenticatedCheckpointNamespace& checkpoint_namespace) {
    constexpr std::string_view domain = "ninfer-checkpoint-namespace-v1";
    crypto::Sha256 hasher;
    hasher.update(std::as_bytes(std::span(domain.data(), domain.size())));
    hasher.update(std::as_bytes(std::span(checkpoint_namespace.tenant_sha256().data(),
                                          checkpoint_namespace.tenant_sha256().size())));
    hasher.update(std::as_bytes(std::span(checkpoint_namespace.session_sha256().data(),
                                          checkpoint_namespace.session_sha256().size())));
    return crypto::sha256_hex(hasher.finish());
}

std::filesystem::path stored_session_path(const std::filesystem::path& root,
                                          const ResponseStoreSnapshot& snapshot,
                                          char tenant_byte = '1') {
    return root / "sessions" /
           namespace_storage_digest(checkpoint_namespace(snapshot, tenant_byte));
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
        if (entry.is_regular_file()) {
            total += static_cast<std::uint64_t>(entry.file_size());
        }
    }
    return total;
}

std::uint64_t retained_generation_bytes(const std::filesystem::path& root) {
    std::uint64_t total = 0;
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
            total += regular_file_bytes(whole_session ? entry.path() / "generations"
                                                       : entry.path());
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

nlohmann::json production_fingerprint(const BuildInfo& build,
                                      std::size_t sequence_capacity_bytes = 32ULL << 20) {
    ServeOptions options;
    options.binary_sha256       = std::string(64, '1');
    options.artifact_sha256     = std::string(64, '2');
    options.config_sha256       = std::string(64, '3');
    options.deployment_profile  = "rtx-3090-release";
    options.allow_prefix_reuse  = true;
    options.max_context         = 65536;
    options.kv_capacity         = KvCapacityPolicy::explicit_capacity(65536);
    options.max_concurrency     = 2;
    options.prefill_chunk       = 1024;
    options.kv_cache            = KvCacheStorage::RotatedInt8KeyInt4ValueGroup64;
    options.speculative.backend = SpeculativeBackend::Mtp;
    options.speculative.draft_tokens = 3;

    EngineOptions engine;
    engine.max_context     = options.max_context;
    engine.kv_capacity     = options.kv_capacity;
    engine.max_concurrency = options.max_concurrency;
    engine.prefill_chunk   = options.prefill_chunk;
    engine.kv_cache        = options.kv_cache;
    engine.speculative     = options.speculative;
    engine.use_cuda_graph  = true;

    LoadSummary load;
    load.target     = "qwen3_6_27b";
    load.model_id   = "qwen3.6-27b";
    load.weights_id = "groupwise-int";

    MemorySummary memory;
    memory.max_context                       = engine.max_context;
    memory.kv_capacity_mode                  = KvCapacityMode::Explicit;
    memory.kv_capacity                       = 65536;
    memory.kv_capacity_page_groups           = 256;
    memory.kv_cache                          = engine.kv_cache;
    memory.sequence.capacity_bytes           = sequence_capacity_bytes;
    memory.workspace.capacity_bytes          = 64ULL << 20;
    memory.request_transient.capacity_bytes  = 8ULL << 20;
    memory.minimum_runtime_reservation_bytes = 512ULL << 20;
    memory.runtime_reservation_bytes         = 768ULL << 20;
    memory.kv_capacity_increment_bytes       = 4ULL << 20;
    memory.kv_payload_bytes                  = 384ULL << 20;
    return session_checkpoint_runtime_fingerprint(options, engine, load, memory, build);
}

SessionCheckpointStoreOptions manager_options(const std::filesystem::path& root) {
    return {.root = root,
            .disk_quota_bytes = 8ULL << 20,
            .staging_bytes = 1ULL << 20,
            .tombstone_cleanup = {}};
}

class FakeCheckpointEngine {
public:
    SessionCheckpointEngine access() {
        SessionCheckpointEngine out;
        out.checkpoint =
            [this](const AuthenticatedCheckpointNamespace& checkpoint_namespace,
                   std::string_view checkpoint_tag, ContinuationCheckpointWriter& writer,
                   std::size_t) -> std::optional<ContinuationCheckpointStats> {
            ++checkpoint_calls;
            tenant = checkpoint_namespace.tenant_sha256();
            session = checkpoint_namespace.session_sha256();
            tag.assign(checkpoint_tag);
            if (checkpoint_hook) { checkpoint_hook(); }
            if (!write_chunked(writer, "engine/state.bin", payload) || fail_checkpoint) {
                return std::nullopt;
            }
            return ContinuationCheckpointStats{.frontier_tokens = 4096,
                                               .restored_tokens = 4096,
                                               .payload_bytes = payload.size()};
        };
        out.restore =
            [this](const AuthenticatedCheckpointNamespace& checkpoint_namespace,
                   std::string_view checkpoint_tag, const runtime::ContinuationCheckpointReader& reader,
                   ContinuationCheckpointStats expected,
                   std::size_t) -> std::optional<ContinuationCheckpointStats> {
            ++restore_calls;
            tenant = checkpoint_namespace.tenant_sha256();
            session = checkpoint_namespace.session_sha256();
            tag.assign(checkpoint_tag);
            if (fail_restore ||
                reader.file_size("engine/state.bin") !=
                    std::optional<std::uint64_t>(payload.size())) {
                return std::nullopt;
            }
            restored_payload.resize(payload.size());
            if (!reader.read_file("engine/state.bin", 0, std::span(restored_payload)) ||
                restored_payload != payload) {
                return std::nullopt;
            }
            return expected;
        };
        return out;
    }

    std::vector<std::byte> payload = engine_payload();
    std::vector<std::byte> restored_payload;
    std::string tenant;
    std::string session;
    std::string tag;
    int checkpoint_calls = 0;
    int restore_calls    = 0;
    bool fail_checkpoint = false;
    bool fail_restore    = false;
    std::function<void()> checkpoint_hook;
};

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

int test_authenticated_namespace_validation() {
    int failures = 0;
    const auto rejects = [](std::string tenant, std::string session) {
        try {
            (void)AuthenticatedCheckpointNamespace::authenticated(std::move(tenant),
                                                                  std::move(session));
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    failures += check(rejects(std::string(63, 'a'), std::string(64, 'b')),
                      "short authenticated tenant digest is rejected");
    failures += check(rejects(std::string(64, 'A'), std::string(64, 'b')),
                      "non-lowercase authenticated tenant digest is rejected");
    failures += check(rejects(std::string(64, 'a'), std::string(64, '/')),
                      "non-hex authenticated session digest is rejected");
    const auto first = AuthenticatedCheckpointNamespace::authenticated(std::string(64, 'a'),
                                                                       std::string(64, 'b'));
    const auto same = AuthenticatedCheckpointNamespace::authenticated(std::string(64, 'a'),
                                                                      std::string(64, 'b'));
    const auto other_tenant = AuthenticatedCheckpointNamespace::authenticated(
        std::string(64, 'c'), std::string(64, 'b'));
    failures += check(first == same && first != other_tenant,
                      "tenant and session jointly define checkpoint identity");
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
    failures += check(turns.size() == 2 && turns.front().content.size() == 1 &&
                          turns.front().content[0].text == "marker private text",
                      "exact continuation marker survives codec");
    failures += check(turns.back().tool_calls.size() == 1 &&
                          turns.back().tool_calls[0].arguments_json == "{\"x\":1}",
                      "typed tool call survives codec");
    failures += check(
        second.input_items == original.records.back().input_items &&
            second.input_items.front().at("output") ==
                "diff --git a/workspace.txt b/workspace.txt\n+exact marker\n",
        "nonempty workspace diff and typed tool result survive byte-exactly");
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
    bool duplicate_external_commit = false;
    failures += check(!live.restore_session(original, [&] {
                          duplicate_external_commit = true;
                          return true;
                      }) &&
                          !duplicate_external_commit && live.size() == 1 &&
                          live.get_for_session("resp_private_first",
                                               original.client_session_sha256) != nullptr &&
                          live.get_for_session("resp_private_second",
                                               original.client_session_sha256) == nullptr,
                      "invalid ResponseStore restore leaves both transaction sides unchanged");

    ResponseStore gated(4, 1ULL << 20);
    bool failed_external_commit = false;
    failures += check(!gated.restore_session(original, [&] {
                          failed_external_commit = true;
                          return false;
                      }) &&
                          failed_external_commit && gated.size() == 0,
                      "failed external restore does not publish Responses state");
    failures += check(gated.restore_session(original, [] { return true; }) && gated.size() == 2 &&
                          gated.get_for_session("resp_private_first",
                                                original.client_session_sha256) != nullptr &&
                          gated.get_for_session("resp_private_second",
                                                original.client_session_sha256) != nullptr,
                      "successful external restore atomically publishes Responses state");
    return failures;
}

int test_transaction_restart_compatibility_and_corruption() {
    TemporaryDirectory temporary;
    const ResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload = engine_payload();
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = 8ULL << 20,
                                  .staging_bytes = 4ULL << 10,
                                  .tombstone_cleanup = {}});
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
    failures += check(payload.size() > store.options().staging_bytes,
                      "engine fixture exceeds the configured host staging bound");
    const ResponseStoreSnapshot mismatched_responses = sample_snapshot('b');
    failures += check(
        !store.save(checkpoint_namespace(responses), mismatched_responses, fingerprint(), exporter),
        "response state from another authenticated session cannot be published");
    const std::optional<SessionCheckpointSaveResult> saved = store.save(
        checkpoint_namespace(responses), responses, fingerprint(), exporter);
    failures += check(saved.has_value(), "complete generation publishes");
    if (!saved) { return failures; }
    const std::filesystem::path session = stored_session_path(temporary.path, responses);
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
                                      .staging_bytes = 4ULL << 10,
                                      .tombstone_cleanup = {}});
    SessionCheckpointLoadResult loaded = restarted.load(
        checkpoint_namespace(responses), fingerprint(), responses.latest_response_id);
    failures += check(loaded.state == SessionCheckpointLoadState::Available &&
                          loaded.checkpoint && loaded.checkpoint->generation == saved->generation,
                      "standalone restart ignores interrupted staging and restores prior current");
    if (!loaded.checkpoint) { return failures; }
    std::vector<std::byte> restored(payload.size());
    failures += check(loaded.checkpoint->engine->read_file("engine/state-0.bin", 0, restored) &&
                          restored == payload &&
                          loaded.checkpoint->expected_engine == saved->engine,
                      "verified engine reader restores exact payload and token summary");
    ResponseStore response_store(8, 1ULL << 20);
    failures += check(
        response_store.restore_session(loaded.checkpoint->responses, [] { return true; }),
        "verified response snapshot installs atomically");
    const auto restored_response =
        response_store.get_for_session(responses.latest_response_id,
                                       responses.client_session_sha256);
    failures += check(restored_response && restored_response->previous_response_id ==
                                               "resp_private_first",
                      "restarted ResponseStore exposes exact continuation lineage");

    loaded.checkpoint.reset();
    const SessionCheckpointLoadResult other_tenant =
        restarted.load(checkpoint_namespace(responses, '2'), fingerprint());
    failures += check(other_tenant.state == SessionCheckpointLoadState::Missing &&
                          !other_tenant.checkpoint,
                      "same session digest in another authenticated tenant is isolated");
    const nlohmann::json wrong = fingerprint("different-artifact");
    const SessionCheckpointLoadResult wrong_fingerprint =
        restarted.load(checkpoint_namespace(responses), wrong);
    failures += check(wrong_fingerprint.state == SessionCheckpointLoadState::Incompatible &&
                          !wrong_fingerprint.checkpoint,
                      "runtime/model fingerprint mismatch is a cache miss");
    const nlohmann::json incompatible = restarted.status(checkpoint_namespace(responses), wrong);
    failures += check(incompatible.at("state") == "incompatible",
                      "status distinguishes incompatible from corrupt");
    SessionCheckpointLoadResult compatible =
        restarted.load(checkpoint_namespace(responses), fingerprint());
    failures += check(compatible.state == SessionCheckpointLoadState::Available &&
                          compatible.checkpoint.has_value(),
                      "compatibility miss does not quarantine a valid generation");
    compatible.checkpoint.reset();

    // An exporter cancellation may write staging bytes, but it cannot replace current.
    const auto cancelled = [&](ContinuationCheckpointWriter& writer)
        -> std::optional<ContinuationCheckpointStats> {
        (void)write_chunked(writer, "engine/partial.bin", payload);
        return std::nullopt;
    };
    failures += check(!restarted.save(checkpoint_namespace(responses), responses, fingerprint(),
                                      cancelled),
                      "cancelled export is not published");
    const nlohmann::json after_cancel =
        restarted.status(checkpoint_namespace(responses), fingerprint());
    failures += check(after_cancel.at("state") == "available" &&
                          after_cancel.at("generation") == saved->generation,
                      "cancelled export leaves prior current unchanged");
    const std::string public_status = after_cancel.dump();
    failures += check(public_status.find(std::string(64, '1')) == std::string::npos &&
                          public_status.find(responses.client_session_sha256) == std::string::npos &&
                          public_status.find("resp_private") == std::string::npos &&
                          public_status.find("marker private") == std::string::npos &&
                          public_status.find("call_private") == std::string::npos,
                      "status does not expose raw tenant/session identity, response, prompt, tool, or "
                      "reasoning content");

    const auto insufficient_prefix = [&](ContinuationCheckpointWriter& writer)
        -> std::optional<ContinuationCheckpointStats> {
        if (!write_chunked(writer, "engine/insufficient.bin", payload)) { return std::nullopt; }
        return ContinuationCheckpointStats{.frontier_tokens = 100000,
                                           .restored_tokens = 94999,
                                           .payload_bytes = payload.size()};
    };
    failures += check(!restarted.save(checkpoint_namespace(responses), responses, fingerprint(),
                                      insufficient_prefix),
                      "generation below the frozen 95 percent prefix floor is not published");
    const nlohmann::json after_insufficient =
        restarted.status(checkpoint_namespace(responses), fingerprint());
    failures += check(after_insufficient.at("state") == "available" &&
                          after_insufficient.at("generation") == saved->generation,
                      "insufficient-prefix save leaves prior current unchanged");
    const std::optional<SessionCheckpointSaveResult> second =
        restarted.save(checkpoint_namespace(responses), responses, fingerprint(), exporter);
    failures += check(second.has_value() && second->generation != saved->generation,
                      "new complete generation replaces current");
    if (!second) { return failures; }
    failures += check(!std::filesystem::exists(interrupted),
                      "successful GC removes an abandoned staging generation");
    const std::filesystem::path generation_root =
        session / "generations" / second->generation;
    const std::filesystem::path manifest = generation_root / "manifest.json";
    const std::filesystem::path hidden_manifest = generation_root / "manifest.temporarily-missing";
    std::filesystem::rename(manifest, hidden_manifest);
    const SessionCheckpointLoadResult unavailable =
        restarted.load(checkpoint_namespace(responses), fingerprint());
    const nlohmann::json unavailable_status =
        restarted.status(checkpoint_namespace(responses), fingerprint());
    failures += check(unavailable.state == SessionCheckpointLoadState::Unavailable &&
                          !unavailable.checkpoint && unavailable_status.at("state") == "unavailable" &&
                          std::filesystem::is_regular_file(session / "current") &&
                          std::filesystem::is_directory(generation_root),
                      "transient manifest loss preserves current and returns unavailable");
    std::filesystem::rename(hidden_manifest, manifest);
    SessionCheckpointLoadResult retry =
        restarted.load(checkpoint_namespace(responses), fingerprint());
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
        restarted.load(checkpoint_namespace(responses), fingerprint());
    failures += check(corrupted.state == SessionCheckpointLoadState::Corrupt &&
                          !corrupted.checkpoint,
                      "checksum corruption cannot enter ResponseStore or Engine");
    failures += check(!std::filesystem::exists(session / "current") &&
                          !std::filesystem::exists(generation_root) &&
                          std::filesystem::exists(session / "generations" / saved->generation),
                      "corrupt current is quarantined while prior immutable generation survives");
    return failures;
}

int test_production_manager_restart_and_identity_isolation() {
    TemporaryDirectory temporary;
    const std::string api_key = "checkpoint-api-key";
    const std::string tenant  = session_checkpoint_tenant_sha256(api_key);
    const BuildInfo build{
        .upstream_base_sha = "upstream-sha",
        .patch_stack_sha = "patch-sha",
        .build_profile = "omp-v0.2-rtx3090",
        .build_type = "Release",
        .cxx_compiler = "GNU-13.3.0",
        .cuda_compiler = "NVIDIA-12.8.93",
        .cuda_toolkit = "12.8.93",
        .cuda_architecture = "86",
        .source_dirty = false,
    };
    const nlohmann::json runtime = production_fingerprint(build);
    ResponseStoreSnapshot snapshot = sample_snapshot();
    for (StoredResponse& response : snapshot.records) {
        response.session_key = "stable-checkpoint-session-tag";
    }
    ResponseStore source(8, 8ULL << 20);
    for (const StoredResponse& response : snapshot.records) { source.put(response); }

    int failures = 0;
    failures += check(
        tenant == "db1ab03f8e85eb9ea5f0e20bcb0bbaff8518cbe2b935820dfbaffba3a9c71deb" &&
            tenant != session_checkpoint_tenant_sha256("checkpoint-api-key-2") &&
            AuthenticatedCheckpointNamespace::valid_sha256(tenant),
        "API key tenant derivation is deterministic, domain-separated, and nonidentity");
    failures += check(runtime.at("identity").at("model_id") == "qwen3.6-27b" &&
                          runtime.at("identity").at("artifact_sha256") == std::string(64, '2') &&
                          runtime.at("build").at("patch_stack_sha") == "patch-sha" &&
                          runtime.at("build").at("build_profile") == "omp-v0.2-rtx3090" &&
                          runtime.at("build").at("cuda_architecture") == "86" &&
                          runtime.at("engine").at("max_context") == 65536 &&
                          runtime.at("engine").at("kv_cache") ==
                              static_cast<int>(KvCacheStorage::RotatedInt8KeyInt4ValueGroup64) &&
                          runtime.at("engine").at("speculative_backend") ==
                              static_cast<int>(SpeculativeBackend::Mtp) &&
                          runtime.at("target_layout").at("kv_capacity_tokens") == 65536 &&
                          runtime.at("target_layout").at("sequence_capacity_bytes") ==
                              (32ULL << 20),
                      "runtime fingerprint omits a required identity or target layout field");

    SessionCheckpointManager disabled;
    const std::size_t source_size = source.size();
    failures += check(!disabled.enabled() &&
                          disabled.save(snapshot.client_session_sha256,
                                        snapshot.latest_response_id, source).state ==
                              SessionCheckpointSaveState::Disabled &&
                          disabled.restore(snapshot.client_session_sha256,
                                           snapshot.latest_response_id, source) ==
                              SessionCheckpointRestoreState::Disabled &&
                          disabled.status(snapshot.client_session_sha256).at("state") ==
                              "disabled" &&
                          disabled.erase(snapshot.client_session_sha256) ==
                              SessionCheckpointEraseResult::Missing &&
                          source.size() == source_size,
                      "disabled checkpoint manager changed ordinary Responses state");

    FakeCheckpointEngine exporter;
    SessionCheckpointManager manager(manager_options(temporary.path), runtime, tenant,
                                     exporter.access());
    failures += check(manager.save(snapshot.client_session_sha256, "resp_not_completed", source)
                                  .state == SessionCheckpointSaveState::Missing &&
                          exporter.checkpoint_calls == 0,
                      "checkpoint export was not bound to the completed stored response");
    const SessionCheckpointSaveOutcome saved =
        manager.save(snapshot.client_session_sha256, snapshot.latest_response_id, source);
    failures += check(saved.state == SessionCheckpointSaveState::Saved && saved.checkpoint &&
                          exporter.checkpoint_calls == 1 && exporter.tenant == tenant &&
                          exporter.session == snapshot.client_session_sha256 &&
                          exporter.tag == "stable-checkpoint-session-tag",
                      "production manager did not export the complete stable-tagged session");
    failures += check(manager.status(snapshot.client_session_sha256).at("state") ==
                          "available",
                      "manager status did not expose the published checkpoint");
    if (!saved.checkpoint) { return failures; }

    bool api_key_persisted = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(temporary.path)) {
        if (entry.is_regular_file() &&
            read_file_bytes(entry.path()).find(api_key) != std::string::npos) {
            api_key_persisted = true;
        }
    }
    failures += check(!api_key_persisted, "checkpoint files persisted the API key");

    FakeCheckpointEngine restorer;
    SessionCheckpointManager restarted(manager_options(temporary.path), runtime, tenant,
                                       restorer.access());
    ResponseStore restored_responses(8, 8ULL << 20);
    const SessionCheckpointRestoreState restored = restarted.restore(
        snapshot.client_session_sha256, snapshot.latest_response_id, restored_responses);
    const std::shared_ptr<const StoredResponse> required = restored_responses.get_for_session(
        snapshot.latest_response_id, snapshot.client_session_sha256);
    failures += check(restored == SessionCheckpointRestoreState::Restored && required &&
                          required->previous_response_id == "resp_private_first" &&
                          restorer.restore_calls == 1 && restorer.restored_payload == restorer.payload &&
                          restorer.tenant == tenant &&
                          restorer.tag == "stable-checkpoint-session-tag",
                      "restart did not atomically restore Engine and re-readable Responses state");

    FakeCheckpointEngine rejected_engine;
    rejected_engine.fail_restore = true;
    SessionCheckpointManager rejected(manager_options(temporary.path), runtime, tenant,
                                      rejected_engine.access());
    ResponseStore rejected_responses(8, 8ULL << 20);
    failures += check(rejected.restore(snapshot.client_session_sha256, snapshot.latest_response_id,
                                       rejected_responses) == SessionCheckpointRestoreState::Failed &&
                          rejected_engine.restore_calls == 1 && rejected_responses.size() == 0,
                      "failed Engine restore published partial Responses state");

    FakeCheckpointEngine wrong_response_engine;
    SessionCheckpointManager wrong_response(manager_options(temporary.path), runtime, tenant,
                                            wrong_response_engine.access());
    ResponseStore wrong_response_store(8, 8ULL << 20);
    failures += check(wrong_response.restore(snapshot.client_session_sha256, "resp_wrong",
                                             wrong_response_store) ==
                              SessionCheckpointRestoreState::Missing &&
                          wrong_response_engine.restore_calls == 0 && wrong_response_store.size() == 0,
                      "wrong response id reached Engine restore");

    FakeCheckpointEngine other_tenant_engine;
    SessionCheckpointManager other_tenant(
        manager_options(temporary.path), runtime,
        session_checkpoint_tenant_sha256("another-api-key"), other_tenant_engine.access());
    ResponseStore other_tenant_store(8, 8ULL << 20);
    failures += check(other_tenant.restore(snapshot.client_session_sha256,
                                           snapshot.latest_response_id, other_tenant_store) ==
                              SessionCheckpointRestoreState::Missing &&
                          other_tenant_engine.restore_calls == 0 && other_tenant_store.size() == 0,
                      "same session digest crossed an authenticated tenant namespace");

    const auto incompatible_restore = [&](const nlohmann::json& incompatible,
                                          const std::string& message) {
        FakeCheckpointEngine engine;
        SessionCheckpointManager drifted(manager_options(temporary.path), incompatible, tenant,
                                         engine.access());
        ResponseStore responses(8, 8ULL << 20);
        return check(drifted.restore(snapshot.client_session_sha256, snapshot.latest_response_id,
                                     responses) == SessionCheckpointRestoreState::Incompatible &&
                         engine.restore_calls == 0 && responses.size() == 0,
                     message);
    };
    BuildInfo architecture_drift = build;
    architecture_drift.cuda_architecture = "89";
    failures += incompatible_restore(production_fingerprint(architecture_drift),
                                     "CUDA architecture drift was restored");
    BuildInfo source_drift       = build;
    source_drift.patch_stack_sha = "different-patch";
    failures += incompatible_restore(production_fingerprint(source_drift),
                                     "patch stack drift was restored");
    BuildInfo profile_drift     = build;
    profile_drift.build_profile = "different-profile";
    failures += incompatible_restore(production_fingerprint(profile_drift),
                                     "build profile drift was restored");
    failures += incompatible_restore(production_fingerprint(build, (32ULL << 20) + 1),
                                     "target layout drift was restored");

    const AuthenticatedCheckpointNamespace checkpoint_namespace =
        AuthenticatedCheckpointNamespace::authenticated(tenant,
                                                        snapshot.client_session_sha256);
    const std::filesystem::path session =
        temporary.path / "sessions" / namespace_storage_digest(checkpoint_namespace);
    std::string generation = read_file_bytes(session / "current");
    generation.pop_back();
    std::fstream corrupt(session / "generations" / generation / "engine/state.bin",
                         std::ios::in | std::ios::out | std::ios::binary);
    char byte = 0;
    corrupt.read(&byte, 1);
    byte ^= 0x33;
    corrupt.seekp(0);
    corrupt.write(&byte, 1);
    corrupt.close();
    FakeCheckpointEngine corrupt_engine;
    SessionCheckpointManager corrupt_manager(manager_options(temporary.path), runtime, tenant,
                                             corrupt_engine.access());
    ResponseStore corrupt_responses(8, 8ULL << 20);
    failures += check(corrupt_manager.restore(snapshot.client_session_sha256,
                                              snapshot.latest_response_id, corrupt_responses) ==
                              SessionCheckpointRestoreState::Corrupt &&
                          corrupt_engine.restore_calls == 0 && corrupt_responses.size() == 0,
                      "corruption reached Engine or Responses publication");
    failures += check(manager.erase(snapshot.client_session_sha256) ==
                              SessionCheckpointEraseResult::Erased &&
                          manager.status(snapshot.client_session_sha256).at("state") ==
                              "missing" &&
                          manager.erase(snapshot.client_session_sha256) ==
                              SessionCheckpointEraseResult::Missing,
                      "manager namespace deletion did not remove the checkpoint atomically");
    return failures;
}

int test_delete_checkpoint_update_fails_closed() {
    TemporaryDirectory temporary;
    const std::string tenant = session_checkpoint_tenant_sha256("checkpoint-delete-api-key");
    const nlohmann::json runtime = fingerprint();
    ResponseStoreSnapshot snapshot = sample_snapshot();
    ResponseStore responses(8, 8ULL << 20);
    for (const StoredResponse& response : snapshot.records) { responses.put(response); }

    FakeCheckpointEngine engine;
    SessionCheckpointManager manager(manager_options(temporary.path), runtime, tenant,
                                     engine.access());
    const SessionCheckpointSaveOutcome baseline = manager.save(
        snapshot.client_session_sha256, snapshot.latest_response_id, responses);
    int failures = 0;
    failures += check(baseline.state == SessionCheckpointSaveState::Saved &&
                          baseline.checkpoint && engine.checkpoint_calls == 1,
                      "delete failure fixture did not publish its restart baseline");
    if (!baseline.checkpoint) { return failures; }

    const std::filesystem::path session = stored_session_path(temporary.path, snapshot);
    const std::string current_before = read_file_bytes(session / "current");
    engine.fail_checkpoint = true;
    const SessionCheckpointEraseResult failed = manager.erase_response(
        snapshot.client_session_sha256, snapshot.records.front().id, responses);
    const auto parent = responses.get_for_session(snapshot.records.front().id,
                                                  snapshot.client_session_sha256);
    const auto descendant = responses.get_for_session(snapshot.latest_response_id,
                                                      snapshot.client_session_sha256);
    failures += check(
        failed == SessionCheckpointEraseResult::Conflict && engine.checkpoint_calls == 2 &&
            parent && descendant && read_file_bytes(session / "current") == current_before,
        "failed post-delete checkpoint publication mutated memory or the durable current pointer");

    FakeCheckpointEngine restart_engine;
    SessionCheckpointManager restarted(manager_options(temporary.path), runtime, tenant,
                                       restart_engine.access());
    ResponseStore restarted_responses(8, 8ULL << 20);
    const SessionCheckpointRestoreState restored = restarted.restore(
        snapshot.client_session_sha256, snapshot.latest_response_id, restarted_responses);
    const auto restarted_descendant = restarted_responses.get_for_session(
        snapshot.latest_response_id, snapshot.client_session_sha256);
    failures += check(
        restored == SessionCheckpointRestoreState::Restored && restarted_descendant &&
            restarted_descendant->previous_response_id == snapshot.records.front().id &&
            restart_engine.restore_calls == 1,
        "restart after failed DELETE lost the surviving descendant checkpoint");

    engine.fail_checkpoint = false;
    const SessionCheckpointEraseResult erased = manager.erase_response(
        snapshot.client_session_sha256, snapshot.records.front().id, responses);
    failures += check(
        erased == SessionCheckpointEraseResult::Erased && engine.checkpoint_calls == 3 &&
            !responses.get_for_session(snapshot.records.front().id,
                                       snapshot.client_session_sha256) &&
            responses.get_for_session(snapshot.latest_response_id,
                                      snapshot.client_session_sha256) &&
            !std::filesystem::exists(session / "generations" /
                                     baseline.checkpoint->generation),
        "successful parent DELETE did not retain its in-memory descendant");

    FakeCheckpointEngine committed_restart_engine;
    SessionCheckpointManager committed_restart(manager_options(temporary.path), runtime, tenant,
                                               committed_restart_engine.access());
    ResponseStore committed_responses(8, 8ULL << 20);
    const SessionCheckpointRestoreState committed_restored = committed_restart.restore(
        snapshot.client_session_sha256, snapshot.latest_response_id, committed_responses);
    failures += check(
        committed_restored == SessionCheckpointRestoreState::Restored &&
            !committed_responses.get_for_session(snapshot.records.front().id,
                                                 snapshot.client_session_sha256) &&
            committed_responses.get_for_session(snapshot.latest_response_id,
                                                snapshot.client_session_sha256),
        "successful parent DELETE did not durably retain only its surviving descendant");
    return failures;
}

int test_delete_pins_session_across_cross_session_lru_eviction() {
    TemporaryDirectory temporary;
    const std::string tenant = session_checkpoint_tenant_sha256("checkpoint-delete-lru-api-key");
    const nlohmann::json runtime = fingerprint();
    ResponseStoreSnapshot snapshot = sample_snapshot();
    ResponseStore responses(3, 8ULL << 20);
    for (const StoredResponse& response : snapshot.records) { responses.put(response); }

    ResponseStoreSnapshot unrelated_snapshot = sample_snapshot('b');
    StoredResponse unrelated = unrelated_snapshot.records.front();
    unrelated.id = "resp_unrelated_lru_victim";
    unrelated.response["id"] = unrelated.id;
    responses.put(unrelated);

    FakeCheckpointEngine engine;
    SessionCheckpointManager manager(manager_options(temporary.path), runtime, tenant,
                                     engine.access());
    const SessionCheckpointSaveOutcome baseline = manager.save(
        snapshot.client_session_sha256, snapshot.latest_response_id, responses);
    int failures = 0;
    failures += check(baseline.state == SessionCheckpointSaveState::Saved && baseline.checkpoint,
                      "cross-session eviction fixture did not publish its restart baseline");
    if (!baseline.checkpoint) { return failures; }

    ResponseStoreSnapshot incoming_snapshot = sample_snapshot('c');
    StoredResponse incoming = incoming_snapshot.records.front();
    incoming.id = "resp_cross_session_insert";
    incoming.response["id"] = incoming.id;
    engine.checkpoint_hook = [&] { responses.put(incoming); };

    const SessionCheckpointEraseResult erased = manager.erase_response(
        snapshot.client_session_sha256, snapshot.records.front().id, responses);
    engine.checkpoint_hook = {};
    failures += check(
        erased == SessionCheckpointEraseResult::Erased &&
            !responses.get_for_session(snapshot.records.front().id,
                                       snapshot.client_session_sha256) &&
            responses.get_for_session(snapshot.latest_response_id,
                                      snapshot.client_session_sha256) &&
            !responses.get_for_session(unrelated.id,
                                       *unrelated.client_session_sha256) &&
            responses.get_for_session(incoming.id, *incoming.client_session_sha256),
        "cross-session LRU insertion evicted or conflicted with the checkpointed DELETE session");

    FakeCheckpointEngine restart_engine;
    SessionCheckpointManager restarted(manager_options(temporary.path), runtime, tenant,
                                       restart_engine.access());
    ResponseStore restarted_responses(3, 8ULL << 20);
    const SessionCheckpointRestoreState restored = restarted.restore(
        snapshot.client_session_sha256, snapshot.latest_response_id, restarted_responses);
    failures += check(
        restored == SessionCheckpointRestoreState::Restored &&
            !restarted_responses.get_for_session(snapshot.records.front().id,
                                                 snapshot.client_session_sha256) &&
            restarted_responses.get_for_session(snapshot.latest_response_id,
                                                snapshot.client_session_sha256),
        "restart disagreed with the successful DELETE after cross-session LRU pressure");
    return failures;
}

int test_delete_rejects_post_delete_generation_over_disk_quota() {
    TemporaryDirectory temporary;
    ResponseStoreSnapshot snapshot = sample_snapshot();
    ResponseStore responses(8, 8ULL << 20);
    for (const StoredResponse& response : snapshot.records) { responses.put(response); }
    FakeCheckpointEngine engine;
    SessionCheckpointStoreOptions options = manager_options(temporary.path);
    options.disk_quota_bytes = 1;
    SessionCheckpointManager manager(
        options, fingerprint(), session_checkpoint_tenant_sha256("delete-quota-api-key"),
        engine.access());

    const SessionCheckpointEraseResult result = manager.erase_response(
        snapshot.client_session_sha256, snapshot.records.front().id, responses);
    int failures = 0;
    failures += check(
        result == SessionCheckpointEraseResult::Conflict &&
            responses.get_for_session(snapshot.records.front().id,
                                      snapshot.client_session_sha256) &&
            responses.get_for_session(snapshot.latest_response_id,
                                      snapshot.client_session_sha256),
        "over-quota post-delete generation returned Erased or changed the live lineage");
    FakeCheckpointEngine restarted_engine;
    SessionCheckpointManager restarted(
        options, fingerprint(), session_checkpoint_tenant_sha256("delete-quota-api-key"),
        restarted_engine.access());
    ResponseStore restarted_responses(8, 8ULL << 20);
    failures += check(
        restarted.restore(snapshot.client_session_sha256, snapshot.latest_response_id,
                          restarted_responses) == SessionCheckpointRestoreState::Missing,
        "rejected over-quota DELETE exposed a partial durable generation");
    return failures;
}

int test_delete_survives_post_commit_directory_sync_failure() {
    TemporaryDirectory temporary;
    ResponseStoreSnapshot snapshot = sample_snapshot();
    ResponseStore responses(8, 8ULL << 20);
    for (const StoredResponse& response : snapshot.records) { responses.put(response); }
    const std::string tenant = session_checkpoint_tenant_sha256("delete-sync-api-key");
    FakeCheckpointEngine baseline_engine;
    SessionCheckpointManager baseline_manager(
        manager_options(temporary.path), fingerprint(), tenant, baseline_engine.access());
    const SessionCheckpointSaveOutcome baseline = baseline_manager.save(
        snapshot.client_session_sha256, snapshot.latest_response_id, responses);
    int failures = 0;
    failures += check(baseline.state == SessionCheckpointSaveState::Saved && baseline.checkpoint,
                      "directory-sync fixture baseline save failed");
    if (!baseline.checkpoint) { return failures; }

    int sync_calls = 0;
    SessionCheckpointStoreOptions options = manager_options(temporary.path);
    options.current_pointer_sync = [&](const std::filesystem::path&) {
        ++sync_calls;
        throw std::runtime_error("injected post-commit directory sync failure");
    };
    FakeCheckpointEngine engine;
    SessionCheckpointManager manager(options, fingerprint(), tenant, engine.access());
    failures += check(
        manager.erase_response(snapshot.client_session_sha256, snapshot.records.front().id,
                               responses) == SessionCheckpointEraseResult::Erased &&
            sync_calls == 1 &&
            !responses.get_for_session(snapshot.records.front().id,
                                       snapshot.client_session_sha256),
        "post-commit directory sync failure reported Conflict or retained the live response");
    FakeCheckpointEngine restart_engine;
    SessionCheckpointManager restarted(
        manager_options(temporary.path), fingerprint(), tenant, restart_engine.access());
    ResponseStore restarted_responses(8, 8ULL << 20);
    failures += check(
        restarted.restore(snapshot.client_session_sha256, snapshot.latest_response_id,
                          restarted_responses) == SessionCheckpointRestoreState::Restored &&
            !restarted_responses.get_for_session(snapshot.records.front().id,
                                                 snapshot.client_session_sha256) &&
            restarted_responses.get_for_session(snapshot.latest_response_id,
                                                snapshot.client_session_sha256),
        "restart disagreed with DELETE committed before directory sync failure");
    return failures;
}

int test_delete_restores_durable_response_after_live_lru_eviction() {
    TemporaryDirectory temporary;
    ResponseStoreSnapshot snapshot = sample_snapshot();
    ResponseStore responses(2, 8ULL << 20);
    for (const StoredResponse& response : snapshot.records) { responses.put(response); }
    const std::string tenant = session_checkpoint_tenant_sha256("delete-durable-lru-api-key");
    FakeCheckpointEngine engine;
    SessionCheckpointManager manager(
        manager_options(temporary.path), fingerprint(), tenant, engine.access());
    int failures = 0;
    failures += check(
        manager.save(snapshot.client_session_sha256, snapshot.latest_response_id, responses).state ==
            SessionCheckpointSaveState::Saved,
        "durable-only DELETE fixture baseline save failed");

    for (char digest_byte : {'b', 'c'}) {
        ResponseStoreSnapshot foreign = sample_snapshot(digest_byte);
        StoredResponse record = foreign.records.front();
        record.id = std::string("resp_foreign_") + digest_byte;
        record.response["id"] = record.id;
        responses.put(std::move(record));
    }
    failures += check(
        !responses.get_for_session(snapshot.records.front().id,
                                   snapshot.client_session_sha256) &&
            !responses.get_for_session(snapshot.latest_response_id,
                                       snapshot.client_session_sha256),
        "foreign-session LRU pressure did not evict the durable session from live memory");

    failures += check(
        manager.erase_response(snapshot.client_session_sha256, snapshot.records.front().id,
                               responses) == SessionCheckpointEraseResult::Erased &&
            !responses.get_for_session(snapshot.records.front().id,
                                       snapshot.client_session_sha256) &&
            responses.get_for_session(snapshot.latest_response_id,
                                      snapshot.client_session_sha256),
        "durable-only response DELETE reported Missing or failed to remove the restored response");
    FakeCheckpointEngine restart_engine;
    SessionCheckpointManager restarted(
        manager_options(temporary.path), fingerprint(), tenant, restart_engine.access());
    ResponseStore restarted_responses(2, 8ULL << 20);
    failures += check(
        restarted.restore(snapshot.client_session_sha256, snapshot.latest_response_id,
                          restarted_responses) == SessionCheckpointRestoreState::Restored &&
            !restarted_responses.get_for_session(snapshot.records.front().id,
                                                 snapshot.client_session_sha256),
        "durable-only deleted response resurrected after restart");
    return failures;
}

int test_delete_middle_latest_and_standalone_checkpoint_state() {
    TemporaryDirectory temporary;
    ResponseStoreSnapshot snapshot = sample_snapshot();
    StoredResponse third = snapshot.records.back();
    third.id = "resp_private_third";
    third.session_key = std::string(64, 'c');
    third.previous_response_id = snapshot.records.back().id;
    third.response["id"] = third.id;
    snapshot.latest_response_id = third.id;
    snapshot.records.push_back(third);
    ResponseStore responses(8, 8ULL << 20);
    for (const StoredResponse& response : snapshot.records) { responses.put(response); }
    const std::string tenant = session_checkpoint_tenant_sha256("delete-shapes-api-key");
    FakeCheckpointEngine engine;
    SessionCheckpointManager manager(manager_options(temporary.path), fingerprint(), tenant,
                                     engine.access());
    int failures = 0;
    failures += check(manager.save(snapshot.client_session_sha256, third.id, responses).state ==
                          SessionCheckpointSaveState::Saved,
                      "delete-shapes baseline save failed");

    const std::string middle_id = snapshot.records[1].id;
    failures += check(
        manager.erase_response(snapshot.client_session_sha256, middle_id, responses) ==
                SessionCheckpointEraseResult::Erased &&
            engine.tag == third.session_key &&
            !responses.get_for_session(middle_id, snapshot.client_session_sha256) &&
            responses.get_for_session(third.id, snapshot.client_session_sha256),
        "middle DELETE did not checkpoint the surviving latest engine frontier");
    FakeCheckpointEngine middle_restart_engine;
    SessionCheckpointManager middle_restart(manager_options(temporary.path), fingerprint(), tenant,
                                            middle_restart_engine.access());
    ResponseStore middle_restart_responses(8, 8ULL << 20);
    failures += check(
        middle_restart.restore(snapshot.client_session_sha256, third.id,
                               middle_restart_responses) ==
                SessionCheckpointRestoreState::Restored &&
            !middle_restart_responses.get_for_session(middle_id,
                                                      snapshot.client_session_sha256) &&
            middle_restart_responses.get_for_session(third.id,
                                                     snapshot.client_session_sha256),
        "middle DELETE transcript state reappeared after restart");

    const std::string first_id = snapshot.records.front().id;
    failures += check(
        manager.erase_response(snapshot.client_session_sha256, third.id, responses) ==
                SessionCheckpointEraseResult::Erased &&
            engine.tag == snapshot.records.front().session_key &&
            responses.get_for_session(first_id, snapshot.client_session_sha256) &&
            !responses.get_for_session(third.id, snapshot.client_session_sha256),
        "latest DELETE did not checkpoint the prior surviving response frontier");
    FakeCheckpointEngine latest_restart_engine;
    SessionCheckpointManager latest_restart(manager_options(temporary.path), fingerprint(), tenant,
                                            latest_restart_engine.access());
    ResponseStore latest_restart_responses(8, 8ULL << 20);
    failures += check(
        latest_restart.restore(snapshot.client_session_sha256, first_id,
                               latest_restart_responses) ==
                SessionCheckpointRestoreState::Restored &&
            latest_restart.restore(snapshot.client_session_sha256, third.id,
                                   latest_restart_responses) ==
                SessionCheckpointRestoreState::Missing,
        "deleted latest response remained durably restorable");

    failures += check(
        manager.erase_response(snapshot.client_session_sha256, first_id, responses) ==
            SessionCheckpointEraseResult::Erased,
        "standalone DELETE did not erase the session checkpoint");
    FakeCheckpointEngine empty_restart_engine;
    SessionCheckpointManager empty_restart(manager_options(temporary.path), fingerprint(), tenant,
                                           empty_restart_engine.access());
    ResponseStore empty_restart_responses(8, 8ULL << 20);
    failures += check(
        empty_restart.restore(snapshot.client_session_sha256, first_id,
                              empty_restart_responses) ==
                SessionCheckpointRestoreState::Missing &&
            empty_restart_responses.size() == 0,
        "erased standalone response remained durably restorable");
    return failures;
}

int test_active_reader_delete_and_gc() {
    TemporaryDirectory temporary;
    const ResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload = engine_payload();
    bool refuse_cleanup = false;
    SessionCheckpointStore store(
        {.root = temporary.path,
         .disk_quota_bytes = 64ULL << 10,
         .staging_bytes = 1ULL << 20,
         .tombstone_cleanup = [&](const std::filesystem::path& path) {
             if (refuse_cleanup) { return false; }
             std::error_code error;
             std::filesystem::remove_all(path, error);
             return !error;
         }});
    const auto exporter = [&](ContinuationCheckpointWriter& writer)
        -> std::optional<ContinuationCheckpointStats> {
        if (!write_chunked(writer, "engine/state.bin", payload)) { return std::nullopt; }
        return ContinuationCheckpointStats{.frontier_tokens = 4096,
                                           .restored_tokens = 4096,
                                           .payload_bytes = payload.size()};
    };
    int failures = 0;
    const auto saved =
        store.save(checkpoint_namespace(responses), responses, fingerprint(), exporter);
    failures += check(saved.has_value(), "GC fixture generation saves");
    if (!saved) { return failures; }
    const std::filesystem::path session = stored_session_path(temporary.path, responses);
    SessionCheckpointLoadResult loaded =
        store.load(checkpoint_namespace(responses), fingerprint());
    const SessionCheckpointEraseResult conflict = store.erase(checkpoint_namespace(responses));
    failures += check(loaded.state == SessionCheckpointLoadState::Available && loaded.checkpoint &&
                          conflict == SessionCheckpointEraseResult::Conflict &&
                          std::filesystem::is_regular_file(
                              session / "generations" / saved->generation / "responses.cbor") &&
                          std::filesystem::is_regular_file(
                              session / "generations" / saved->generation / "engine/state.bin") &&
                          store.status(checkpoint_namespace(responses), fingerprint()).at("state") ==
                              "available",
                      "refused deletion reports conflict and preserves transcript and Engine state");

    const auto newer =
        store.save(checkpoint_namespace(responses), responses, fingerprint(), exporter);
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
    failures += check(!std::filesystem::exists(stale) &&
                          std::filesystem::exists(session / "generations" /
                                                  saved->generation) &&
                          std::filesystem::exists(session / "generations" /
                                                  newer->generation) &&
                          std::filesystem::exists(session / "current"),
                      "quota GC removes stale LRU data but protects active and current generations");
    loaded.checkpoint.reset();
    const std::filesystem::path occupied_tombstone =
        temporary.path / ".tombstones" /
        namespace_storage_digest(checkpoint_namespace(responses));
    std::filesystem::create_directories(occupied_tombstone);
    std::ofstream(occupied_tombstone / "occupied", std::ios::binary) << "occupied";
    const SessionCheckpointEraseResult collision_safe_erase =
        store.erase(checkpoint_namespace(responses));
    failures += check(
        collision_safe_erase == SessionCheckpointEraseResult::Erased &&
            !std::filesystem::exists(session) &&
            std::filesystem::is_directory(occupied_tombstone),
        "an occupied prior tombstone blocked collision-free session deletion");

    const auto resaved =
        store.save(checkpoint_namespace(responses), responses, fingerprint(), exporter);
    failures += check(resaved.has_value() && std::filesystem::is_directory(session),
                      "a retained tombstone blocked reuse of the live session namespace");
    if (!resaved) { return failures; }

    refuse_cleanup = true;
    failures += check(store.erase(checkpoint_namespace(responses)) ==
                              SessionCheckpointEraseResult::Erased &&
                          !std::filesystem::exists(session) &&
                          !std::filesystem::is_empty(temporary.path / ".tombstones") &&
                          store.status(checkpoint_namespace(responses), fingerprint()).at("state") ==
                              "missing" &&
                          store.erase(checkpoint_namespace(responses)) ==
                              SessionCheckpointEraseResult::Missing,
                      "successful rename is deleted even when physical cleanup is deferred");
    refuse_cleanup = false;
    store.collect_garbage();
    failures += check(std::filesystem::is_empty(temporary.path / ".tombstones"),
                      "garbage collection removes occupied and deferred session tombstones");
    return failures;
}

int test_store_wide_quota_across_sessions() {
    const ResponseStoreSnapshot first_session  = sample_snapshot('a');
    const ResponseStoreSnapshot second_session = sample_snapshot('b');
    const ResponseStoreSnapshot third_session  = sample_snapshot('c');
    const std::vector<std::byte> payload = engine_payload();
    const auto exporter = [&](ContinuationCheckpointWriter& writer)
        -> std::optional<ContinuationCheckpointStats> {
        if (!write_chunked(writer, "engine/state.bin", payload)) { return std::nullopt; }
        return ContinuationCheckpointStats{.frontier_tokens = 4096,
                                           .restored_tokens = 4096,
                                           .payload_bytes = payload.size()};
    };

    TemporaryDirectory measurement;
    SessionCheckpointStore measuring_store({.root = measurement.path,
                                            .disk_quota_bytes = 1ULL << 20,
                                            .staging_bytes = 1ULL << 20,
                                            .tombstone_cleanup = {}});
    const auto measured = measuring_store.save(checkpoint_namespace(first_session), first_session,
                                               fingerprint(), exporter);
    int failures = 0;
    failures += check(measured.has_value(), "quota fixture generation size is measurable");
    if (!measured) { return failures; }

    const std::uint64_t quota = measured->bytes * 2;
    TemporaryDirectory temporary;
    SessionCheckpointStore store({.root = temporary.path,
                                  .disk_quota_bytes = quota,
                                  .staging_bytes = 1ULL << 20,
                                  .tombstone_cleanup = {}});
    const auto first =
        store.save(checkpoint_namespace(first_session), first_session, fingerprint(), exporter);
    const auto second =
        store.save(checkpoint_namespace(second_session), second_session, fingerprint(), exporter);
    const auto third =
        store.save(checkpoint_namespace(third_session), third_session, fingerprint(), exporter);
    failures += check(first && second && third, "three sessions publish under a two-generation cap");
    if (!first || !second || !third) { return failures; }

    const nlohmann::json first_status =
        store.status(checkpoint_namespace(first_session), fingerprint());
    const nlohmann::json second_status =
        store.status(checkpoint_namespace(second_session), fingerprint());
    const nlohmann::json third_status =
        store.status(checkpoint_namespace(third_session), fingerprint());
    failures += check(first_status.at("state") == "missing" &&
                          second_status.at("state") == "available" &&
                          third_status.at("state") == "available",
                      "deterministic quota eviction removes the oldest inactive current session");
    failures += check(retained_generation_bytes(temporary.path) <= quota,
                      "resident generations stay within the store-wide disk quota");
    failures += check(
        !std::filesystem::exists(stored_session_path(temporary.path, first_session) / "current") &&
            std::filesystem::is_regular_file(stored_session_path(temporary.path, second_session) /
                                             "current") &&
            std::filesystem::is_regular_file(stored_session_path(temporary.path, third_session) /
                                             "current"),
        "eviction removes its current pointer while retained sessions keep valid pointers");
    TemporaryDirectory protected_temporary;
    SessionCheckpointStore protected_store({.root = protected_temporary.path,
                                             .disk_quota_bytes = measured->bytes,
                                             .staging_bytes = 1ULL << 20,
                                             .tombstone_cleanup = {}});
    const auto protected_saved =
        protected_store.save(checkpoint_namespace(first_session), first_session, fingerprint(),
                             exporter);
    SessionCheckpointLoadResult active =
        protected_store.load(checkpoint_namespace(first_session), fingerprint());
    const auto refused = protected_store.save(checkpoint_namespace(second_session), second_session,
                                              fingerprint(), exporter);
    failures += check(protected_saved && active.checkpoint && !refused &&
                          protected_store
                                  .status(checkpoint_namespace(first_session), fingerprint())
                                  .at("state") == "available" &&
                          protected_store
                                  .status(checkpoint_namespace(second_session), fingerprint())
                                  .at("state") == "missing" &&
                          retained_generation_bytes(protected_temporary.path) <= measured->bytes,
                      "active reader ownership rejects admission without exceeding the quota");

    TemporaryDirectory failure_temporary;
    bool refuse_cleanup = false;
    SessionCheckpointStore failure_store(
        {.root = failure_temporary.path,
         .disk_quota_bytes = quota,
         .staging_bytes = 1ULL << 20,
         .tombstone_cleanup = [&](const std::filesystem::path& path) {
             if (refuse_cleanup) { return false; }
             std::error_code error;
             std::filesystem::remove_all(path, error);
             return !error;
         }});
    const auto failure_first = failure_store.save(
        checkpoint_namespace(first_session), first_session, fingerprint(), exporter);
    const auto failure_previous = failure_store.save(
        checkpoint_namespace(second_session), second_session, fingerprint(), exporter);
    failures += check(failure_first && failure_previous,
                      "quota failure fixture publishes two current sessions");
    if (!failure_first || !failure_previous) { return failures; }
    const std::filesystem::path previous_session =
        stored_session_path(failure_temporary.path, second_session);
    const std::filesystem::path previous_generation =
        previous_session / "generations" / failure_previous->generation;
    const std::string previous_current = read_file_bytes(previous_session / "current");
    const std::string previous_manifest = read_file_bytes(previous_generation / "manifest.json");
    const std::string previous_responses = read_file_bytes(previous_generation / "responses.cbor");
    const std::string previous_engine = read_file_bytes(previous_generation / "engine/state.bin");

    refuse_cleanup = true;
    const auto failed_replacement = failure_store.save(
        checkpoint_namespace(second_session), second_session, fingerprint(), exporter);
    SessionCheckpointLoadResult previous_loaded =
        failure_store.load(checkpoint_namespace(second_session), fingerprint());
    std::optional<std::filesystem::path> deferred_tombstone;
    const std::string deferred_prefix =
        namespace_storage_digest(checkpoint_namespace(first_session)) + "--session--";
    for (const auto& entry :
         std::filesystem::directory_iterator(failure_temporary.path / ".tombstones")) {
        if (entry.path().filename().string().starts_with(deferred_prefix)) {
            deferred_tombstone = entry.path();
            break;
        }
    }
    failures += check(
        !failed_replacement && previous_loaded.state == SessionCheckpointLoadState::Available &&
            previous_loaded.checkpoint &&
            previous_loaded.checkpoint->generation == failure_previous->generation &&
            read_file_bytes(previous_session / "current") == previous_current &&
            read_file_bytes(previous_generation / "manifest.json") == previous_manifest &&
            read_file_bytes(previous_generation / "responses.cbor") == previous_responses &&
            read_file_bytes(previous_generation / "engine/state.bin") == previous_engine &&
            std::filesystem::is_directory(previous_generation) &&
            deferred_tombstone && std::filesystem::is_directory(*deferred_tombstone) &&
            regular_file_bytes(*deferred_tombstone / "generations") ==
                failure_first->bytes &&
            retained_generation_bytes(failure_temporary.path) ==
                failure_first->bytes + failure_previous->bytes,
        "cleanup failure rejects save before publication and preserves the prior current bytes");
    previous_loaded.checkpoint.reset();
    refuse_cleanup = false;
    failure_store.collect_garbage();
    failures += check(deferred_tombstone && !std::filesystem::exists(*deferred_tombstone) &&
                          failure_store
                                  .status(checkpoint_namespace(second_session), fingerprint())
                                  .at("state") == "available" &&
                          retained_generation_bytes(failure_temporary.path) ==
                              failure_previous->bytes,
                      "deferred tombstones remain quota-accounted and are reclaimed by GC");
    return failures;
}

int test_load_scan_failure_does_not_deadlock() {
    TemporaryDirectory temporary;
    const ResponseStoreSnapshot responses = sample_snapshot();
    const std::vector<std::byte> payload  = engine_payload();
    SessionCheckpointStore store({
        .root             = temporary.path,
        .disk_quota_bytes = 8ULL << 20,
        .staging_bytes    = 1ULL << 20,
        .generation_size  = [](const std::filesystem::path&) -> std::uint64_t {
            throw std::runtime_error("injected directory scan failure");
        },
    });
    const auto saved = store.save(
        checkpoint_namespace(responses), responses, fingerprint(),
        [&](ContinuationCheckpointWriter& writer) -> std::optional<ContinuationCheckpointStats> {
            if (!write_chunked(writer, "engine/state.bin", payload)) { return std::nullopt; }
            return ContinuationCheckpointStats{
                .frontier_tokens = 4096,
                .restored_tokens = 4096,
                .payload_bytes   = payload.size(),
            };
        });
    if (!saved) { return check(false, "directory-scan fixture did not save"); }
    const SessionCheckpointLoadResult loaded =
        store.load(checkpoint_namespace(responses), fingerprint());
    return check(loaded.state == SessionCheckpointLoadState::Unavailable && !loaded.checkpoint,
                 "generation scan failure did not unwind before reader ownership");
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "usage: " << argv[0] << " [test-name]\n";
        return 2;
    }
    const std::array tests{
        std::pair{"sha256", &test_sha256_streaming},
        std::pair{"namespace", &test_authenticated_namespace_validation},
        std::pair{"codec", &test_codec_round_trip},
        std::pair{"transaction", &test_transaction_restart_compatibility_and_corruption},
        std::pair{"manager-restart", &test_production_manager_restart_and_identity_isolation},
        std::pair{"delete-failure", &test_delete_checkpoint_update_fails_closed},
        std::pair{"delete-lru-pin", &test_delete_pins_session_across_cross_session_lru_eviction},
        std::pair{"delete-quota", &test_delete_rejects_post_delete_generation_over_disk_quota},
        std::pair{"delete-sync", &test_delete_survives_post_commit_directory_sync_failure},
        std::pair{"delete-evicted", &test_delete_restores_durable_response_after_live_lru_eviction},
        std::pair{"delete-states", &test_delete_middle_latest_and_standalone_checkpoint_state},
        std::pair{"active-reader", &test_active_reader_delete_and_gc},
        std::pair{"quota", &test_store_wide_quota_across_sessions},
        std::pair{"load-unwind", &test_load_scan_failure_does_not_deadlock},
    };
    const std::string_view filter = argc == 2 ? argv[1] : "";
    int failures = 0;
    bool matched = filter.empty();
    for (const auto& [name, test] : tests) {
        if (!filter.empty() && filter != name) { continue; }
        matched = true;
        std::cout << "RUN " << name << '\n' << std::flush;
        failures += test();
    }
    if (!matched) {
        std::cerr << "unknown test: " << filter << '\n';
        return 2;
    }
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
