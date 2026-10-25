#include "ninfer/engine.h"
#include "runtime/contract/continuation_checkpoint.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

ninfer::RequestOptions request(std::uint32_t outputs, bool reuse = true) {
    ninfer::RequestOptions options;
    options.execution.requested_output_tokens = outputs;
    options.execution.sampling.temperature = 0.0F;
    options.execution.allow_prefix_reuse = reuse;
    options.stop.include_model_defaults = false;
    options.output.raw = true;
    return options;
}

void complete(const ninfer::GenerationResult& result, std::size_t outputs) {
    require(result.generated_token_ids.size() == outputs &&
                result.finish_reason == ninfer::FinishReason::OutputLimit,
            "one request lost its independent output budget");
}

void settled(ninfer::Engine& engine) {
    (void)engine.memory_summary(); // Complete the terminal worker boundary before inspecting gauges.
    const auto stats = engine.runtime_stats();
    require(stats.running_requests == 0 && stats.waiting_requests == 0 &&
                stats.materializing_requests == 0 && stats.prefilling_requests == 0 &&
                stats.decode_ready_requests == 0 && stats.paused_requests == 0 &&
                stats.replaying_requests == 0 && stats.capture_pending_requests == 0 &&
                stats.terminal_pending_requests == 0,
            "settlement/cancellation left live scheduler membership");
}

void inflight_bound(ninfer::Engine& engine) {
    std::array<ninfer::GenerationHandle, 4> handles;
    for (auto& handle : handles) {
        handle = engine.submit(engine.prepare_tokens({198, 5834, 198}), request(2, false));
    }
    bool overloaded = false;
    try {
        auto extra = engine.submit(engine.prepare_tokens({198, 5834, 198}), request(2, false));
    } catch (const ninfer::RequestError& error) {
        overloaded = error.kind() == ninfer::RequestErrorKind::Overloaded;
    }
    require(overloaded, "C2 plus two pending/retained consumers did not enforce four in flight");
    // Completion does not release an unconsumed handle twice; drain in reverse submission order.
    for (auto it = handles.rbegin(); it != handles.rend(); ++it) { complete(it->wait(), 2); }
    complete(engine.generate(engine.prepare_tokens({198, 1002, 198}), request(2, false)), 2);
    settled(engine);
}

// An actual production Engine, not a serving-policy fake. Any attempted read means the
// adapter crossed the policy gate before refusing a warmup/cache-disabled request.
void checkpoint_policy_refusal(ninfer::Engine& engine) {
    using namespace ninfer::runtime;
    class Unreadable final : public ContinuationCheckpointReader {
    public:
        mutable std::size_t calls = 0;
        std::optional<std::uint64_t> file_size(std::string_view) const override {
            ++calls; return std::nullopt;
        }
        std::optional<DurablePayload> file_descriptor(std::string_view) const override {
            ++calls; return std::nullopt;
        }
        bool read_file(std::string_view, std::uint64_t, std::span<std::byte>) const override {
            ++calls; return false;
        }
        bool verify_complete() const override { ++calls; return false; }
        DurableReason failure_reason() const noexcept override {
            ++calls; return DurableReason::IoUnavailable;
        }
        std::shared_ptr<const ContinuationCheckpointReader> fresh_reader() const override {
            ++calls; return {};
        }
    } reader;
    const auto before = engine.memory_summary();
    for (bool warmup : {true, false}) {
        const DurableImportTarget target{
            .session_sha256 = std::string(64, 'a'), .expected_tag = "policy-refusal",
            .allow_prefix_reuse = warmup, .cache_participation = !warmup};
        auto prepared = CheckpointEngineAccess::prepare_import(engine, target, reader, {}, {}, {});
        require(prepared.state == DurablePrepareState::PolicyDisabled && !prepared.prepared &&
                    prepared.reason == DurableReason::CacheDisabled,
                "direct adapter prepared a warmup/cache-disabled import");
        const auto imported = CheckpointEngineAccess::import_session(engine, target, reader, {}, {}, {});
        require(imported.state == DurableImportState::PolicyDisabled &&
                    imported.reason == DurableReason::CacheDisabled && reader.calls == 0,
                "direct adapter inspected checkpoint bytes before request policy");
    }
    const auto after = engine.memory_summary();
    require(before.host_context_occupied_bytes == after.host_context_occupied_bytes &&
                before.host_context_reserved_bytes == after.host_context_reserved_bytes &&
                before.host_state_occupied_slots == after.host_state_occupied_slots &&
                before.host_kv_occupied_bytes == after.host_kv_occupied_bytes,
            "policy refusal changed native reservation accounting");
}

