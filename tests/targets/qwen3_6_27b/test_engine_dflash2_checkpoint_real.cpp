#include "ninfer/engine.h"
#include "runtime/contract/continuation_checkpoint.h"
#include "runtime/engine/checkpoint_engine_access.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A DFlash2 session checkpoint carries the draft context ring inside its StateImages. Exported
// from one Engine and restored into a fresh one, the session must decode its next turn exactly as
// the exporting Engine does: the same tokens, rounds, accepted drafts and reused prefix. The
// session is longer than the 2,048-position ring, so the restored ring has wrapped.
namespace {

using ninfer::runtime::CheckpointEngineAccess;
using ninfer::runtime::ContinuationCheckpointStats;

constexpr std::string_view kSession =
    "d2f1a5e0c3b7d9e2f4a6c8b0d1e3f5a7c9b2d4e6f8a0c1e3d5f7b9a2c4e6f8a0";
constexpr std::size_t kStagingBytes = 256ULL << 20;

void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

// The Program writes each checkpoint file in pieces at offsets within its declared total size.
class MemoryCheckpoint final : public ninfer::runtime::ContinuationCheckpointWriter,
                               public ninfer::runtime::ContinuationCheckpointReader {
public:
    bool write_file(std::string_view path, std::uint64_t offset, std::uint64_t total_bytes,
                    std::span<const std::byte> bytes) override {
        auto& file = files_[std::string(path)];
        if (file.size() != total_bytes) { file.assign(total_bytes, std::byte{0}); }
        if (offset > total_bytes || bytes.size() > total_bytes - offset) { return false; }
        std::copy(bytes.begin(), bytes.end(), file.begin() + static_cast<std::ptrdiff_t>(offset));
        return true;
    }

    [[nodiscard]] std::optional<std::uint64_t> file_size(std::string_view path) const override {
        const auto found = files_.find(std::string(path));
        if (found == files_.end()) { return std::nullopt; }
        return found->second.size();
    }

    bool read_file(std::string_view path, std::uint64_t offset,
                   std::span<std::byte> destination) const override {
        const auto found = files_.find(std::string(path));
        if (found == files_.end() || offset > found->second.size() ||
            destination.size() > found->second.size() - offset) {
            return false;
        }
        std::copy_n(found->second.begin() + static_cast<std::ptrdiff_t>(offset), destination.size(),
                    destination.begin());
        return true;
    }

    [[nodiscard]] bool contains(std::string_view path) const {
        return files_.contains(std::string(path));
    }

private:
    std::map<std::string, std::vector<std::byte>> files_;
};

ninfer::ChatMessage message(ninfer::ChatRole role, std::string text) {
    ninfer::ChatMessage out;
    out.role = role;
    out.parts.push_back(
        {.kind = ninfer::MessagePartKind::Text, .text = std::move(text), .media = {}});
    return out;
}

// About 3,300 tokens of copyable lines, so drafts are accepted and the ring wraps.
std::string ledger() {
    std::string text = "Operations ledger. Each line names a desk, a code and a status.\n";
    for (int line = 1; line <= 160; ++line) {
        text += "Line " + std::to_string(line) + ": desk " + std::to_string(line * 7 % 31) +
                " reports code " + std::to_string(1000 + line * 37 % 997) + " and status " +
                (line % 3 == 0 ? "amber" : "green") + ".\n";
    }
    return text;
}

ninfer::PreparedPrompt prepare(ninfer::Engine& engine, std::vector<ninfer::ChatMessage> messages,
                               std::string tag) {
    ninfer::PromptInput input;
    input.messages                  = std::move(messages);
    input.options.enable_thinking   = false;
    input.context_cache.session_key = "http:" + std::string(kSession);
    input.context_cache.retention   = ninfer::CacheRetentionHint::LiveSession;
    ninfer::PreparedPrompt prompt   = engine.prepare(std::move(input));
    CheckpointEngineAccess::set_checkpoint_tag(prompt, std::move(tag));
    return prompt;
}

ninfer::RequestOptions request(std::uint32_t outputs) {
    ninfer::RequestOptions options;
    options.execution.requested_output_tokens = outputs;
    options.execution.sampling.temperature    = 0.0F;
    options.execution.allow_prefix_reuse      = true;
    return options;
}

std::string describe(const ninfer::GenerationResult& result) {
    return "tokens=" + std::to_string(result.generated_token_ids.size()) +
           " rounds=" + std::to_string(result.speculative.rounds) +
           " accepted=" + std::to_string(result.speculative.accepted_tokens) + "/" +
           std::to_string(result.speculative.drafted_tokens) +
           " reused=" + std::to_string(result.reused_prompt_tokens) +
           " path=" + std::to_string(static_cast<int>(result.prefix_reuse_path));
}
} // namespace

