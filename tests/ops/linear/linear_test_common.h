#pragma once

#include "ninfer/ops/linear.h"
#include "ops/linear_common.h"
#include "ops/quantized_weight.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ninfer::test::linear {

enum class ActivationCompute : std::uint8_t {
    A16,
    A8,
    A4,
};

enum class CallForm : std::uint8_t {
    Policy,
    A16Convenience,
};

enum class Comparison : std::uint8_t {
    Full,
    Sampled,
};

struct Invocation {
    std::int32_t t;
    CallForm call_form       = CallForm::Policy;
    ops::LinearPolicy policy = ops::LinearPolicy::A16Only;
    bool graph_replay        = false;
};

enum class ActivationPattern : std::uint8_t { Dense, KTail, Cancellation };

using ActivationGenerator = std::vector<std::uint16_t> (*)(std::int32_t, std::int32_t,
                                                           std::uint32_t);
using IndexSampler        = std::vector<std::int32_t> (*)(std::int32_t);

struct ShapeCase {
    std::int32_t n;
    std::int32_t k;
    std::uint32_t seed;
    Comparison comparison;
    bool verify_input_preservation;
    std::span<const Invocation> invocations;
    ActivationPattern activation_pattern = ActivationPattern::Dense;
    // Explicit fixture recipes preserve different represented inputs and sample coordinates.
    // Null callbacks select this family's standard activation and evenly spaced 32-point sample.
    ActivationGenerator activation_generator = nullptr;
    IndexSampler row_sampler                 = nullptr;
    IndexSampler column_sampler              = nullptr;
    bool verify_each_invocation              = false;
};

using WeightGenerator = quantized_weight::PackedWeight (*)(std::int32_t, std::int32_t,
                                                           std::uint32_t);

quantized_weight::PackedWeight make_bf16_weight(std::int32_t n, std::int32_t k, std::uint32_t seed);

quantized_weight::PackedWeight make_q4_g64_fp16_weight(std::int32_t n, std::int32_t k,
                                                       std::uint32_t seed);
quantized_weight::PackedWeight make_q5_g64_fp16_weight(std::int32_t n, std::int32_t k,
                                                       std::uint32_t seed);
quantized_weight::PackedWeight make_q6_g64_fp16_weight(std::int32_t n, std::int32_t k,
                                                       std::uint32_t seed);
quantized_weight::PackedWeight make_q8_g32_fp16_weight(std::int32_t n, std::int32_t k,
                                                       std::uint32_t seed);
quantized_weight::PackedWeight make_nvfp4_weight(std::int32_t n, std::int32_t k,
                                                 std::uint32_t seed);
quantized_weight::PackedWeight make_fp8_weight(std::int32_t n, std::int32_t k, std::uint32_t seed);

void cpu_linear_gemm_fp64(const float* weight, const float* activation, double* output,
                          std::int32_t n, std::int32_t k, std::int32_t t);

int run_shape(std::string_view label, ActivationCompute activation_compute,
              WeightGenerator generator, const ShapeCase& shape);

// A declared token interval must reserve enough space for every public point it contains.
int verify_workspace_envelopes(QType qtype, std::int32_t n, std::int32_t k);

} // namespace ninfer::test::linear
