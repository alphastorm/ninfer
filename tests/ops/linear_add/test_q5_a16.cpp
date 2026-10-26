#include "ops/linear_add/linear_add_test_common.h"
#include "ops/linear_common.h"

#include <array>

namespace {

using ninfer::test::linear_add::ShapeCase;
using ninfer::test::linear_add::WeightFormat;

int q5_a16_conformance() {
    // Exercise the production transitions, including the same transitions in
    // a composite tail and the 192-token limit on that tail.
    constexpr std::array<std::int32_t, 15> starts{5, 8, 9, 16, 17, 25, 33, 49, 65,
                                                  97, 129, 161, 193, 257, 513};
    constexpr std::array<std::int32_t, 13> interiors{1,  2,   3,   7,   13,  20,  40,
                                                     80, 112, 144, 224, 768, 1024};
    constexpr std::array<std::int32_t, 12> graphs{8, 13, 16, 40, 80, 112, 128, 192, 513, 545, 609, 705};
    constexpr std::array<std::int32_t, 0> no_starts{};
    constexpr std::array<std::int32_t, 8> full{8, 13, 16, 32, 40, 80, 112, 192};
    int failures = 0;
    for (const auto k : {6144, 17408}) {
        failures += ninfer::test::linear_add::run_shape("Q5_A16 LinearAdd", WeightFormat::Q5G64F16S,
                                                        ShapeCase{.n                = 5120,
                                                                  .k                = k,
                                                                  .seed             = 401U,
                                                                  .route_starts     = starts,
                                                                  .route_interiors  = interiors,
                                                                  .graph_tokens     = graphs,
                                                                  .composite_offset = 512});
        failures += ninfer::test::linear_add::run_shape("Q5_A16 LinearAdd", WeightFormat::Q5G64F16S,
                                                        ShapeCase{.n               = 5120,
                                                                  .k               = k,
                                                                  .seed            = 401U,
                                                                  .route_starts    = no_starts,
                                                                  .route_interiors = full,
                                                                  .full_output     = true});
        using namespace ninfer::test::linear_add;
        using ninfer::ops::LinearPolicy;
        constexpr std::array<Invocation, 12> a8{{
            {31, LinearPolicy::AllowA8, Arithmetic::A16},
            {32, LinearPolicy::AllowA8, Arithmetic::A16},
            {33, LinearPolicy::AllowA8, Arithmetic::A8, GraphReplay::OriginalZeroOriginal},
            {127, LinearPolicy::AllowA8, Arithmetic::A8},
            {128, LinearPolicy::AllowA8, Arithmetic::A8},
            {129, LinearPolicy::AllowA8, Arithmetic::A8, GraphReplay::OriginalNegative},
            {255, LinearPolicy::AllowA8, Arithmetic::A8},
            {256, LinearPolicy::AllowA8, Arithmetic::A8},
            {257, LinearPolicy::AllowA4, Arithmetic::A8},
            {512, LinearPolicy::AllowA8, Arithmetic::A8},
            {513, LinearPolicy::AllowA8, Arithmetic::A8, GraphReplay::OriginalZeroOriginal},
            {1024, LinearPolicy::AllowA8, Arithmetic::A8},
        }};
        failures += run_cases("Q5 allow-A8 LinearAdd", WeightFormat::Q5G64F16S,
                              {.n = 5120, .k = k, .seed = 509U, .invocations = a8,
                               .fixture = FixturePattern::Hashed, .rows = Samples::Uniform32});
    }
    return failures;
}

} // namespace

int main() {
    return ninfer::test::linear_common::run_test_main("Q5_A16 LinearAdd", q5_a16_conformance);
}
