#include "ops/linear_pair/linear_pair_test_common.h"
#include "ops/linear_common.h"

#include <array>

namespace {

using ninfer::test::linear_pair::ShapeCase;
using ninfer::test::linear_pair::WeightParents;

int q8_a16_conformance() {
    int failures = 0;

    constexpr std::array<std::int32_t, 2> kK5120RouteStarts{86, 961};
    constexpr std::array<std::int32_t, 3> kK5120RouteInteriors{24, 512, 1024};
    failures += ninfer::test::linear_pair::run_q8_a16_shape(
        "Q8_A16 LinearPair", ShapeCase{.k               = 5120,
                                       .seed            = 431U,
                                       .parents         = WeightParents::Separate,
                                       .parent_rows     = 1024,
                                       .first_row       = 0,
                                       .second_row      = 0,
                                       .route_starts    = kK5120RouteStarts,
                                       .route_interiors = kK5120RouteInteriors});

    constexpr std::array<std::int32_t, 36> kK2048RouteStarts{
        2,    33,   49,   65,   81,   89,   97,   105,  113,  129,  161,  193,
        385,  481,  641,  642,  673,  681,  785,  897,  961,  977,  1281, 1317,
        1345, 1346, 1441, 1467, 1681, 1709, 1921, 1923, 2017, 2019, 2209, 2271,
    };
    constexpr std::array<std::int32_t, 37> kK2048RouteInteriors{
        1,    16,   40,   56,   72,   84,   92,   100,  108,  120,  144,  176,  288,
        432,  560,  641,  656,  676,  736,  840,  928,  968,  1120, 1296, 1332, 1345,
        1392, 1456, 1576, 1696, 1816, 1921, 1968, 2017, 2112, 2240, 4096,
    };
    failures += ninfer::test::linear_pair::run_q8_a16_shape(
        "Q8_A16 LinearPair", ShapeCase{.k               = 2048,
                                       .seed            = 433U,
                                       .parents         = WeightParents::Shared,
                                       .parent_rows     = 6144,
                                       .first_row       = 4096,
                                       .second_row      = 5120,
                                       .route_starts    = kK2048RouteStarts,
                                       .route_interiors = kK2048RouteInteriors});
    return failures;
}

} // namespace

int main() {
    return ninfer::test::linear_common::run_test_main("Q8_A16 LinearPair", q8_a16_conformance);
}
