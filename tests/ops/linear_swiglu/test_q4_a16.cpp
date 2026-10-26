#include "core/weight.h"
#include "ops/linear_swiglu/linear_swiglu_test_common.h"
#include "ops/linear_common.h"

#include <array>

int main() {
    using namespace ninfer;
    using namespace ninfer::test::linear_swiglu;

    return test::linear_common::run_test_main("LinearSwiGLU Q4_A16 correctness", [] {
        // Public numerical cases straddle each registered Q4 implementation interval. They make
        // no assertion about the private route selected for any T.
        constexpr std::array<std::int32_t, 57> kTokenCases{
            1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,
            13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,
            25,  26,  27,  28,  29,  30,  31,  32,  33,
            40,  41,  48,  49,  96,  128, 129, 152, 153, 168, 169, 176,
            192, 224, 225, 256, 257, 384, 385, 512, 513, 640, 641, 1024,
        };
        return run_profile("LinearSwiGLU Q4_A16",
                           {QType::Q4_G64_FP16, 34816, 5120, 17408, 1401U, ActivationCompute::A16},
                           kTokenCases,
                           std::array<std::int32_t, 12>{4, 7, 8, 9, 16, 25, 32, 49, 128, 152, 168, 1024});
    });
}
