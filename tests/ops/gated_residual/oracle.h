#pragma once
#include "fixtures.h"

namespace ninfer::test::hc {
struct Reference {
    std::vector<double> residual, normalized;
    std::vector<std::uint16_t> residual_words;
};

Reference reference(const Fixture& fixture);
std::vector<double> grouped_norm(std::span<const double> input, std::span<const float> weight,
                                 int columns, float epsilon);
} // namespace ninfer::test::hc
