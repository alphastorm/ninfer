#include "ninfer/engine.h"
#include "core/sha256.h"
#include "models/qwen3_5/program/storage/checkpoint_codec.h"
#include "runtime/contract/continuation_checkpoint.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace ninfer;
using namespace ninfer::runtime;
namespace codec = models::qwen3_5::detail::checkpoint_codec;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct File {
    DurablePayload descriptor;
    std::vector<std::byte> bytes;
    std::uint64_t written = 0;
};
struct Archive {
    std::map<std::string, std::shared_ptr<File>, std::less<>> files;
    bool sealed = false;
};
class Writer final : public ContinuationCheckpointWriter {
public:
    std::shared_ptr<Archive> archive = std::make_shared<Archive>();
    bool write_file(std::string_view path, std::uint64_t offset, std::uint64_t total,
                    std::span<const std::byte> bytes, std::uint32_t alignment) override {
        if (archive->sealed) return false;
        auto [it, added] = archive->files.try_emplace(std::string(path));
        if (added) {
            it->second = std::make_shared<File>();
            it->second->descriptor = {std::string(path), total, {}, alignment};
            it->second->bytes.resize(total);
        }
        auto& file = *it->second;
        if (offset != file.written || total != file.bytes.size() || offset > total ||
            bytes.size() > total - offset || alignment != file.descriptor.alignment) return false;
        std::copy(bytes.begin(), bytes.end(), file.bytes.begin() + offset);
        file.written += bytes.size();
        return true;
    }
    bool finish() override {
        if (archive->sealed) return true;
        for (auto& [path, file] : archive->files) {
            if (file->written != file->bytes.size()) return false;
            file->descriptor.sha256 = crypto::sha256_hex(crypto::sha256(file->bytes));
        }
        archive->sealed = true;
        return true;
    }
};
struct Trace {
    std::uint64_t bytes = 0;
    unsigned index_reads = 0, fresh_readers = 0, verified_sets = 0;
};
class Reader final : public ContinuationCheckpointReader {
public:
    Reader(std::shared_ptr<const Archive> archive, std::shared_ptr<Trace> trace)
        : archive_(std::move(archive)), trace_(std::move(trace)) {
        require(archive_->sealed, "reader received an unfinished real export");
        for (const auto& [path, file] : archive_->files) reads_.try_emplace(path);
    }
    std::optional<std::uint64_t> file_size(std::string_view path) const override {
        const auto descriptor = file_descriptor(path);
        return descriptor ? std::optional(descriptor->bytes) : std::nullopt;
    }
    std::optional<DurablePayload> file_descriptor(std::string_view path) const override {
        const auto it = archive_->files.find(path);
        return it == archive_->files.end() ? std::nullopt : std::optional(it->second->descriptor);
    }
    bool read_file(std::string_view path, std::uint64_t offset,
                   std::span<std::byte> bytes) const override {
        const auto file = archive_->files.find(path);
        if (file == archive_->files.end()) return fail(DurableReason::PayloadSizeMismatch);
        auto& read = reads_.find(path)->second;
        const auto& data = file->second->bytes;
        if (offset != read.consumed || offset > data.size() || bytes.size() > data.size() - offset)
            return fail(DurableReason::PayloadSizeMismatch);
        std::copy_n(data.begin() + offset, bytes.size(), bytes.begin());
        read.hash.update(bytes);
        read.consumed += bytes.size();
        trace_->bytes += bytes.size();
        if (path == "engine/index.cbor") ++trace_->index_reads;
        if (read.consumed == data.size()) read.digest = crypto::sha256_hex(read.hash.finish());
        return true;
    }
    bool verify_complete() const override {
        for (const auto& [path, file] : archive_->files) {
            const auto& read = reads_.find(path)->second;
            if (read.consumed != file->bytes.size()) return fail(DurableReason::PayloadSizeMismatch);
            if (read.digest != file->descriptor.sha256) return fail(DurableReason::PayloadDigestMismatch);
        }
        ++trace_->verified_sets;
        return true;
    }
    DurableReason failure_reason() const noexcept override { return failure_; }
    std::shared_ptr<const ContinuationCheckpointReader> fresh_reader() const override {
        ++trace_->fresh_readers;
        return std::make_shared<Reader>(archive_, trace_);
    }
private:
    struct Read { std::uint64_t consumed = 0; crypto::Sha256 hash; std::string digest; };
    bool fail(DurableReason reason) const noexcept { failure_ = reason; return false; }
    std::shared_ptr<const Archive> archive_;
    std::shared_ptr<Trace> trace_;
    mutable std::map<std::string, Read, std::less<>> reads_;
    mutable DurableReason failure_ = DurableReason::None;
};
class Oracle final : public ReclaimableSessionOracle {
public:
    std::string session, tag;
    std::shared_ptr<const Reader> reader;
    mutable unsigned pins = 0;
    std::shared_ptr<const ContinuationCheckpointReader>
    pin_recoverable(std::string_view wanted_session, std::string_view wanted_tag) const noexcept override {
        if (wanted_session != session || wanted_tag != tag) return {};
        ++pins;
        return reader;
    }
};
class ReleaseGuard final : public PressureCheckpointHandler {
public:
    std::shared_ptr<Oracle> oracle;
    std::shared_ptr<Trace> trace;
    std::uint64_t expected_bytes = 0;
    unsigned calls = 0;
    bool verified_before_release = true;
    void save_before_eviction(std::span<PressureCheckpointVictim> victims) noexcept override {
        for (auto& victim : victims) {
            ++calls;
            const bool verified = victim.session_sha256 == oracle->session &&
                victim.checkpoint_tag == oracle->tag && oracle->pins == 2 &&
                trace->verified_sets == 1 && trace->bytes == expected_bytes;
            verified_before_release &= verified;
            victim.outcome = verified ? PressureCheckpointOutcome::Settled : PressureCheckpointOutcome::Pending;
        }
    }
};
PromptInput prompt(std::string_view session) {
    PromptInput input;
    input.options.enable_thinking = false;
    input.context_cache.session_key = "http:" + std::string(session);
    input.context_cache.reuse_scope = input.context_cache.session_key;
    input.context_cache.shared_reuse_scope = "principal:checkpoint-victim-test";
    input.context_cache.allow_engine_automatic_shared_prefixes = false;
    input.context_cache.markers.push_back({.after_message_count = 1,
        .kind = PromptCacheMarkerKind::PrivateLongAnchor,
        .location = PromptCacheMarkerLocation::MessageBoundary});
    std::string prefix, suffix;
    for (unsigned i = 0; i < 130; ++i) prefix += "alpha ";
    for (unsigned i = 0; i < 140; ++i) suffix += "beta ";
    input.messages = {{.role = ChatRole::System, .parts = {{.text = std::move(prefix)}}},
                      {.role = ChatRole::User, .parts = {{.text = std::move(suffix)}}}};
    return input;
}
RequestOptions request() {
    RequestOptions options;
    options.execution.requested_output_tokens = 8;
    options.execution.sampling.temperature = 0;
    options.execution.allow_prefix_reuse = true;
    options.stop.include_model_defaults = false;
    options.output.raw = true;
    options.tool_choice.constraints = ToolConstraintMode::Automatic;
    return options;
}
RuntimeFingerprintV2 fingerprint(Engine& engine) {
    const auto& options = engine.options();
    const auto memory = engine.memory_summary();
    RuntimeFingerprintV2 result;
    result.frontend = CheckpointEngineAccess::frontend_identity(engine);
    result.native_layout = CheckpointEngineAccess::checkpoint_layout(engine);
    result.engine = {.max_context = options.max_context, .kv_capacity = memory.kv_capacity,
        .kv_dtype = "bf16", .max_concurrency = options.max_concurrency,
        .prefill_chunk = memory.prefill_chunk, .speculative_backend = "dflash2",
        .draft_window = options.speculative.draft_tokens, .proposal_head = "optimized",
        .enable_vision = false, .use_cuda_graph = false};
    return result;
}
DurableImportResult import_set(Engine& engine, std::string_view session, std::string_view tag,
                           const Reader& reader, const RuntimeFingerprintV2& identity,
                           const DurableCheckpointStats& stats) {
    const DurableImportTarget target{.session_sha256 = std::string(session),
        .expected_tag = std::string(tag),
        .expected_epoch = CheckpointEngineAccess::session_epoch(engine, session),
        .allow_prefix_reuse = true, .cache_participation = true};
    return CheckpointEngineAccess::import_session(engine, target, reader, identity, stats, {});
}
} // namespace