// Optional arguments: K and optimized-head selection, as in the DFlash2 real test.
int main(int argc, char** argv) {
    const char* artifact = std::getenv("NINFER_QWEN3_8_27B_DFLASH2_WEIGHTS");
    if (!artifact || !*artifact) {
        std::cout << "skip: NINFER_QWEN3_8_27B_DFLASH2_WEIGHTS is not set\n";
        return 77;
    }
    try {
        ninfer::EngineOptions options;
        options.artifact_path       = artifact;
        options.max_context         = 8192;
        options.kv_capacity         = ninfer::KvCapacityPolicy::explicit_capacity(2 * 8192);
        options.prefill_chunk       = 1024;
        options.max_concurrency     = 2;
        options.speculative.backend = ninfer::SpeculativeBackend::DFlash2;
        options.speculative.draft_tokens =
            argc > 1 ? static_cast<unsigned>(std::stoul(argv[1])) : 7U;
        options.speculative.proposal_head            = argc > 2 && std::stoi(argv[2]) == 0
                                                           ? ninfer::ProposalHead::Full
                                                           : ninfer::ProposalHead::Optimized;
        options.context_cache.host_state_slots       = 4;
        options.context_cache.host_kv_capacity_bytes = 1ULL << 30;

        const auto system =
            message(ninfer::ChatRole::System, "Answer with the requested ledger lines only.");
        const auto first_user =
            message(ninfer::ChatRole::User, ledger() + "\nRepeat lines 150 through 154 exactly.");
        const auto second_user =
            message(ninfer::ChatRole::User, "Now repeat lines 20 through 24 exactly.");

        MemoryCheckpoint files;
        ContinuationCheckpointStats exported;
        ninfer::GenerationResult first;
        ninfer::GenerationResult live;
        std::vector<ninfer::ChatMessage> second_turn;
        {
            ninfer::Engine source(options);
            first = source.generate(prepare(source, {system, first_user}, "turn-1"), request(128));
            require(first.speculative.backend == ninfer::SpeculativeBackend::DFlash2 &&
                        first.speculative.accepted_tokens != 0,
                    "first turn did not decode through DFlash2: " + describe(first));
            require(first.prompt.prompt_tokens > 2048,
                    "first turn does not wrap the 2,048-position ring");

            ninfer::runtime::SessionCheckpointSkipDetail skip;
            const auto saved = CheckpointEngineAccess::checkpoint_session(
                source, kSession, "turn-1", files, kStagingBytes, &skip);
            require(
                saved.has_value(),
                "DFlash2 session export was refused: " +
                    std::string(ninfer::runtime::session_checkpoint_skip_reason_name(skip.reason)) +
                    " / " +
                    std::string(ninfer::runtime::continuation_export_skip_reason_name(
                        skip.export_detail.reason)));
            exported = *saved;
            require(exported.restored_tokens >= first.prompt.prompt_tokens,
                    "export did not cover the first turn's prompt");
            require(files.contains("engine/continuation.bin") &&
                        files.contains("engine/state/0.bin") &&
                        files.contains("engine/text-kv.bin") &&
                        !files.contains("engine/backend-kv.bin"),
                    "DFlash2 export wrote an unexpected file set");

            second_turn = {system, first_user, message(ninfer::ChatRole::Assistant, first.content),
                           second_user};
            live        = source.generate(prepare(source, second_turn, "turn-2"), request(96));
            require(live.reused_prompt_tokens > 2048,
                    "exporting Engine did not reuse the first turn: " + describe(live));
        }

        ninfer::Engine restored(options);
        ninfer::runtime::SessionRestoreSkipDetail restore_skip;
        const auto imported = CheckpointEngineAccess::restore_session(
            restored, kSession, "turn-1", files, exported, kStagingBytes, &restore_skip);
        require(imported.has_value() && *imported == exported,
                "DFlash2 session restore was refused: " +
                    std::string(
                        ninfer::runtime::session_restore_skip_reason_name(restore_skip.reason)) +
                    " / " +
                    std::string(ninfer::runtime::continuation_import_skip_reason_name(
                        restore_skip.import_reason)));
        const auto resumed =
            restored.generate(prepare(restored, second_turn, "turn-2"), request(96));
        require(resumed.generated_token_ids == live.generated_token_ids,
                "restored DFlash2 session decoded different tokens: live " + describe(live) +
                    ", restored " + describe(resumed));
        require(resumed.speculative.rounds == live.speculative.rounds &&
                    resumed.speculative.accepted_tokens == live.speculative.accepted_tokens &&
                    resumed.speculative.accepted_per_position ==
                        live.speculative.accepted_per_position,
                "restored DFlash2 ring drafted differently: live " + describe(live) +
                    ", restored " + describe(resumed));
        require(resumed.reused_prompt_tokens == live.reused_prompt_tokens &&
                    resumed.prefix_reuse_path == live.prefix_reuse_path,
                "restored session reused a different prefix: live " + describe(live) +
                    ", restored " + describe(resumed));
        std::cout << "ok K=" << options.speculative.draft_tokens
                  << " exported=" << exported.restored_tokens << " tokens/"
                  << exported.payload_bytes << " bytes; live " << describe(live) << "; restored "
                  << describe(resumed) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
