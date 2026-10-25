#pragma once
#include "fixtures.h"

namespace ninfer::test::hc::mix {
struct Column {
    std::vector<double> output, injection;
};

Column reference_column(const Weights& weights, std::span<const float> input);
std::vector<Column> reference(const Weights& weights, std::span<const float> catalog);
int verify(const Fixture& fixture, std::span<const Column> reference, const std::string& label);
int validate(Weights& weights, std::span<const Column> reference, CudaExecutionView execution);
} // namespace ninfer::test::hc::mix
