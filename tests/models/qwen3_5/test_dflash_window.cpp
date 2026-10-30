#include "models/qwen3_5/program/round_buffers.h"
#include "product/speculative_options.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
namespace qwen = ninfer::models::qwen3_5;
int failures = 0;

void expect(bool condition, std::string_view message) {
    if (condition) { return; }
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

// Exercise the real layout and binding with Host memory, without a DeviceContext or CUDA call.
struct alignas(256) Block {
    std::array<std::byte, 256> bytes{};
};

std::size_t check_window(std::uint32_t k, std::uint32_t batch) {
    ninfer::product::validate_speculative_cli_options(
        {.backend = ninfer::SpeculativeBackend::DFlash2,
         .draft_tokens = k,
         .proposal_head = ninfer::ProposalHead::Optimized});
    ninfer::LayoutBuilder builder;
    auto layout = qwen::begin_round_state_layout(
        builder, {.hidden = 32, .output_rows = 128, .batch_capacity = batch,
                  .draft_window = k, .backend = ninfer::SpeculativeBackend::DFlash2});
    qwen::complete_round_state_layout(builder, layout);
    const auto bytes = builder.finish(256);
    expect(layout.complete && layout.dflash_prefill && layout.dflash_decode,
           "DFlash2 owns complete prefill and decode frames");
    expect(!layout.ordinary && !layout.mtp && !layout.mtp_decode,
           "changing the DFlash2 window does not allocate another backend");
    std::vector<Block> storage(bytes / sizeof(Block));
    qwen::DFlashDecodeState frame(
        {storage.data(), bytes, ninfer::Device::cpu()}, *layout.dflash_decode, batch, k);
    const auto width = static_cast<std::int32_t>(k + 1);
    const auto b = static_cast<std::int32_t>(batch);
    expect(frame.proposal_ids.ne[0] == width && frame.proposal_ids.ne[1] == b &&
               frame.verify_ids.ne[0] == width && frame.verify_ids.ne[1] == b &&
               frame.append_positions.ne[0] == width && frame.append_positions.ne[1] == b &&
               frame.target_rope_positions.ne[0] == width &&
               frame.target_rope_positions.ne[1] == b,
           "proposal, verify, append and RoPE use the configured K+1, not fixed width eight");
    expect(frame.draft_tokens.ne[0] == static_cast<std::int32_t>(k) &&
               frame.draft_tokens.ne[1] == b && frame.candidate_ids.ne[0] == 16 &&
               frame.candidate_ids.ne[1] == static_cast<std::int32_t>(k) &&
               frame.candidate_ids.ne[2] == b && frame.proposal_q.ne[0] == 16 &&
               frame.proposal_q.ne[1] == static_cast<std::int32_t>(k) &&
               frame.proposal_q.ne[2] == b,
           "candidate selector and proposal probabilities retain exactly K positions per row");
    expect(frame.target_logits.ne[0] == 128 && frame.target_logits.ne[1] == width &&
               frame.target_logits.ne[2] == b && frame.target_hidden.ne[0] == 32 &&
               frame.target_hidden.ne[1] == width && frame.target_hidden.ne[2] == b &&
               frame.licensed_tokens.ne[0] == width && frame.licensed_tokens.ne[1] == b,
           "target and accepted-output buffers follow the configured window");
    expect(frame.verify_ids.nb[1] == width * sizeof(ninfer::TokenId) &&
               frame.draft_tokens.nb[1] == k * sizeof(ninfer::TokenId) &&
               frame.candidate_ids.nb[2] == 16 * k * sizeof(ninfer::TokenId),
           "compact batch row strides follow K and K+1");
    expect(frame.target_continuation_hidden.ne[0] == 32 &&
               frame.target_continuation_hidden.ne[1] == b &&
               frame.proposal_extents.ne[0] == b && frame.licensed_counts.ne[0] == b,
           "one continuation and scalar extent/count are retained per row");
    return bytes;
}

void check_invalid(std::uint32_t k, std::uint32_t batch) {
    bool rejected = false;
    try {
        ninfer::LayoutBuilder builder;
        (void)qwen::begin_round_state_layout(
            builder, {.hidden = 32, .output_rows = 128, .batch_capacity = batch,
                      .draft_window = k, .backend = ninfer::SpeculativeBackend::DFlash2});
    } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "out-of-domain DFlash2 windows and batches are rejected before layout");
}
} // namespace

int main() {
    for (const auto batch : {1U, 8U}) {
        std::size_t previous = 0;
        for (const auto k : {1U, 3U, 5U, 6U, 7U, 15U}) {
            const auto bytes = check_window(k, batch);
            expect(bytes > previous, "larger windows reserve larger real round buffers");
            previous = bytes;
        }
    }
    check_invalid(0, 1);
    check_invalid(16, 1);
    check_invalid(7, 0);
    check_invalid(7, 9);
    if (failures) { return 1; }
    std::cout << "OK DFlash2 configured-window Host layout and binding\n";
    return 0;
}