void checkpoint_epoch_refusal(ninfer::Engine& engine) {
    using namespace ninfer::runtime;
    const std::string digest(64, 'b');
    const auto old = CheckpointEngineAccess::session_epoch(engine, digest);
    ninfer::PromptInput input;
    input.messages = {{.role = ninfer::ChatRole::User,
                       .parts = {{.text = "Count from one to three."}}}};
    input.context_cache.session_key = "http:" + digest;
    input.context_cache.reuse_scope = "http:" + digest;
    input.context_cache.shared_reuse_scope = "principal:test";
    auto prompt = engine.prepare(std::move(input));
    CheckpointEngineAccess::set_checkpoint_tag(prompt, "epoch-turn");
    auto options = request(8);
    options.tool_choice.constraints = ninfer::ToolConstraintMode::Automatic;
    auto handle = engine.submit(std::move(prompt), options);
    const auto submitted = CheckpointEngineAccess::publication_epoch(handle);
    require(submitted.value > old.value, "submission failed to advance the publication epoch");
    bool called = false;
    const auto stale = CheckpointEngineAccess::publish_export(
        engine, digest, "epoch-turn", old, [&] { called = true; return true; });
    require(stale.state == DurableExportState::Stale && !called,
            "production adapter published a stale captured epoch");
    (void)handle.wait();
    CheckpointEngineAccess::response_stored(engine, digest, "epoch-turn");
    const auto stored = CheckpointEngineAccess::session_epoch(engine, digest);
    const auto current = CheckpointEngineAccess::publish_export(
        engine, digest, "epoch-turn", stored, [&] { called = true; return true; });
    require(current.state == DurableExportState::Exported && called,
            "settled production epoch was not publishable");
}

void checkpoint_failure_classification(ninfer::Engine& engine) {
    using namespace ninfer::runtime;
    const auto& options = engine.options();
    const auto memory = engine.memory_summary();
    RuntimeFingerprintV2 fingerprint;
    fingerprint.frontend = CheckpointEngineAccess::frontend_identity(engine);
    fingerprint.native_layout = CheckpointEngineAccess::checkpoint_layout(engine);
    fingerprint.engine = {
        .max_context = options.max_context, .kv_capacity = memory.kv_capacity,
        .kv_dtype = "bf16", .max_concurrency = options.max_concurrency,
        .prefill_chunk = memory.prefill_chunk,
        .speculative_backend = options.speculative.backend == ninfer::SpeculativeBackend::None
            ? "none" : "dflash2",
        .draft_window = options.speculative.draft_tokens,
        .proposal_head = options.speculative.proposal_head == ninfer::ProposalHead::Optimized
            ? "optimized" : "full",
        .enable_vision = options.enable_vision, .use_cuda_graph = options.use_cuda_graph};
    class BrokenIndex final : public ContinuationCheckpointReader {
    public:
        bool unavailable = true;
        std::optional<std::uint64_t> file_size(std::string_view) const override { return 1; }
        std::optional<DurablePayload> file_descriptor(std::string_view) const override {
            return std::nullopt;
        }
        bool read_file(std::string_view, std::uint64_t, std::span<std::byte> bytes) const override {
            if (unavailable) { return false; }
            bytes[0] = std::byte{0xff}; // Invalid standalone CBOR break byte.
            return true;
        }
        bool verify_complete() const override { return false; }
        DurableReason failure_reason() const noexcept override {
            return unavailable ? DurableReason::IoUnavailable : DurableReason::None;
        }
        std::shared_ptr<const ContinuationCheckpointReader> fresh_reader() const override { return {}; }
    } reader;
    const DurableImportTarget target{.session_sha256 = std::string(64, 'c'),
        .expected_tag = "broken-index", .allow_prefix_reuse = true, .cache_participation = true};
    const auto unavailable = CheckpointEngineAccess::import_session(
        engine, target, reader, fingerprint, {}, {});
    require(unavailable.state == DurableImportState::Unavailable &&
                unavailable.reason == DurableReason::IoUnavailable,
            "adapter quarantinable corruption replaced retryable Reader I/O failure");
    reader.unavailable = false;
    const auto corrupt = CheckpointEngineAccess::import_session(
        engine, target, reader, fingerprint, {}, {});
    require(corrupt.state == DurableImportState::Corrupt && corrupt.reason == DurableReason::InvalidIndex,
            "adapter failed to classify invalid native CBOR as Corrupt");
}

