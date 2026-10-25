#include "core/device.h"
#include "core/sha256.h"
#include "models/qwen3_5/execution/parameters.h"
#include "models/common/qwen/frontend/frontend.h"
#include "models/common/qwen/frontend/resources.h"
#include "models/qwen3_5/load.h"
#include "models/qwen3_5/program/program.h"
#include "models/qwen3_5/program/program_impl.h"
#include "runtime/contract/continuation_checkpoint.h"
#include <nlohmann/json.hpp>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {
using namespace ninfer;
using namespace runtime;
namespace q35 = models::qwen3_5;
namespace fs = std::filesystem;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::uint64_t pid() {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}
Json read_json(const fs::path& path) { std::ifstream stream(path); return Json::parse(stream); }
void write_json(const fs::path& path, const Json& value) {
    std::ofstream stream(path); stream << value.dump(); require(stream.good(), "cannot write test metadata");
}
struct File {
    DurablePayload descriptor;
    std::uint64_t consumed = 0;
    crypto::Sha256 hash;
};
class Writer final : public ContinuationCheckpointWriter {
public:
    explicit Writer(fs::path root) : root_(std::move(root)) {}
    bool write_file(std::string_view path, std::uint64_t offset, std::uint64_t total,
                    std::span<const std::byte> bytes, std::uint32_t alignment) override {
        if (finished_) return false;
        auto [it, inserted] = files_.try_emplace(std::string(path));
        auto& file = it->second;
        if (inserted) file.descriptor = {std::string(path), total, {}, alignment};
        if (offset != file.consumed || total != file.descriptor.bytes ||
            alignment != file.descriptor.alignment || offset > total || bytes.size() > total - offset)
            return false;
        const auto destination = root_ / path;
        fs::create_directories(destination.parent_path());
        std::ofstream stream(destination, std::ios::binary | (offset ? std::ios::app : std::ios::trunc));
        stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); stream.close();
        if (!stream.good()) return false;
        file.hash.update(bytes); file.consumed += bytes.size(); return true;
    }
    bool finish() override {
        if (finished_) return true;
        Json inventory = Json::array();
        for (auto& [path, file] : files_) {
            if (file.consumed != file.descriptor.bytes) return false;
            file.descriptor.sha256 = crypto::sha256_hex(file.hash.finish());
            inventory.push_back({{"path", path}, {"bytes", file.descriptor.bytes},
                {"alignment", file.descriptor.alignment}, {"sha256", file.descriptor.sha256}});
        }
        write_json(root_ / "test-inventory.json", inventory); finished_ = true; return true;
    }
