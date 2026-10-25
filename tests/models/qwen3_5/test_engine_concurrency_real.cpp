#include "ninfer/engine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
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

void pending_timeout(ninfer::Engine& engine) {
    require(engine.options().pending_timeout_ms == 30000, "shipped 30 s pending default changed");
    std::mutex mutex;
    std::condition_variable ready;
    std::array<bool, 2> started{};
    std::array<ninfer::GenerationHandle, 2> active;
    for (std::size_t row = 0; row < active.size(); ++row) {
        ninfer::GenerationObservationOptions observation;
        observation.first_token = [&, row](const auto&) {
            {
                std::lock_guard lock(mutex);
                started[row] = true;
            }
            ready.notify_one();
        };
        active[row] = engine.submit(
            engine.prepare_tokens({198, static_cast<ninfer::TokenId>(1000 + row), 198}),
            request(256, false), ninfer::OutputConsumerMode::Aggregate, observation);
    }
    {
        std::unique_lock lock(mutex);
        require(ready.wait_for(lock, std::chrono::seconds(30),
                               [&] { return started[0] && started[1]; }),
                "two requests never started before the pending-timeout fixture");
    }
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
    for (auto& handle : active) { complete(handle.wait(), 256); }
    settled(engine);
}

void shared_readers_and_live_sibling(ninfer::Engine& engine) {
    auto prefix = engine.tokenize_text(
        "Explain deterministic scheduling, cancellation and independent branch continuation. ");
    const auto seed = prefix;
    while (prefix.size() < 256) { prefix.insert(prefix.end(), seed.begin(), seed.end()); }
    const auto primed = engine.generate(engine.prepare_tokens(prefix), request(8));
    complete(primed, 8);
    prefix.insert(prefix.end(), primed.generated_token_ids.begin(), primed.generated_token_ids.end());
    prefix.push_back(198);
    auto other = prefix;
    other.push_back(5834);
    const auto before = engine.runtime_stats();
    auto older = engine.submit(engine.prepare_tokens(prefix), request(512));
    auto younger = engine.submit(engine.prepare_tokens(other), request(16));
    const auto branch = younger.wait();
    complete(branch, 16);
    require(branch.reused_prompt_tokens != 0, "second reader did not reuse the primed checkpoint");
    require(engine.runtime_stats().running_requests != 0,
            "short sibling waited for the long sibling to finish");
    other.insert(other.end(), branch.generated_token_ids.begin(), branch.generated_token_ids.end());
    other.push_back(198);
    const auto continued = engine.generate(engine.prepare_tokens(other), request(8));
    require(continued.prefix_reuse_path == ninfer::PrefixReusePath::PrivateEndpoint &&
                engine.runtime_stats().last_selected_reuse_path == continued.prefix_reuse_path,
            "successful sibling binding lost its actual checkpoint-role telemetry");
    const auto fresh = engine.generate(engine.prepare_tokens(other), request(8, false));
    complete(continued, 8);
    complete(fresh, 8);
    require(continued.reused_prompt_tokens != 0 && fresh.reused_prompt_tokens == 0,
            "live-sibling continuation did not preserve reuse policy");
    require(continued.generated_token_ids == fresh.generated_token_ids,
            "continuing a sibling changed deterministic tokens versus a fresh prefix");
    const auto long_branch = older.wait();
    complete(long_branch, 512);
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
        options.kv_capacity = ninfer::KvCapacityPolicy::explicit_capacity(16384);
        options.max_concurrency = 2;
        options.max_pending_requests = 2;
        options.prefill_chunk = 256;
        options.context_cache.device_state_slots = 4;
        options.context_cache.host_capacity_bytes = 0;
        const char* backend = std::getenv("NINFER_TEST_BACKEND");
        if (!backend || std::string_view(backend) == "dflash2") {
            options.speculative.backend = ninfer::SpeculativeBackend::DFlash2;
            options.speculative.draft_tokens = 7;
        } else {
            require(std::string_view(backend) == "none", "NINFER_TEST_BACKEND must be none or dflash2");
        }
        ninfer::Engine engine(options);
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