int main() {
    const char* artifact = std::getenv("NINFER_TEST_ARTIFACT");
    if (!artifact || !*artifact) { std::cout << "SKIP: NINFER_TEST_ARTIFACT is required\n"; return 77; }
    try {
        const std::string victim(64, 'a'), target(64, 'b'), tag = "victim-turn";
        EngineOptions options;
        options.artifact_path = artifact;
        options.max_context = 1024;
        options.kv_capacity = KvCapacityPolicy::explicit_capacity(1024);
        options.max_concurrency = 1;
        options.prefill_chunk = 128;
        options.kv_cache = KvCacheStorage::BFloat16;
        options.enable_vision = false;
        options.use_cuda_graph = false;
        options.speculative.backend = SpeculativeBackend::DFlash2;
        options.speculative.draft_tokens = 7;
        options.speculative.proposal_head = ProposalHead::Optimized;
        options.context_cache.device_state_slots = 4;
        options.context_cache.host_capacity_bytes = 1ULL << 30;
        Writer exported;
        DurableExportResult saved;
        std::vector<TokenId> reference;
        {
            Engine engine(options);
            auto prepared = engine.prepare(prompt(victim));
            CheckpointEngineAccess::set_checkpoint_tag(prepared, tag);
            reference = engine.generate(std::move(prepared), request()).generated_token_ids;
            require(reference.size() == 8, "real source did not complete its output budget");
            CheckpointEngineAccess::response_stored(engine, victim, tag);
            (void)engine.memory_summary();
            saved = CheckpointEngineAccess::export_session(engine, victim, tag, exported, {});
            require(saved.state == DurableExportState::Exported && saved.stats.points == 3,
                    "real Engine did not export anchor, input and endpoint");
        }
        const auto& index_file = *exported.archive->files.at("engine/index.cbor");
        const auto resident_bytes = saved.stats.payload_bytes - index_file.bytes.size();
        constexpr std::uint64_t mib = 1ULL << 20;
        options.context_cache.host_capacity_bytes = ((resident_bytes + resident_bytes / 2 + mib - 1) / mib) * mib;
        require(options.context_cache.host_capacity_bytes >= resident_bytes &&
                options.context_cache.host_capacity_bytes < 2 * resident_bytes,
                "Host fixture must fit one native set but not two");
        Engine engine(options);
        const auto identity = fingerprint(engine);
        Reader first(exported.archive, std::make_shared<Trace>());
        require(import_set(engine, victim, tag, first, identity, saved.stats).state == DurableImportState::Imported,
                "first Host-only victim import failed");
        const auto occupied = engine.memory_summary().host_context_occupied_bytes;
        require(occupied >= resident_bytes, "victim did not occupy its native Host set");
        Reader no_oracle(exported.archive, std::make_shared<Trace>());
        require(import_set(engine, target, tag, no_oracle, identity, saved.stats).state == DurableImportState::CapacityRefused,
                "target fit without reclaim or absence of oracle allowed destructive release");

        // Descriptor and CBOR remain internally valid. Only native identity reconstruction can
        // reject this set; a Reader-only byte check is not a recoverability proof.
        auto malformed = std::make_shared<Archive>(*exported.archive);
        auto altered = std::make_shared<File>(index_file);
        auto set = codec::decode(altered->bytes, {});
        set.identities.front().shortlist_tag[0] ^= 1;
        const auto encoded = codec::encode(set);
        require(encoded.size() == altered->bytes.size(), "identity mutation changed index shape");
        std::transform(encoded.begin(), encoded.end(), altered->bytes.begin(),
                       [](std::uint8_t byte) { return std::byte{byte}; });
        altered->descriptor.sha256 = crypto::sha256_hex(crypto::sha256(altered->bytes));
        malformed->files["engine/index.cbor"] = std::move(altered);
        auto trace = std::make_shared<Trace>();
        auto oracle = std::make_shared<Oracle>();
        oracle->session = victim; oracle->tag = tag;
        oracle->reader = std::make_shared<Reader>(malformed, trace);
        auto guard = std::make_shared<ReleaseGuard>();
        guard->oracle = oracle; guard->trace = trace; guard->expected_bytes = saved.stats.payload_bytes;
        CheckpointEngineAccess::set_reclaimable_session_oracle(engine, oracle);
        CheckpointEngineAccess::set_pressure_checkpoint_handler(engine, guard);
        Reader refused(exported.archive, std::make_shared<Trace>());
        require(import_set(engine, target, tag, refused, identity, saved.stats).state == DurableImportState::CapacityRefused &&
                oracle->pins == 1 && trace->index_reads == 1 && trace->verified_sets == 0 && guard->calls == 0,
                "native-invalid victim reached destructive release");
        require(engine.memory_summary().host_context_occupied_bytes == occupied &&
                engine.memory_summary().host_context_reserved_bytes == 0,
                "refused victim validation changed native ownership");
        {
            Writer intact;
            require(CheckpointEngineAccess::export_session(engine, victim, tag, intact, {}).state == DurableExportState::Exported,
                    "native-invalid victim proof lost the resident victim");
        }

        trace = std::make_shared<Trace>();
        oracle->pins = 0;
        oracle->reader = std::make_shared<Reader>(exported.archive, trace);
        guard->trace = trace;
        auto target_trace = std::make_shared<Trace>();
        Reader accepted(exported.archive, target_trace);
        const auto restored = import_set(engine, target, tag, accepted, identity, saved.stats);
        std::cout << "reclaim ordering: state=" << static_cast<unsigned>(restored.state)
                  << " reason=" << static_cast<unsigned>(restored.reason)
                  << " pins=" << oracle->pins << " verified_sets=" << trace->verified_sets
                  << " verified_bytes=" << trace->bytes << " payload_bytes=" << saved.stats.payload_bytes
                  << " releases=" << guard->calls << " verified_before_release=" << guard->verified_before_release
                  << " target_fresh_readers=" << target_trace->fresh_readers << '\n';
        // Import preflight proves the victim before ReleaseGuard runs (checked above).
        // The pressure settlement then independently proves it again before releasing it.
        require(restored.state == DurableImportState::Imported && oracle->pins == 4 &&
                trace->verified_sets == 2 && trace->bytes == 2 * saved.stats.payload_bytes &&
                guard->calls == 1 && guard->verified_before_release && target_trace->fresh_readers == 1,
                "actual Engine reclaim did not verify, re-pin, release and retry in order");
        Writer absent;
        const auto evicted = CheckpointEngineAccess::export_session(engine, victim, tag, absent, {});
        require(evicted.state == DurableExportState::Refused && evicted.reason == DurableReason::SessionNotIndexed,
                "successful target import did not actually release the victim");
        require(engine.memory_summary().host_context_reserved_bytes == 0,
                "successful target import retained unpublished reservations");
        CheckpointEngineAccess::set_pressure_checkpoint_handler(engine, {});
        CheckpointEngineAccess::set_reclaimable_session_oracle(engine, {});
        auto continued = engine.prepare(prompt(target));
        CheckpointEngineAccess::set_checkpoint_tag(continued, "continued-turn");
        const auto result = engine.generate(std::move(continued), request());
        require(result.generated_token_ids == reference && result.reused_prompt_tokens != 0 && engine.is_available(),
                "reclaimed target failed exact cached continuation");
        std::cout << "PASS actual-engine-victim-preflight points=" << saved.stats.points
                  << " resident_bytes=" << resident_bytes << " host_capacity=" << *options.context_cache.host_capacity_bytes
                  << " verified_bytes=" << trace->bytes << " pins=" << oracle->pins
                  << " releases=" << guard->calls << " target_fresh_readers=" << target_trace->fresh_readers
                  << " reused_tokens=" << result.reused_prompt_tokens
                  << " native_invalid_victim_preserved=1 validation_before_release=1 exact_tokens=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL actual-engine-victim-preflight: " << error.what() << '\n';
        return 1;
    }
}
