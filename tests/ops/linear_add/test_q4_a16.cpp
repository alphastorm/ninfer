#include "ops/linear_add/linear_add_test_common.h"
#include "ops/linear_common.h"

#include <array>

int main() {
    using namespace ninfer::test::linear_add;
    return ninfer::test::linear_common::run_test_main("Q4_A16 LinearAdd", [] {
        constexpr std::array<std::int32_t, 8> route_starts{2, 5, 9, 17, 25, 33, 97, 193};
        constexpr std::array<std::int32_t, 9> interiors{1, 4, 8, 64, 128, 512, 513, 1024, 1025};
        constexpr std::array<std::int32_t, 6> graph_tokens{1, 4, 33, 97, 193, 512};
        constexpr std::array<std::int32_t, 3> full_tokens{1, 4, 8};
        int failures = run_shape("Q4_A16 LinearAdd", WeightFormat::Q4G64F16S,
                                 ShapeCase{.n               = 5120,
                                           .k               = 6144,
                                           .seed            = 429U,
                                           .route_starts    = route_starts,
                                           .route_interiors = interiors,
                                           .graph_tokens    = graph_tokens});
        failures += run_shape("Q4_A16 LinearAdd full", WeightFormat::Q4G64F16S,
                              ShapeCase{.n               = 5120,
                                        .k               = 6144,
                                        .seed            = 431U,
                                        .route_starts    = {},
                                        .route_interiors = full_tokens,
                                        .full_output     = true});
        return failures;
    });
}
