#include "ninfer/engine.h"
#include "kv_cache_storage.h"

#include <array>
#include <barrier>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
    const char* artifact = std::getenv("NINFER_TEST_ARTIFACT");
    if (!artifact || !*artifact) {
        std::cout << "SKIP: NINFER_TEST_ARTIFACT is required\n";
        return 77;
    }
    try {
        require(argc <= 2, "usage: shared_prefix_real_test [none|mtp|dflash2]");
        const std::string_view backend = argc == 2 ? argv[1] : "mtp";
        require(backend == "none" || backend == "mtp" || backend == "dflash2", "invalid backend");
        ninfer::EngineOptions options;
        options.artifact_path = artifact;
        options.max_context = 2048;
        options.kv_capacity = ninfer::KvCapacityPolicy::explicit_capacity(4096);
        options.prefill_chunk = 128;
        options.max_concurrency = 2;
        options.max_pending_requests = 2;
        const char* kv = std::getenv("NINFER_TEST_KV_DTYPE");
        options.kv_cache = ninfer::test::parse_kv_cache_storage(kv ? kv : "bf16");
        options.speculative.backend = backend == "mtp" ? ninfer::SpeculativeBackend::Mtp :
            backend == "dflash2" ? ninfer::SpeculativeBackend::DFlash2 : ninfer::SpeculativeBackend::None;
        options.speculative.draft_tokens = backend == "none" ? 0 : 3;
        options.speculative.proposal_head = ninfer::ProposalHead::Optimized;
        options.context_cache.device_state_slots = 2;
        options.context_cache.host_capacity_bytes = 1ULL << 30;
        ninfer::Engine engine(options);
        ninfer::RequestOptions request;
        request.execution.requested_output_tokens = 96;
        request.execution.sampling.temperature = 0;
        request.execution.allow_prefix_reuse = true;
        request.tool_choice.constraints = ninfer::ToolConstraintMode::Automatic;
        request.stop.include_model_defaults = false;
        std::uint64_t reused = 0;
        unsigned clients = 0;
        const auto before = engine.runtime_stats();
        unsigned partial_tail_clients = 0;
        // Cold identical submissions exercise capture while another client runs. Explicit
        // SharedStablePrefix markers make the primed donor non-consumable by the first client.
        for (const unsigned words : {65U, 258U, 515U}) {
            std::string system = "Shared fixture " + std::to_string(words) + ": ";
            for (unsigned i = 0; i < words; ++i) system += "alpha ";
            const auto prompt = [&](bool seed) {
                ninfer::PromptInput input;
                input.options.enable_thinking = false;
                input.context_cache.allow_engine_automatic_shared_prefixes = false;
                input.context_cache.markers.push_back({.after_message_count = 1,
                    .kind = ninfer::PromptCacheMarkerKind::SharedStablePrefix,
                    .location = ninfer::PromptCacheMarkerLocation::MessageBoundary});
                input.messages.push_back({.role = ninfer::ChatRole::System,
                    .parts = {{.kind = ninfer::MessagePartKind::Text, .text = system}}});
                input.messages.push_back({.role = ninfer::ChatRole::User,
                    .parts = {{.kind = ninfer::MessagePartKind::Text,
                        .text = seed ? "Name one short word." : "Describe the shared instructions in detail."}}});
                return input;
            };
            for (const bool primed : {false, true}) {
                if (primed) {
                    auto seed = request;
                    seed.execution.requested_output_tokens = 1;
                    require(engine.generate(engine.prepare(prompt(true)), seed).generated_token_ids.size() == 1,
                            "shared-prefix seed failed");
                }
                std::barrier start(2);
                const auto client = [&] {
                    auto prepared = engine.prepare(prompt(false));
                    start.arrive_and_wait();
                    return engine.generate(std::move(prepared), request);
                };
                auto first = std::async(std::launch::async, client);
                auto second = std::async(std::launch::async, client);
                for (auto* future : {&first, &second}) {
                    const auto result = future->get();
                    require(result.generated_token_ids.size() == 96 &&
                            result.finish_reason == ninfer::FinishReason::OutputLimit,
                            "shared-prefix client failed its output budget");
                    if (primed) {
                        require(result.reused_prompt_tokens > 0,
                                "primed client did not share its retained prefix");
                        partial_tail_clients += result.reused_prompt_tokens % 64 != 0;
                    }
                    reused += result.reused_prompt_tokens;
                    ++clients;
                }
                require(engine.is_available(), "shared-prefix clients poisoned the engine");
            }
        }
        const auto after = engine.runtime_stats();
        const auto rounds = after.decode_rounds - before.decode_rounds;
        const auto rows = after.decode_row_rounds - before.decode_row_rounds;
        require(rows > rounds, "two clients never executed a multi-row decode round");
        require(partial_tail_clients != 0, "shared-prefix clients never used an incomplete tail page");
        request.execution.requested_output_tokens = 4;
        require(engine.generate(engine.prepare_tokens({1000, 198}), request).generated_token_ids.size() == 4 &&
                engine.is_available(), "engine unavailable after shared-prefix traffic");
        std::cout << "PASS shared-prefix backend=" << backend << " kv=" << (kv ? kv : "bf16")
                  << " clients=" << clients
                  << " reused_tokens=" << reused << " decode_rounds=" << rounds
                  << " decode_rows=" << rows << " partial_tail_clients=" << partial_tail_clients
                  << " available=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL shared-prefix: " << error.what() << '\n';
        return 1;
    }
}
