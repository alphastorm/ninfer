#include "core/weight.h"
#include "ops/linear_swiglu/linear_swiglu_test_common.h"
#include "ops/linear_common.h"
#include "ops/op_tester.h"
#include "ops/quantized_weight.h"

#include <array>

namespace {
int check_negative_gate() {
    using namespace ninfer;
    namespace qw       = test::quantized_weight;
    constexpr int rows = 34816, k = 5120, half = rows / 2;
    auto packed = qw::make_patterned_weight(QType::FP8_E4M3FN_ROW_BF16, rows, k, 1817U);
    std::fill(packed.payload.begin(), packed.payload.end(), 0);
    for (int row = 0; row < rows; ++row) {
        // E4M3 -20 and +32, with independently represented BF16 row multipliers.
        packed.payload[static_cast<std::size_t>(row) * k] = row < half ? 0xda : 0x60;
        const auto scale = test::f32_to_bf16(row < half ? 1.0078125F : 1048576.0F);
        std::memcpy(packed.payload.data() + packed.scale_plane_offset + 2 * row, &scale, 2);
    }
    return test::linear_swiglu::run_negative_gate("FP8 SwiGLU", packed,
                                                  test::linear_swiglu::ActivationCompute::A8,
                                                  std::array{1, 64, 65, 257, 513}, std::array{0});
}
} // namespace

int main() {
    using namespace ninfer;
    using namespace ninfer::test::linear_swiglu;

    return test::linear_common::run_test_main("LinearSwiGLU FP8 correctness", [] {
        constexpr std::array kA16Cases{1,  2,  4,  5,  8,  9,  16,  17,  24,  25,
                                       32, 33, 64, 65, 96, 97, 128, 129, 1024};
        constexpr std::array kA8Cases{1,   2,   3,   4,   5,   6,   8,   15,  16,   17,   31,
                                      32,  33,  48,  63,  64,  65,  96,  127, 128,  129,  191,
                                      192, 193, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025};
        int failures = 0;
        failures += check_negative_gate();
        failures += run_profile(
            "LinearSwiGLU FP8_A16",
            {QType::FP8_E4M3FN_ROW_BF16, 34816, 5120, 17408, 1811U, ActivationCompute::A16},
            kA16Cases, std::array<std::int32_t, 1>{16});
        failures += run_profile(
            "LinearSwiGLU FP8_A8",
            {QType::FP8_E4M3FN_ROW_BF16, 34816, 5120, 17408, 1813U, ActivationCompute::A8},
            kA8Cases, std::array{4, 5, 65, 193, 257, 512, 513, 1025});
        return failures;
    });
}
