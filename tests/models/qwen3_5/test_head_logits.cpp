#include "models/qwen3_5/program/round_buffers.h"
#include "models/qwen3_5/program/planning/startup.h"
#include "ninfer/ops/linear.h"

#include <iostream>
#include <stdexcept>

namespace {
using namespace ninfer;
namespace qwen = models::qwen3_5;
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

qwen::RoundStateLayout layout(SpeculativeBackend backend, std::uint32_t drafts, DType dtype,
                              std::size_t& bytes) {
    LayoutBuilder builder;
    auto out = qwen::begin_round_state_layout(
        builder, {.hidden = 5120, .output_rows = 248320, .batch_capacity = 2,
                  .draft_window = drafts, .backend = backend, .logits_dtype = dtype});
    qwen::complete_round_state_layout(builder, out);
    bytes = builder.finish(256);
    return out;
}

void check_layout(SpeculativeBackend backend, std::uint32_t drafts) {
    std::size_t bf16_bytes, fp32_bytes;
    const auto bf16 = layout(backend, drafts, DType::BF16, bf16_bytes);
    const auto fp32 = layout(backend, drafts, DType::FP32, fp32_bytes);
    expect(bf16.logits.dtype == DType::BF16 && fp32.logits.dtype == DType::FP32,
           "step logits retain the selected dtype");
    expect(fp32.logits.region.bytes == 993280 && bf16.logits.region.bytes == 496640,
           "step logits reserve actual dtype bytes");
    const auto& narrow = backend == SpeculativeBackend::None ? bf16.ordinary->logits
                         : backend == SpeculativeBackend::Mtp ? bf16.mtp_decode->target_logits
                                                             : bf16.dflash_decode->target_logits;
    const auto& wide = backend == SpeculativeBackend::None ? fp32.ordinary->logits
                       : backend == SpeculativeBackend::Mtp ? fp32.mtp_decode->target_logits
                                                           : fp32.dflash_decode->target_logits;
    const std::size_t columns = backend == SpeculativeBackend::None ? 2 : (drafts + 1) * 2;
    expect(wide.dtype == DType::FP32 && narrow.dtype == DType::BF16 &&
               wide.region.bytes == 248320 * columns * sizeof(float) &&
               narrow.region.bytes == 248320 * columns * sizeof(std::uint16_t),
           "ordinary and speculative target logits reserve actual dtype bytes");
    expect(fp32_bytes - bf16_bytes ==
               (fp32.logits.region.bytes - bf16.logits.region.bytes) +
                   (wide.region.bytes - narrow.region.bytes),
           "aligned persistent fit accounting includes every extra target byte");
    if (backend == SpeculativeBackend::DFlash2 && drafts == 7) {
        expect(wide.region.bytes == 15892480 && narrow.region.bytes == 7946240 &&
                   fp32_bytes - bf16_bytes == 8442880,
               "K7 C2 uses 15,892,480 target bytes and 8,442,880 extra persistent bytes");
    }
    if (backend == SpeculativeBackend::Mtp) {
        expect(fp32.mtp_decode->proposal_logits.dtype == DType::BF16 &&
                   fp32.mtp_decode->proposal_logits.region.bytes ==
                       bf16.mtp_decode->proposal_logits.region.bytes,
               "MTP proposal dtype and capacity are unchanged");
    }
}
} // namespace

int main() {
    expect(!ninfer::EngineOptions{}.fp32_head_logits, "FP32 head is opt-in");
    expect(qwen::RoundStateSpec{}.logits_dtype == DType::BF16, "round default remains BF16");
    check_layout(SpeculativeBackend::None, 0);
    check_layout(SpeculativeBackend::Mtp, 5);
    check_layout(SpeculativeBackend::DFlash, 7);
    check_layout(SpeculativeBackend::DFlash2, 7);
    check_layout(SpeculativeBackend::DFlash2, 15);
    for (const auto type : {QType::BF16, QType::Q4_G64_FP16, QType::Q5_G64_FP16,
                           QType::Q6_G64_FP16, QType::Q8_G32_FP16,
                           QType::FP8_E4M3FN_ROW_BF16, QType::NVFP4}) {
        expect(ops::linear_writes_fp32(type), "supported head formats write FP32 sums");
    }
    expect(!ops::linear_writes_fp32(QType::FP32) && !ops::linear_writes_fp32(QType::INT32),
           "unsupported weight formats do not claim FP32 output");
    expect(qwen::detail::causal_score_tile(DType::BF16) == 1024 &&
               qwen::detail::causal_score_tile(DType::FP32) == 512,
           "scoring retains its logits byte ceiling");
    try {
        LayoutBuilder builder;
        (void)qwen::begin_round_state_layout(
            builder, {.hidden = 5120, .output_rows = 248320, .logits_dtype = DType::I32});
        expect(false, "integer logits are rejected");
    } catch (const std::invalid_argument&) {}
    std::cout << (failures ? "FAIL" : "OK") << " head logits dtype and accounting\n";
    return failures ? 1 : 0;
}