void pending_timeout(ninfer::Engine& engine) {
    require(engine.options().pending_timeout_ms == 30000, "shipped 30 s pending default changed");
    std::mutex mutex;
    std::condition_variable ready;
    std::array<bool, 2> started{};
    std::array<std::future<ninfer::GenerationResult>, 2> active;
    for (std::size_t row = 0; row < active.size(); ++row) {
        ninfer::GenerationObservationOptions observation;
        observation.first_token = [&, row](const auto&) {
            {
                std::lock_guard lock(mutex);
                started[row] = true;
            }
            ready.notify_one();
        };
        auto handle = engine.submit(
            engine.prepare_tokens({198, static_cast<ninfer::TokenId>(1000 + row), 198}),
            request(1024, false), ninfer::OutputConsumerMode::Aggregate, observation);
        // Observers are delivered by wait()'s consumer, not by the GPU worker. Keep both
        // consumers running while the main thread exercises a third queued request.
        active[row] = std::async(std::launch::async, [handle = std::move(handle)]() mutable {
            return handle.wait();
        });
    }
    {
        std::unique_lock lock(mutex);
        require(ready.wait_for(lock, std::chrono::seconds(30),
                               [&] { return started[0] && started[1]; }),
                "two requests never started before the pending-timeout fixture");
    }
    // The first-output observations establish live execution. memory_summary() takes
    // execution_mutex_ and may wait behind decode; it is not a live-lane barrier.
    require(engine.runtime_stats().running_requests == 2,
            "timeout fixture did not hold both execution lanes");
    auto waiting = engine.submit(engine.prepare_tokens({198, 1002, 198}), request(2, false),
                                 ninfer::OutputConsumerMode::Aggregate, {},
                                 Clock::now() + std::chrono::milliseconds(20));
    bool timed_out = false;
    try {
        (void)waiting.wait();
    } catch (const ninfer::RequestError& error) {
        timed_out = error.kind() == ninfer::RequestErrorKind::QueueTimeout;
    }
    require(timed_out, "queued deadline was ignored while both lanes decoded");
    for (auto& result : active) { complete(result.get(), 1024); }
    settled(engine);
}

void shared_readers_and_live_sibling(ninfer::Engine& engine) {
    std::string shared;
    for (unsigned i = 0; i < 32; ++i) {
        shared += "Explain deterministic scheduling, cancellation and independent branch continuation. ";
    }
    const auto prompt = [&](char session, std::string text) {
        ninfer::PromptInput input;
        input.options.enable_thinking = false;
        input.context_cache.session_key = "http:" + std::string(64, session);
        input.context_cache.reuse_scope = input.context_cache.session_key;
        input.context_cache.shared_reuse_scope = "principal:c2-shared-readers";
        input.context_cache.allow_engine_automatic_shared_prefixes = false;
        input.context_cache.markers.push_back({.after_message_count = 1,
            .kind = ninfer::PromptCacheMarkerKind::SharedStablePrefix,
            .location = ninfer::PromptCacheMarkerLocation::MessageBoundary});
        input.messages = {
            {.role = ninfer::ChatRole::System, .parts = {{.text = shared}}},
            {.role = ninfer::ChatRole::User, .parts = {{.text = std::move(text)}}}};
        return input;
    };
    const auto options = [&](std::uint32_t outputs, bool reuse = true) {
        auto value = request(outputs, reuse);
        value.output.raw = false;
        value.tool_choice.constraints = ninfer::ToolConstraintMode::Automatic;
        return value;
    };
    // A private raw-token endpoint is consumable by its first reader. Shared fanout
    // must instead use the same explicit, principal-scoped marker as production.
    const auto primed = engine.generate(engine.prepare(prompt('c', "Name one short word.")), options(8));
    complete(primed, 8);
    auto other = prompt('e', "Explain the short branch.");
    const auto before = engine.runtime_stats();
    auto older = engine.submit(engine.prepare(prompt('d', "Explain the long branch.")), options(1024));
    auto younger = engine.submit(engine.prepare(other), options(16));
    const auto branch = younger.wait();
    complete(branch, 16);
    require(branch.reused_prompt_tokens != 0, "second reader did not reuse the primed checkpoint");
    require(engine.runtime_stats().running_requests != 0,
            "short sibling waited for the long sibling to finish");
    other.messages.push_back({.role = ninfer::ChatRole::Assistant, .parts = {{.text = branch.content}}});
    other.messages.push_back({.role = ninfer::ChatRole::User, .parts = {{.text = "Continue briefly."}}});
    const auto continued = engine.generate(engine.prepare(other), options(8));
    require(continued.prefix_reuse_path == ninfer::PrefixReusePath::PrivateEndpoint &&
                engine.runtime_stats().last_selected_reuse_path == continued.prefix_reuse_path,
            "successful sibling binding lost its actual checkpoint-role telemetry");
    const auto fresh = engine.generate(engine.prepare(other), options(8, false));
    complete(continued, 8);
    complete(fresh, 8);
    require(continued.reused_prompt_tokens != 0 && fresh.reused_prompt_tokens == 0,
            "live-sibling continuation did not preserve reuse policy");
    require(continued.generated_token_ids == fresh.generated_token_ids,
            "continuing a sibling changed deterministic tokens versus a fresh prefix");
    const auto long_branch = older.wait();
    complete(long_branch, 1024);
    require(long_branch.reused_prompt_tokens != 0,
            "first reader lost its shared source while the sibling continued");
    settled(engine);
    const auto after = engine.runtime_stats();
    require(after.decode_row_rounds - before.decode_row_rounds >
                after.decode_rounds - before.decode_rounds,
            "C2 fixture never executed a multi-row decode batch");
    std::cout << "C2 rows=" << after.decode_row_rounds - before.decode_row_rounds
              << " rounds=" << after.decode_rounds - before.decode_rounds
              << " sibling_reused=" << continued.reused_prompt_tokens << '\n';
}