private:
    fs::path root_;
    std::map<std::string, File, std::less<>> files_;
    bool finished_ = false;
};
class Reader final : public ContinuationCheckpointReader {
public:
    explicit Reader(fs::path root, bool corrupt = false,
                    DurableReason failure = DurableReason::None, bool fail_at_finish = false)
        : root_(std::move(root)), corrupt_(corrupt), failure_(failure), fail_at_finish_(fail_at_finish) {
        for (const auto& item : read_json(root_ / "test-inventory.json")) {
            File file;
            file.descriptor = {item.at("path"), item.at("bytes"), item.at("sha256"), item.at("alignment")};
            auto source = std::make_shared<InputSource>(root_ / file.descriptor.path);
            files_.emplace(item.at("path").get<std::string>(),
                           InputFile(std::move(file), std::move(source)));
        }
    }
    Reader(const Reader& previous)
        : root_(previous.root_), corrupt_(previous.corrupt_), failure_(previous.failure_),
          fail_at_finish_(previous.fail_at_finish_) {
        for (const auto& [path, input] : previous.files_) {
            File file;
            file.descriptor = input.descriptor;
            files_.emplace(path, InputFile(std::move(file), input.source));
        }
    }
    std::optional<std::uint64_t> file_size(std::string_view path) const override {
        const auto found = file_descriptor(path);
        return found ? std::optional(found->bytes) : std::nullopt;
    }
    std::optional<DurablePayload> file_descriptor(std::string_view path) const override {
        const auto found = files_.find(path);
        return found == files_.end() ? std::nullopt : std::optional(found->second.descriptor);
    }
    bool read_file(std::string_view path, std::uint64_t offset, std::span<std::byte> bytes) const override {
        if (path != "engine/index.cbor" && failure_ != DurableReason::None && !fail_at_finish_)
            return fail(failure_);
        auto found = files_.find(path);
        if (found == files_.end()) return fail(DurableReason::PayloadSizeMismatch);
        auto& file = found->second;
        if (offset != file.consumed || offset > file.descriptor.bytes || bytes.size() > file.descriptor.bytes - offset)
            return fail(DurableReason::PayloadSizeMismatch);
        std::lock_guard guard(file.source->mutex);
        auto& stream = file.source->stream;
        if (!stream) return fail(DurableReason::IoUnavailable);
        stream.clear();
        stream.seekg(offset);
        stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (stream.bad()) return fail(DurableReason::IoUnavailable);
        if (stream.gcount() != static_cast<std::streamsize>(bytes.size()))
            return fail(DurableReason::PayloadSizeMismatch);
        if (corrupt_ && path != "engine/index.cbor" && !bytes.empty()) bytes[0] ^= std::byte{1};
        file.hash.update(bytes); file.consumed += bytes.size();
        if (file.consumed == file.descriptor.bytes)
            file.actual = crypto::sha256_hex(file.hash.finish());
        return true;
    }
    bool verify_complete() const override {
        if (failure_ != DurableReason::None && fail_at_finish_) return fail(failure_);
        for (const auto& [path, file] : files_) {
            if (file.consumed != file.descriptor.bytes) return fail(DurableReason::PayloadSizeMismatch);
            if (file.actual != file.descriptor.sha256) return fail(DurableReason::PayloadDigestMismatch);
        }
        return true;
    }
    DurableReason failure_reason() const noexcept override { return last_failure_; }
    std::shared_ptr<const ContinuationCheckpointReader> fresh_reader() const override {
        if (last_failure_ == DurableReason::PayloadDigestMismatch ||
            last_failure_ == DurableReason::PayloadSizeMismatch) return {};
        return std::make_shared<Reader>(*this);
    }
    DurableCheckpointSet index() const {
        const auto size = file_size("engine/index.cbor").value();
        require(size <= kDurableManifestMaxBytes, "test metadata unexpectedly oversized");
        std::vector<std::byte> bytes(size);
        require(read_file("engine/index.cbor", 0, bytes), "index read failed");
        return q35::Program::decode_checkpoint_index(bytes, {});
    }
private:
    bool fail(DurableReason reason) const noexcept { last_failure_ = reason; return false; }
    struct InputSource {
        explicit InputSource(const fs::path& path) : stream(path, std::ios::binary) {}
        std::ifstream stream;
        std::mutex mutex;
    };
    struct InputFile : File {
        std::string actual;
        std::shared_ptr<InputSource> source;
        InputFile(File value, std::shared_ptr<InputSource> input)
            : File(std::move(value)), source(std::move(input)) {}
    };
    fs::path root_;
    mutable std::map<std::string, InputFile, std::less<>> files_;
    bool corrupt_;
    DurableReason failure_;
    bool fail_at_finish_;
    mutable DurableReason last_failure_ = DurableReason::None;
};
void settle(DeviceContext& device, q35::Program& program, q35::ContextProgress& result) {
    for (int i = 0; i < 8; ++i) {
        device.synchronize(); CUDA_CHECK(cudaStreamSynchronize(device.transfer_stream()));
        result = program.poll_context({}); if (result.complete) return;
    }
    throw std::runtime_error("native transaction did not settle");
}
struct Generated {
    std::vector<TokenId> tokens;
    std::vector<q35::CheckpointHandle> points;
    std::uint32_t reused = 0;
};
Generated generate(DeviceContext& device, q35::Program& program,
                   models::qwen::PreparedPrompt prompt, std::optional<q35::CheckpointHandle> source,
                   bool capture, std::string_view scenario = "terminal") {
    ResolvedExecutionOptions options;
    options.requested_output_tokens = scenario == "terminal" ? 8 : 24;
    options.allow_prefix_reuse = true;
    options.sampling.temperature = 0; options.sampling.top_k = 1;
    auto base = program.plan_request(std::move(prompt), options);
    auto candidate = program.inspect_source(base, source);
    require(candidate.has_value(), "matching native source missing");
    Generated result; result.reused = candidate->reused_tokens;
    require(static_cast<bool>(program.start_binding(base, {0}, *candidate)), "native source binding refused");
    q35::ContextProgress bound; settle(device, program, bound);
    require(bound.sequence.has_value(), "native binding did not publish sequence");
    auto sequence = *bound.sequence;
    const auto reserve = [&](q35::ExecutionUnitKind kind, std::uint32_t budget = 0) {
        const std::array<q35::ExecutionUnit, 1> units{{{sequence, kind, budget}}};
        require(static_cast<bool>(program.reserve_units(units)), "native unit reservation refused");
    };
    const auto capture_point = [&] {
        if (!capture) { program.skip_capture(sequence); return; }
        require(program.start_capture(sequence), "native input capture refused");
        q35::ContextProgress progress; settle(device, program, progress);
        require(progress.published && !progress.captured_checkpoints.empty(), "input capture did not publish");
        result.points.insert(result.points.end(), progress.captured_checkpoints.begin(), progress.captured_checkpoints.end());
    };
    bool began = false;
    for (unsigned step = 0; step < 16; ++step) {
        reserve(q35::ExecutionUnitKind::Prefill);
        auto progress = program.advance_prefill(sequence);
        if (progress.capture_ready) capture_point();
        if (!progress.complete) continue;
        require(progress.pending && !progress.pending->tokens().empty(), "prefill has no sampled token");
        result.tokens.push_back(progress.pending->tokens().front());
        const std::array<CommitDecision, 1> accept{{{.accepted_tokens = 1}}};
        const auto committed = program.commit(std::move(*progress.pending), accept);
        if (committed.capture_ready[0]) capture_point();
        began = true; break;
    }
    require(began, "prefill failed to finish");
    const auto target = scenario == "terminal" ? 8U : 24U;
    bool exercised = scenario == "terminal";
    while (result.tokens.size() < target) {
        const auto remaining = static_cast<std::uint32_t>(target - result.tokens.size());
        reserve(q35::ExecutionUnitKind::Decode, remaining);
        const std::array members{sequence};
        const std::array<RoundBudget, 1> budgets{{{.generated_tokens_remaining = remaining}}};
        auto pending = program.decode(members, budgets);
        auto count = pending.row_counts().empty() ? 1U : static_cast<std::uint32_t>(pending.row_counts()[0]);
        require(count && count <= remaining, "decode exceeded requested tokens");
        const bool partial = scenario == "partial" && !exercised && count > 1;
        if (partial) count = 1;
        result.tokens.insert(result.tokens.end(), pending.tokens().begin(), pending.tokens().begin() + count);
        const bool terminal = result.tokens.size() == target;
        const std::array<CommitDecision, 1> accept{{{.accepted_tokens = count, .terminal = terminal}}};
        (void)program.commit(std::move(pending), accept);
        if (partial) exercised = true;
        if (!terminal && !exercised && scenario == "forced") {
            const std::array<TokenId, 3> forced{198, 198, 199};
            const std::array<std::optional<std::uint32_t>, 1> splits{std::nullopt};
            reserve(q35::ExecutionUnitKind::Control, forced.size());
            (void)program.append_forced_tokens(members, forced, forced.size(), splits);
            result.tokens.insert(result.tokens.end(), forced.begin(), forced.end());
            exercised = true;
        }
        if (!terminal && !exercised && scenario == "pause") {
            const auto* native = static_cast<const q35::detail::ProgramImpl*>(
                q35::detail::ContractAccess::owner(sequence));
            const auto lane = q35::detail::ContractAccess::lane(sequence).value;
            const auto before = native->sequences[lane].continuation.metadata;
            if (native->speculative_backend == SpeculativeBackend::DFlash2)
                require(before.dflash_context_frontier < before.execution_frontier,
                        "pause did not exercise an unsettled draft frontier");
            require(program.start_pause(sequence, true), "draft pause refused");
            q35::ContextProgress paused; settle(device, program, paused);
            require(paused.paused && paused.paused->has_snapshot(), "draft pause lost its snapshot");
            auto root = program.inspect_source(base, std::nullopt);
            require(root && static_cast<bool>(program.start_binding(base, {0}, *root, &*paused.paused)),
                    "draft snapshot resume refused");
            q35::ContextProgress resumed; settle(device, program, resumed);
            require(resumed.sequence.has_value(), "draft snapshot resume did not publish");
            sequence = *resumed.sequence;
            exercised = true;
        }
    }
    require(exercised, "requested draft regression transition was not exercised");
    const auto finish = program.finish(sequence);
    require(finish.status == ConsumeStatus::Consumed && finish.checkpoint, "native endpoint not retained");
    result.points.push_back(*finish.checkpoint);
    return result;
}
models::qwen::PreparedPrompt seed_prompt(models::qwen::Frontend& frontend) {
    PromptInput input;
    input.options.enable_thinking = false;
    std::string prefix, suffix;
    for (unsigned i = 0; i < 130; ++i) prefix += "alpha ";
    for (unsigned i = 0; i < 140; ++i) suffix += "beta ";
    input.messages.push_back({.role = ChatRole::System,
        .parts = {{.kind = MessagePartKind::Text, .text = std::move(prefix)}}});
    input.messages.push_back({.role = ChatRole::User,
        .parts = {{.kind = MessagePartKind::Text, .text = std::move(suffix)}}});
    input.context_cache.allow_engine_automatic_shared_prefixes = false;
    input.context_cache.markers.push_back({.after_message_count = 1,
        .kind = PromptCacheMarkerKind::PrivateLongAnchor,
        .location = PromptCacheMarkerLocation::MessageBoundary});
    return frontend.prepare(std::move(input));
}
void release(q35::Program& program, const std::vector<q35::CheckpointHandle>& points) {
    for (auto point : points) require(program.release_checkpoint(point), "checkpoint ownership leaked");
}
} // namespace
int main(int argc, char** argv) {
    const char* artifact = std::getenv("NINFER_TEST_ARTIFACT");
    if (!artifact || !*artifact) { std::cout << "SKIP: NINFER_TEST_ARTIFACT is required\n"; return 77; }
    try {
        require(argc == 4, "usage: checkpoint_restart_test export|import DIRECTORY none|mtp|dflash2");
        const std::string mode = argv[1], backend_name = argv[3];
        require(mode == "export" || mode == "import", "invalid restart test mode");
        const auto backend = backend_name == "none" ? SpeculativeBackend::None :
            backend_name == "mtp" ? SpeculativeBackend::Mtp : SpeculativeBackend::DFlash2;
        require(backend_name == "none" || backend_name == "mtp" || backend_name == "dflash2", "invalid backend");
        const fs::path directory(argv[2]);
        DeviceContext device;
        models::LoadOptions selected; selected.speculative = backend;
        auto model = q35::load_model(artifact, selected, device);
        q35::execution::Parameters parameters(*model);
        auto frontend = models::qwen::make_frontend(model->resources(), {.vision_enabled = false, .max_context = 1024},
            q35::default_sampling(model->config().text.architecture));
        EngineOptions options;
        options.max_context = 1024; options.kv_capacity = KvCapacityPolicy::explicit_capacity(1024);
        options.prefill_chunk = 128; options.max_concurrency = 1; options.kv_cache = KvCacheStorage::BFloat16;
        options.use_cuda_graph = false; options.speculative.backend = backend;
        options.speculative.draft_tokens = backend == SpeculativeBackend::None ? 0 : 7;
        options.context_cache.device_state_slots = 3; options.context_cache.host_capacity_bytes = 1ULL << 30;
        const auto make_program = [&] {
            auto planner = q35::make_sequence_planner(parameters, device, options);
            return q35::create_program(parameters, std::move(planner).finalize(16), device, {});
        };
        auto program = make_program();
        const auto native_layout = program->checkpoint_layout();
        const auto decoded = [&](const std::vector<TokenId>& tokens) {
            std::string bytes;
            for (auto token : tokens) bytes += model->resources().tokenizer->decode_token_bytes(token);
            return bytes;
        };
        const std::vector<std::string> scenarios = backend == SpeculativeBackend::DFlash2
            ? std::vector<std::string>{"terminal", "partial", "forced", "pause"}
            : std::vector<std::string>{"terminal"};
        for (const auto& scenario : scenarios) {
        const auto root = directory / scenario;
        if (mode == "export") {
            require(!fs::exists(root), "restart export directory already exists"); fs::create_directories(root);
            auto seed = generate(device, *program, seed_prompt(frontend), std::nullopt, true, scenario);
            require(seed.points.size() == 3, "test did not retain anchor, input and endpoint");
            DurableCheckpointStats stats;
            DurableCheckpointSet index;
            {
                auto capture = program->prepare_checkpoint_export(seed.points, {});
                require(capture.state() == DurablePrepareState::Prepared, "native set export refused");
                Writer writer(root);
                const auto exported = capture.write(writer);
                require(exported.state == DurableExportState::Exported, "native payload write refused");
                stats = exported.stats;
                index = capture.index();
                require(index.points.size() == 3 && !index.histories.empty() &&
                        index.histories.size() <= index.points.size() * native_layout.pools.size(),
                        "anchor, input and endpoint lost their native histories");
                require(index.points[0].role == DurablePointRole::LongAnchor &&
                        index.points[1].role == DurablePointRole::InputReplay &&
                        index.points[2].role == DurablePointRole::Continuation,
                        "native set lost a semantic point role");
                for (const auto& point : index.points)
                    require(backend != SpeculativeBackend::DFlash2 ||
                            point.dflash_context_frontier == point.frontier,
                            "exported StateImage has an unsettled draft ring");
            }
            Json continuations = Json::array();
            for (std::size_t i = 0; i < index.points.size(); ++i) {
                const auto& point = index.points[i];
                auto prompt = index.identities[point.identity].ledger;
                prompt.resize(point.frontier);
                prompt.insert(prompt.end(), 8, 198);
                auto expected = generate(device, *program, frontend.prepare_tokens(prompt), seed.points[i], false);
                require(expected.reused == point.frontier, "live baseline did not hit selected point");
                const auto bytes = decoded(expected.tokens);
                continuations.push_back({{"input", prompt}, {"tokens", expected.tokens},
                    {"bytes", bytes}, {"frontier", point.frontier}});
                release(*program, expected.points);
            }
            write_json(root / "test-oracle.json", {{"pid", pid()}, {"continuations", continuations},
                {"points", stats.points}, {"states", stats.states}, {"pages", stats.pages}});
            release(*program, seed.points);
        } else {
            const auto oracle = read_json(root / "test-oracle.json");
            require(oracle.at("pid").get<std::uint64_t>() != pid(), "restart test reused the exporting process");
            const auto empty = program->physical_usage();
            {
                Reader corrupted(root, true); const auto index = corrupted.index();
                auto refused = program->prepare_checkpoint_import(index, corrupted, {});
                require(refused.state() == DurablePrepareState::Corrupt && !refused.valid(), "bad payload was not corruption");
            }
            require(program->physical_usage().occupied == empty.occupied, "corruption rollback leaked resources");
            for (const auto failure : {DurableReason::IoUnavailable, DurableReason::PayloadSizeMismatch}) {
                for (const bool finish : {false, true}) {
                    Reader failed(root, false, failure, finish);
                    const auto index = failed.index();
                    auto refused = program->prepare_checkpoint_import(index, failed, {});
                    require(!refused.valid() && refused.reason() == failure &&
                            refused.state() == (failure == DurableReason::IoUnavailable
                                ? DurablePrepareState::Unavailable : DurablePrepareState::Corrupt),
                            "reader failure lost its retryable/corrupt classification");
                    require(program->physical_usage().occupied == empty.occupied &&
                            program->physical_usage().host_reserved_bytes == 0,
                            "reader failure leaked unpublished reservations");
                }
            }
            {
                Reader reader(root); const auto index = reader.index();
                auto aborted = program->prepare_checkpoint_import(index, reader, {});
                require(aborted.valid(), "native import preparation refused");
                const auto usage = program->physical_usage();
                require(usage.occupied.state_slots == empty.occupied.state_slots &&
                        usage.occupied.kv_page_groups == empty.occupied.kv_page_groups,
                        "prepared import allocated Device state/pages");
            }
            require(program->physical_usage().occupied == empty.occupied &&
                    program->physical_usage().host_reserved_bytes == 0, "prepare abort leaked resources");
            program.reset(); options.context_cache.host_capacity_bytes = 0; program = make_program();
            require(program->checkpoint_layout() == native_layout,
                    "Host admission capacity changed the native layout identity");
            {
                Reader reader(root); const auto index = reader.index();
                auto refused = program->prepare_checkpoint_import(index, reader, {});
                require(refused.state() == DurablePrepareState::CapacityRefused,
                        "Host capacity pressure was mistaken for corruption");
            }
            if (scenario == "terminal") {
                const auto before = program->physical_usage();
                {
                    Reader reader(root); const auto index = reader.index();
                    const auto validated = program->validate_checkpoint_import(index, reader, {});
                    require(validated.state == q35::CheckpointValidationState::Validated &&
                            validated.reason == DurableReason::None &&
                            validated.stats.points == index.points.size(),
                            "recoverability validation depended on Host admission capacity");
                }
                {
                    Reader reader(root, true); const auto index = reader.index();
                    const auto invalid = program->validate_checkpoint_import(index, reader, {});
                    require(invalid.state == q35::CheckpointValidationState::Corrupt &&
                            invalid.reason == DurableReason::PayloadDigestMismatch,
                            "recoverability validation accepted corrupted payload bytes");
                }
                for (const bool finish : {false, true}) {
                    Reader reader(root, false, DurableReason::IoUnavailable, finish);
                    const auto index = reader.index();
                    const auto unavailable = program->validate_checkpoint_import(index, reader, {});
                    require(unavailable.state == q35::CheckpointValidationState::Unavailable &&
                            unavailable.reason == DurableReason::IoUnavailable,
                            "recoverability validation quarantined transient I/O");
                }
                {
                    Reader reader(root); auto index = reader.index();
                    // Toggle within the same CBOR integer width: schema/size remains valid,
                    // but rebuilding the exact native identity must reject its shortlist.
                    index.identities.front().shortlist_tag[0] ^= 1;
                    require(program->validate_checkpoint_import(index, reader, {}).state ==
                                q35::CheckpointValidationState::Corrupt &&
                            program->prepare_checkpoint_import(index, reader, {}).state() ==
                                DurablePrepareState::Corrupt,
                            "native identity corruption was hidden by absent Host capacity");
                }
                {
                    Reader reader(root); auto index = reader.index();
                    index.native_codec = "unsupported-checkpoint-codec";
                    const auto mismatch = program->validate_checkpoint_import(index, reader, {});
                    require(mismatch.state == q35::CheckpointValidationState::IdentityMismatch &&
                            mismatch.reason == DurableReason::FingerprintMismatch,
                            "recoverability validation accepted a foreign native layout");
                }
                require(program->physical_usage().occupied == before.occupied &&
                        program->physical_usage().host_reserved_bytes == before.host_reserved_bytes,
                        "recoverability validation reserved or published native state");
            }
            program.reset(); options.context_cache.host_capacity_bytes = 1ULL << 30; program = make_program();
            std::shared_ptr<const Reader> fresh;
            {
                Reader reader(root);
                (void)reader.index();
                fresh = std::static_pointer_cast<const Reader>(reader.fresh_reader());
            }
            require(static_cast<bool>(fresh), "fresh reader lost its pinned generation");
            const auto index = fresh->index();
            auto prepared = program->prepare_checkpoint_import(index, *fresh, {});
            require(prepared.valid(), "native final import refused");
            auto points = program->commit_import(std::move(prepared));
            require(points.size() == index.points.size(), "native commit lost checkpoint points");
            std::size_t continuation_bytes = 0;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const auto& expected = oracle.at("continuations").at(i);
                const auto observed = generate(device, *program,
                    frontend.prepare_tokens(expected.at("input").get<std::vector<TokenId>>()), points[i], false);
                const auto bytes = decoded(observed.tokens);
                require(observed.tokens == expected.at("tokens").get<std::vector<TokenId>>() &&
                        observed.reused == expected.at("frontier").get<std::uint32_t>() &&
                        bytes == expected.at("bytes").get<std::string>(),
                        "real restart changed continuation bytes, tokens or checkpoint hit frontier");
                continuation_bytes += bytes.size();
                release(*program, observed.points);
            }
            release(*program, points);
            require(program->physical_usage().occupied == q35::ContextResources{}, "native import retained resources");
            std::cout << "PASS restart scenario=" << scenario << " backend=" << backend_name
                      << " points=" << points.size() << " states=" << index.states.size()
                      << " continuation_bytes=" << continuation_bytes << '\n';
        }
        }
        std::cout << "OK checkpoint restart " << mode << ' ' << backend_name << '\n'; return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL checkpoint restart: " << error.what() << '\n'; return 1;
    }
}
