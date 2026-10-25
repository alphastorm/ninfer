#include "ninfer/engine.h"
#include "core/sha256.h"
#include "runtime/contract/continuation_checkpoint.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>

namespace {
using namespace ninfer;
using namespace ninfer::runtime;
using namespace std::chrono_literals;
void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}
void phase(const char* name) {
    static const auto started = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "E2 phase=" << name << " elapsed_ms=" << elapsed << std::endl;
}
struct File {
    DurablePayload descriptor;
    std::vector<std::byte> bytes;
};
using Files = std::map<std::string, File, std::less<>>;
class Writer final : public ContinuationCheckpointWriter {
public:
    Files files;
    bool write_file(std::string_view path, std::uint64_t offset, std::uint64_t total,
                    std::span<const std::byte> bytes, std::uint32_t alignment) override {
        auto [it, inserted] = files.try_emplace(std::string(path));
        auto& file = it->second;
        if (inserted) { file.descriptor = {std::string(path), total, {}, alignment}; }
        if (offset != file.bytes.size() || total != file.descriptor.bytes ||
            alignment != file.descriptor.alignment || offset > total || bytes.size() > total - offset) {
            return false;
        }
        file.bytes.insert(file.bytes.end(), bytes.begin(), bytes.end());
        return true;
    }
    bool finish() override {
        for (auto& [path, file] : files) {
            if (file.bytes.size() != file.descriptor.bytes) { return false; }
            crypto::Sha256 hash;
            hash.update(file.bytes);
            file.descriptor.sha256 = crypto::sha256_hex(hash.finish());
        }
        return true;
    }
};
class Reader final : public ContinuationCheckpointReader {
public:
    explicit Reader(std::shared_ptr<const Files> files) : files_(std::move(files)) {}
    std::optional<std::uint64_t> file_size(std::string_view path) const override {
        const auto descriptor = file_descriptor(path);
        return descriptor ? std::optional(descriptor->bytes) : std::nullopt;
    }
    std::optional<DurablePayload> file_descriptor(std::string_view path) const override {
        const auto found = files_->find(path);
        return found == files_->end() ? std::nullopt : std::optional(found->second.descriptor);
    }
    bool read_file(std::string_view path, std::uint64_t offset,
                   std::span<std::byte> bytes) const override {
        const auto found = files_->find(path);
        if (found == files_->end()) { return fail(DurableReason::InvalidIndex); }
        auto& state = consumed_[std::string(path)];
        if (offset != state.offset || offset > found->second.bytes.size() ||
            bytes.size() > found->second.bytes.size() - offset) {
            return fail(DurableReason::PayloadSizeMismatch);
        }
        std::copy_n(found->second.bytes.begin() + offset, bytes.size(), bytes.begin());
        state.hash.update(bytes);
        state.offset += bytes.size();
        if (state.offset == found->second.bytes.size()) {
            state.digest = crypto::sha256_hex(state.hash.finish());
        }
        return true;
    }
    bool verify_complete() const override {
        for (const auto& [path, file] : *files_) {
            const auto found = consumed_.find(path);
            if (found == consumed_.end() || found->second.offset != file.bytes.size()) {
                return fail(DurableReason::PayloadSizeMismatch);
            }
            if (found->second.digest != file.descriptor.sha256) {
                return fail(DurableReason::PayloadDigestMismatch);
            }
        }
        return true;
    }
    DurableReason failure_reason() const noexcept override { return failure_; }
    std::shared_ptr<const ContinuationCheckpointReader> fresh_reader() const override {
        return std::make_shared<Reader>(files_);
    }
private:
    struct Consumption { std::uint64_t offset = 0; crypto::Sha256 hash; std::string digest; };
    bool fail(DurableReason reason) const { failure_ = reason; return false; }
    std::shared_ptr<const Files> files_;
    mutable std::map<std::string, Consumption, std::less<>> consumed_;
    mutable DurableReason failure_ = DurableReason::None;
};
const std::string a(64, 'a'), b(64, 'b');
constexpr std::string_view tag = "pressure-seed";
PromptInput prompt(std::string_view session, const std::string& text) {
    PromptInput input;
    input.messages = {{.role = ChatRole::User, .parts = {{.text = text}}}};
    input.context_cache.session_key = "http:" + std::string(session);
    input.context_cache.reuse_scope = input.context_cache.session_key;
    input.context_cache.shared_reuse_scope = "principal:e2";
    input.context_cache.allow_engine_automatic_shared_prefixes = false;
    return input;
}
RequestOptions request(std::uint32_t count, bool reuse = true) {
    RequestOptions options;
    options.execution.requested_output_tokens = count;
    options.execution.sampling.temperature = 0;
    options.execution.allow_prefix_reuse = reuse;
    options.stop.include_model_defaults = false;
    options.output.raw = true;
    options.tool_choice.constraints = ToolConstraintMode::Automatic;
    return options;
}
RuntimeFingerprintV2 fingerprint(Engine& engine) {
    const auto& options = engine.options();
    const auto memory = engine.memory_summary();
    RuntimeFingerprintV2 value;
    value.frontend = CheckpointEngineAccess::frontend_identity(engine);
    value.native_layout = CheckpointEngineAccess::checkpoint_layout(engine);
    value.engine = {.max_context = options.max_context, .kv_capacity = memory.kv_capacity,
        .kv_dtype = "bf16", .max_concurrency = 2, .prefill_chunk = memory.prefill_chunk,
        .speculative_backend = "dflash2", .draft_window = 7, .proposal_head = "optimized",
        .enable_vision = options.enable_vision, .use_cuda_graph = options.use_cuda_graph};
    return value;
}
DurableImportTarget target(Engine& engine, const std::string& session) {
    return {.session_sha256 = session, .expected_tag = std::string(tag),
        .expected_epoch = CheckpointEngineAccess::session_epoch(engine, session),
        .shared_reuse_scope = "principal:e2", .allow_prefix_reuse = true, .cache_participation = true};
}
struct Oracle final : ReclaimableSessionOracle {
    std::shared_ptr<const Files> files;
    std::shared_ptr<const ContinuationCheckpointReader> pin_recoverable(
        std::string_view session, std::string_view expected_tag) const noexcept override {
        if (session != b || expected_tag != tag) { return {}; }
        try { return std::make_shared<Reader>(files); }
        catch (...) { return {}; }
    }
};
struct Handler final : PressureCheckpointHandler {
    std::atomic<bool> saved{false};
    std::atomic<unsigned> pending{0}, settled{0};
    void save_before_eviction(std::span<PressureCheckpointVictim> victims) noexcept override {
        for (auto& victim : victims) {
            if (victim.session_sha256 == b && victim.checkpoint_tag == tag && !saved.load()) {
                ++pending;
                victim.outcome = PressureCheckpointOutcome::Pending;
            } else {
                ++settled;
                victim.outcome = PressureCheckpointOutcome::Settled;
            }
        }
    }
};
void exercise(const char* artifact) {
    EngineOptions options;
    options.artifact_path = artifact;
    options.max_context = 4096;
    options.kv_capacity = KvCapacityPolicy::explicit_capacity(4096);
    options.max_concurrency = 2;
    options.prefill_chunk = 256;
    options.context_cache.device_state_slots = 4;
    options.context_cache.host_capacity_bytes = 1ULL << 30;
    options.speculative.backend = SpeculativeBackend::DFlash2;
    options.speculative.draft_tokens = 7;
    options.speculative.proposal_head = ProposalHead::Optimized;
    std::string text = "Summarize this ledger:";
    for (unsigned i = 0; i < 1600; ++i) { text += " durable"; }
    Writer seed;
    DurableExportResult exported;
    std::uint32_t prompt_tokens = 0;
    {
        phase("source-load");
        Engine source(options);
        phase("source-ready");
        auto prepared = source.prepare(prompt(a, text));
        prompt_tokens = prepared.summary().prompt_tokens;
        require(prompt_tokens >= 1500 && prompt_tokens < 1900, "unexpected E2 prompt geometry");
        CheckpointEngineAccess::set_checkpoint_tag(prepared, tag);
        require(source.generate(std::move(prepared), request(16)).generated_token_ids.size() == 16,
                "source seed did not complete");
        CheckpointEngineAccess::response_stored(source, a, tag);
        exported = CheckpointEngineAccess::export_session(source, a, tag, seed, {});
        require(exported.state == DurableExportState::Exported && seed.finish(), "source export failed");
        phase("source-exported");
    }
    const auto resident = exported.stats.payload_bytes - seed.files.at("engine/index.cbor").bytes.size();
    constexpr std::uint64_t mib = 1ULL << 20;
    options.context_cache.host_capacity_bytes = ((resident * 3 / 2 + mib - 1) / mib) * mib;
    require(options.context_cache.host_capacity_bytes >= resident &&
                options.context_cache.host_capacity_bytes < 2 * resident,
            "fixture must fit one full set but not two");
    auto files = std::make_shared<const Files>(std::move(seed.files));
    phase("target-load");
    Engine engine(options);
    phase("target-ready");
    const auto identity = fingerprint(engine);
    Reader victim(files);
    require(CheckpointEngineAccess::import_session(engine, target(engine, b), victim,
                identity, exported.stats, {}).state == DurableImportState::Imported,
            "initial Host-only session import failed");
    phase("victim-imported");
    const auto occupied = engine.memory_summary().host_context_occupied_bytes;
    require(occupied >= resident && occupied * 2 > options.context_cache.host_capacity_bytes,
            "import did not exercise near-capacity Host pressure");
    auto handler = std::make_shared<Handler>();
    auto oracle = std::make_shared<Oracle>();
    oracle->files = files;
    CheckpointEngineAccess::set_pressure_checkpoint_handler(engine, handler);
    CheckpointEngineAccess::set_reclaimable_session_oracle(engine, oracle);
    std::mutex mutex;
    std::condition_variable ready;
    bool started = false;
    GenerationObservationOptions observation;
    observation.first_token = [&](const auto&) {
        { std::lock_guard lock(mutex); started = true; }
        ready.notify_one();
    };
    // A store-free/cache-disabled turn in B leaves its last durable turn unchanged.
    // The live B lane must continue while A's restore reclaims that old, saved Host copy.
    auto handle = engine.submit(engine.prepare(prompt(b, text)), request(2048, false),
                                OutputConsumerMode::Aggregate, observation);
    auto sibling = std::async(std::launch::async, [handle = std::move(handle)]() mutable {
        return handle.wait();
    });
    {
        std::unique_lock lock(mutex);
        require(ready.wait_for(lock, 30s, [&] { return started; }), "B never entered decode");
    }
    phase("sibling-decoding");
    Reader blocked(files);
    const auto refused = CheckpointEngineAccess::import_session(
        engine, target(engine, a), blocked, identity, exported.stats, {});
    require(refused.state == DurableImportState::CapacityRefused && handler->pending.load() > 0,
            "Pending did not refuse destructive restore reclaim");
    phase("pending-preserved");
    Writer protected_copy;
    const auto protected_export = CheckpointEngineAccess::export_session(engine, b, tag, protected_copy, {});
    require(protected_export.state == DurableExportState::Exported && protected_copy.finish(),
            "Pending destroyed the victim before save");
    phase("victim-exported");
    const auto publication = CheckpointEngineAccess::publish_export(
        engine, b, tag, protected_export.captured_epoch, [&] {
            oracle->files = std::make_shared<const Files>(std::move(protected_copy.files));
            handler->saved = true;
            return true;
        });
    require(publication.state == DurableExportState::Exported, "save-before-evict publication failed");
    phase("victim-save-published");
    CheckpointEngineAccess::notify_checkpoint_progress(engine);
    require(sibling.wait_for(0s) != std::future_status::ready && engine.runtime_stats().running_requests > 0,
            "B finished before restore overlap");
    Reader restored(files);
    const auto imported = CheckpointEngineAccess::import_session(
        engine, target(engine, a), restored, identity, exported.stats, {});
    require(imported.state == DurableImportState::Imported && handler->settled.load() > 0,
            "saved victim did not unblock restore");
    phase("target-imported");
    require(sibling.wait_for(0s) != std::future_status::ready,
            "restore waited for B decode to finish");
    const auto before = engine.runtime_stats();
    auto continued_prompt = engine.prepare(prompt(a, text));
    CheckpointEngineAccess::set_checkpoint_tag(continued_prompt, "pressure-continuation");
    const auto continued = engine.generate(std::move(continued_prompt), request(16));
    phase("target-continued");
    const auto live = sibling.get();
    phase("sibling-finished");
    const auto after = engine.runtime_stats();
    require(live.generated_token_ids.size() == 2048 && continued.generated_token_ids.size() == 16 &&
                continued.reused_prompt_tokens >= 1500, "restored A or live B lost its continuation");
    require(after.decode_row_rounds - before.decode_row_rounds >
                after.decode_rounds - before.decode_rounds, "restore continuation never decoded at C2");
    const auto fresh = engine.generate(engine.prepare(prompt(a, text)), request(16, false));
    require(continued.generated_token_ids == fresh.generated_token_ids,
            "restored continuation differs from fresh deterministic tokens");
    CheckpointEngineAccess::set_pressure_checkpoint_handler(engine, {});
    CheckpointEngineAccess::set_reclaimable_session_oracle(engine, {});
    require(engine.is_available(), "E2 made Engine unavailable");
    std::cout << "E2 host_capacity=" << *options.context_cache.host_capacity_bytes
              << " resident_bytes=" << resident << " occupied_bytes=" << occupied
              << " prompt_tokens=" << prompt_tokens << " cached_tokens=" << continued.reused_prompt_tokens
              << " pending=" << handler->pending << " settled=" << handler->settled
              << " rows=" << after.decode_row_rounds - before.decode_row_rounds
              << " rounds=" << after.decode_rounds - before.decode_rounds << '\n';
}
} // namespace
int main() {
    const char* artifact = std::getenv("NINFER_TEST_ARTIFACT");
    if (!artifact || !*artifact) { std::cout << "skip: NINFER_TEST_ARTIFACT is not set\n"; return 77; }
    try { exercise(artifact); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return 0;
}