class CancelAfterOutput final : public ninfer::OutputSink {
public:
    void start(ninfer::GenerationStart) override {}
    void progress(ninfer::PromptProgress) override {}
    void timing(ninfer::GenerationTimingObservation timing) override {
        if (timing.generated_tokens) { output.store(true, std::memory_order_release); }
    }
    void publish(ninfer::OutputDelta) override {}
    std::atomic<bool> output{false};
};

void cancellation(ninfer::Engine& engine) {
    auto sibling = engine.submit(engine.prepare_tokens({198, 1000, 198}), request(128, false));
    CancelAfterOutput sink;
    auto cancelled = engine.submit(engine.prepare_tokens({198, 1001, 198}), request(512, false),
                                    ninfer::OutputConsumerMode::Streaming,
                                    {.phase_timings = true, .live_timings = true});
    const auto result = cancelled.wait(&sink, ninfer::CancellationView([&] {
        return sink.output.load(std::memory_order_acquire);
    }));
    require(result.finish_reason == ninfer::FinishReason::Cancelled &&
                !result.generated_token_ids.empty() && result.generated_token_ids.size() < 512,
            "live cancellation did not stop just its own request");
    complete(sibling.wait(), 128);
    {
        auto abandoned = engine.submit(engine.prepare_tokens(std::vector<ninfer::TokenId>(2048, 198)),
                                        request(128, false));
    }
    complete(engine.generate(engine.prepare_tokens({198, 1002, 198}), request(4, false)), 4);
    settled(engine);
}

} // namespace

int main() {
    const char* artifact = std::getenv("NINFER_TEST_ARTIFACT");
    if (!artifact || !*artifact) {
        std::cout << "skip: NINFER_TEST_ARTIFACT is not set\n";
        return 77;
    }
    try {
        ninfer::EngineOptions options;
        options.artifact_path = artifact;
        options.max_context = 4096;
        options.kv_capacity = ninfer::KvCapacityPolicy::explicit_capacity(8192);
        options.max_concurrency = 2;
        options.max_pending_requests = 2;
        options.prefill_chunk = 256;
        options.context_cache.device_state_slots = 4;
        options.context_cache.host_capacity_bytes = 0;
        const char* backend = std::getenv("NINFER_TEST_BACKEND");
        if (!backend || std::string_view(backend) == "dflash2") {
            options.speculative.backend = ninfer::SpeculativeBackend::DFlash2;
            options.speculative.draft_tokens = 7;
            options.speculative.proposal_head = ninfer::ProposalHead::Optimized;
        } else {
            require(std::string_view(backend) == "none", "NINFER_TEST_BACKEND must be none or dflash2");
        }
        ninfer::Engine engine(options);
        checkpoint_policy_refusal(engine);
        checkpoint_failure_classification(engine);
        checkpoint_epoch_refusal(engine);
        inflight_bound(engine);
        pending_timeout(engine);
        shared_readers_and_live_sibling(engine);
        cancellation(engine);
        require(engine.is_available(), "C2 regressions made the engine unavailable");
        std::cout << "ok: inflight bound, queued timeout, shared readers, live sibling, cancellation\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
