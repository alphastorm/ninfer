#include "core/device.h"
#include "core/sha256.h"
#include "models/qwen3_5/execution/parameters.h"
#include "models/common/qwen/frontend/frontend.h"
#include "models/qwen3_5/load.h"
#include "models/qwen3_5/program/program.h"
#include "runtime/contract/continuation_checkpoint.h"
#include <nlohmann/json.hpp>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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
    explicit Reader(fs::path root, bool corrupt = false) : root_(std::move(root)), corrupt_(corrupt) {
        for (const auto& item : read_json(root_ / "test-inventory.json")) {
            File file;
            file.descriptor = {item.at("path"), item.at("bytes"), item.at("sha256"), item.at("alignment")};
            files_.emplace(file.descriptor.path, std::move(file));
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
        auto found = files_.find(path);
        if (found == files_.end()) return false;
        auto& file = found->second;
        if (offset != file.consumed || offset > file.descriptor.bytes || bytes.size() > file.descriptor.bytes - offset)
            return false;
        std::ifstream stream(root_ / path, std::ios::binary); stream.seekg(offset);
        stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (!stream || stream.gcount() != static_cast<std::streamsize>(bytes.size())) return false;
        if (corrupt_ && path != "engine/index.cbor" && !bytes.empty()) bytes[0] ^= std::byte{1};
        file.hash.update(bytes); file.consumed += bytes.size();
        if (file.consumed == file.descriptor.bytes)
            file.actual = crypto::sha256_hex(file.hash.finish());
        return true;
    }
    bool verify_complete() const override {
        for (const auto& [path, file] : files_)
            if (file.consumed != file.descriptor.bytes || file.actual != file.descriptor.sha256) return false;
        return true;
    }
    DurableCheckpointSet index() const {
        const auto size = file_size("engine/index.cbor").value();
        require(size <= kDurableManifestMaxBytes, "test metadata unexpectedly oversized");
        std::vector<std::byte> bytes(size);
        require(read_file("engine/index.cbor", 0, bytes), "index read failed");
        return q35::Program::decode_checkpoint_index(bytes, {});
    }
private:
    struct InputFile : File { std::string actual; InputFile(File value) : File(std::move(value)) {} };
    fs::path root_;
    mutable std::map<std::string, InputFile, std::less<>> files_;
    bool corrupt_;
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
Generated generate(DeviceContext& device, q35::Program& program, models::qwen::Frontend& frontend,
                   const std::vector<TokenId>& input, std::optional<q35::CheckpointHandle> source,
                   bool capture) {
    ResolvedExecutionOptions options;
    options.requested_output_tokens = 8; options.allow_prefix_reuse = true;
    options.sampling.temperature = 0; options.sampling.top_k = 1;
    auto base = program.plan_request(frontend.prepare_tokens(input), options);
    auto candidate = program.inspect_source(base, source);
    require(candidate.has_value(), "matching native source missing");
    Generated result; result.reused = candidate->reused_tokens;
    require(static_cast<bool>(program.start_binding(base, {0}, *candidate)), "native source binding refused");
    q35::ContextProgress bound; settle(device, program, bound);
    require(bound.sequence.has_value(), "native binding did not publish sequence");
    const auto sequence = *bound.sequence;
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
    while (result.tokens.size() < 8) {
        const auto remaining = static_cast<std::uint32_t>(8 - result.tokens.size());
        reserve(q35::ExecutionUnitKind::Decode, remaining);
        const std::array members{sequence};
        const std::array<RoundBudget, 1> budgets{{{.generated_tokens_remaining = remaining}}};
        auto pending = program.decode(members, budgets);
        const auto count = pending.row_counts().empty() ? 1U : static_cast<std::uint32_t>(pending.row_counts()[0]);
        require(count && count <= remaining, "decode exceeded requested tokens");
        result.tokens.insert(result.tokens.end(), pending.tokens().begin(), pending.tokens().begin() + count);
        const std::array<CommitDecision, 1> accept{{{.accepted_tokens = count, .terminal = result.tokens.size() == 8}}};
        (void)program.commit(std::move(pending), accept);
    }
    const auto finish = program.finish(sequence);
    require(finish.status == ConsumeStatus::Consumed && finish.checkpoint, "native endpoint not retained");
    result.points.push_back(*finish.checkpoint);
    return result;
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
        const fs::path root(argv[2]);
        DeviceContext device;
        models::LoadOptions selected; selected.speculative = backend;
        auto model = q35::load_model(artifact, selected, device);
        q35::execution::Parameters parameters(*model);
        auto frontend = models::qwen::make_frontend(model->resources(), {.vision_enabled = false, .max_context = 1024},
            q35::default_sampling(model->config().text.architecture));
        EngineOptions options;
        options.max_context = 1024; options.kv_capacity = KvCapacityPolicy::explicit_capacity(2048);
        options.prefill_chunk = 128; options.max_concurrency = 1; options.kv_cache = KvCacheStorage::BFloat16;
        options.use_cuda_graph = false; options.speculative.backend = backend;
        options.speculative.draft_tokens = backend == SpeculativeBackend::None ? 0 : 7;
        options.context_cache.device_state_slots = 3; options.context_cache.host_capacity_bytes = 1ULL << 30;
        const auto make_program = [&] {
            auto planner = q35::make_sequence_planner(parameters, device, options);
            return q35::create_program(parameters, std::move(planner).finalize(16), device, {});
        };
        auto program = make_program();
        if (mode == "export") {
            require(!fs::exists(root), "restart export directory already exists"); fs::create_directories(root);
            std::vector<TokenId> prompt(257, 198); prompt.front() = 1000;
            auto seed = generate(device, *program, frontend, prompt, std::nullopt, true);
            require(seed.points.size() >= 2, "test did not retain input and endpoint");
            const auto frontier = program->checkpoint_summary(seed.points.back()).frontier;
            DurableCheckpointStats stats;
            {
                auto capture = program->prepare_checkpoint_export(seed.points, {});
                require(capture.state() == DurablePrepareState::Prepared, "native set export refused");
                Writer writer(root);
                const auto exported = capture.write(writer);
                require(exported.state == DurableExportState::Exported, "native payload write refused");
                stats = exported.stats;
                require(capture.index().points.size() >= 2 && capture.index().histories.size() == 1,
                        "input and endpoint did not share a logical history");
            }
            prompt.insert(prompt.end(), seed.tokens.begin(), seed.tokens.end()); prompt.insert(prompt.end(), 8, 198);
            auto expected = generate(device, *program, frontend, prompt, seed.points.back(), false);
            require(expected.reused == frontier, "live baseline did not hit endpoint");
            write_json(root / "test-oracle.json", {{"pid", pid()}, {"input", prompt}, {"tokens", expected.tokens},
                {"frontier", frontier}, {"points", stats.points}, {"states", stats.states}, {"pages", stats.pages}});
            release(*program, expected.points); release(*program, seed.points);
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
            {
                Reader reader(root); const auto index = reader.index();
                auto refused = program->prepare_checkpoint_import(index, reader, {});
                require(refused.state() == DurablePrepareState::CapacityRefused,
                        "Host capacity pressure was mistaken for corruption");
            }
            program.reset(); options.context_cache.host_capacity_bytes = 1ULL << 30; program = make_program();
            Reader reader(root); const auto index = reader.index();
            auto prepared = program->prepare_checkpoint_import(index, reader, {});
            require(prepared.valid(), "native final import refused");
            auto points = program->commit_import(std::move(prepared));
            require(points.size() == index.points.size(), "native commit lost checkpoint points");
            const auto observed = generate(device, *program, frontend, oracle.at("input").get<std::vector<TokenId>>(),
                                           points[index.endpoint], false);
            require(observed.tokens == oracle.at("tokens").get<std::vector<TokenId>>() &&
                    observed.reused == oracle.at("frontier").get<std::uint32_t>(),
                    "real restart changed continuation tokens or checkpoint hit frontier");
            release(*program, observed.points); release(*program, points);
            require(program->physical_usage().occupied == q35::ContextResources{}, "native import retained resources");
        }
        std::cout << "OK checkpoint restart " << mode << ' ' << backend_name << '\n'; return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL checkpoint restart: " << error.what() << '\n'; return 1;
    }
}
