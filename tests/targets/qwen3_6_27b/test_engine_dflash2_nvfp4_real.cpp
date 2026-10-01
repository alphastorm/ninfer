#include "ninfer/engine.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

ninfer::RequestOptions request(std::uint32_t outputs) {
    ninfer::RequestOptions options;
    options.execution.requested_output_tokens = outputs;
    options.execution.sampling.temperature    = 0.0F;
    options.execution.allow_prefix_reuse      = false;
    options.stop.include_model_defaults       = false;
    return options;
}

ninfer::PreparedPrompt prepare(ninfer::Engine& engine, unsigned fixture) {
    ninfer::PromptInput input;
    ninfer::ChatMessage user;
    user.role = ninfer::ChatRole::User;
    user.parts.push_back({
        .kind = ninfer::MessagePartKind::Text,
        .text = fixture == 0
                    ? "Continue counting upward, separated by commas. Output only the sequence, "
                      "with no commentary: 1, 2, 3, 4, 5,"
                    : "Repeat the words red green blue in that order at least one hundred times. "
                      "Output only those words, separated by spaces, with no commentary.",
        .media = {},
    });
    input.messages.push_back(std::move(user));
    input.options.enable_thinking = false;
    return engine.prepare(std::move(input));
}

void require_complete(const ninfer::GenerationResult& result, std::uint32_t outputs) {
    require(result.generated_token_ids.size() == outputs &&
                result.finish_reason == ninfer::FinishReason::OutputLimit,
            "NVFP4 real generation did not honor its output budget");
}

} // namespace

// Dedicated capacity qualification: no smaller-memory fallback is permitted. This is opt-in
// through the same real DFlash2 artifact environment variable as the existing integration tests.
int main() {
    const char* artifact = std::getenv("NINFER_QWEN3_8_27B_DFLASH2_WEIGHTS");
    if (!artifact || !*artifact) {
        std::cout << "skip: NINFER_QWEN3_8_27B_DFLASH2_WEIGHTS is not set\n";
        return 77;
    }
    try {
        ninfer::EngineOptions options;
        options.artifact_path                    = artifact;
        options.max_context                      = 131072;
        options.kv_capacity                      = ninfer::KvCapacityPolicy::automatic();
        options.prefill_chunk                    = 1024;
        options.max_concurrency                  = 2;
        options.context_cache.device_state_slots = 4;
        options.kv_cache                         = ninfer::KvCacheStorage::Nvfp4;
        options.use_cuda_graph                   = true;
        options.enable_vision                    = false;
        options.speculative.backend              = ninfer::SpeculativeBackend::DFlash2;
        options.speculative.draft_tokens          = 7;
        options.speculative.proposal_head         = ninfer::ProposalHead::Optimized;

        // The oracle is serial, eager, ordinary decoding with the same lossy NVFP4 storage.
        // BF16 tokens are not an exact oracle for a quantized KV cache. Destroy the oracle Engine
        // before constructing the capacity-qualified Engine so the two weight pools never overlap.
        std::array<std::vector<ninfer::TokenId>, 2> reference;
        {
            auto serial_options                 = options;
            serial_options.max_context          = 4096;
            serial_options.kv_capacity          = ninfer::KvCapacityPolicy::explicit_capacity(4096);
            serial_options.max_concurrency      = 1;
            serial_options.use_cuda_graph       = false;
            serial_options.speculative          = {};
            ninfer::Engine serial(serial_options);
            require(serial.options().kv_cache == ninfer::KvCacheStorage::Nvfp4 &&
                        serial.memory_summary().kv_cache == ninfer::KvCacheStorage::Nvfp4,
                    "serial oracle did not use NVFP4 KV storage");
            for (unsigned fixture = 0; fixture < reference.size(); ++fixture) {
                auto result = serial.generate(prepare(serial, fixture), request(64));
                require_complete(result, 64);
                reference[fixture] = std::move(result.generated_token_ids);
            }
        }
        require(!std::equal(reference[0].begin(), reference[0].begin() + 48,
                            reference[1].begin()),
                "NVFP4 isolation fixtures must produce distinct serial continuations");

        ninfer::Engine engine(options);
        const auto& effective = engine.options();
        const auto memory     = engine.memory_summary();
        require(effective.max_context == 131072 && memory.max_context == 131072 &&
                    effective.max_concurrency == 2,
                "NVFP4 qualification changed the B2 per-request context profile");
        require(memory.kv_capacity_mode == ninfer::KvCapacityMode::Automatic &&
                    memory.kv_capacity >= 262144,
                "NVFP4 automatic aggregate KV capacity is below 262144 tokens");
        require(effective.kv_cache == ninfer::KvCacheStorage::Nvfp4 &&
                    memory.kv_cache == ninfer::KvCacheStorage::Nvfp4 &&
                    effective.context_cache.device_state_slots == 4,
                "NVFP4 qualification lost its storage identity or four checkpoint slots");
        require(effective.use_cuda_graph &&
                    effective.speculative.backend == ninfer::SpeculativeBackend::DFlash2 &&
                    effective.speculative.draft_tokens == 7,
                "NVFP4 qualification did not retain graph-enabled DFlash2 K7");

        std::uint64_t accepted = 0;
        for (unsigned phase = 0; phase < 2; ++phase) {
            // Prepare both before submission; reverse their slots and unequal output budgets on
            // replay to expose stale graph ingress, block tables and cross-request state reuse.
            const std::array<unsigned, 2> fixtures{phase, 1U - phase};
            const std::array<std::uint32_t, 2> outputs{64U - 16U * phase, 48U + 16U * phase};
            std::array<ninfer::PreparedPrompt, 2> prompts{
                prepare(engine, fixtures[0]), prepare(engine, fixtures[1])};
            const auto before = engine.runtime_stats();
            std::array<ninfer::GenerationHandle, 2> handles{
                engine.submit(std::move(prompts[0]), request(outputs[0])),
                engine.submit(std::move(prompts[1]), request(outputs[1]))};
            for (unsigned row = 0; row < handles.size(); ++row) {
                const auto result = handles[row].wait();
                require_complete(result, outputs[row]);
                require(result.speculative.backend == ninfer::SpeculativeBackend::DFlash2 &&
                            result.speculative.rounds > 1 &&
                            result.speculative.accepted_tokens != 0,
                        "NVFP4 batch row bypassed repeated DFlash2 verify rounds");
                require(std::equal(result.generated_token_ids.begin(),
                                   result.generated_token_ids.end(), reference[fixtures[row]].begin()),
                        "NVFP4 graph batch row differs from its serial NVFP4 oracle");
                accepted += result.speculative.accepted_tokens;
            }
            const auto after = engine.runtime_stats();
            // Graph-enabled DFlash2 dispatch selects a captured executable for every decode
            // round. Prove B2 actually ran, rather than two serial submissions merely passing.
            require(after.decode_row_rounds - before.decode_row_rounds >
                        after.decode_rounds - before.decode_rounds,
                    "NVFP4 qualification did not execute a two-row decode batch");
        }
        std::cout << "ok kv=nvfp4 K=7 B=2 graph=1 max_context=" << memory.max_context
                  << " kv_capacity=" << memory.kv_capacity << " device_state_slots="
                  << *effective.context_cache.device_state_slots << " accepted=" << accepted
                  << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
