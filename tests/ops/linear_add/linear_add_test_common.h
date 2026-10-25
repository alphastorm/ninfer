#pragma once

#include "ninfer/ops/linear_add.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ninfer::test::linear_add {

enum class WeightFormat : std::uint8_t {
    BF16,
    Q4G64F16S,
    Q5G64F16S,
    Q8G32F16S,
    FP8,
    NVFP4,
};

enum class Arithmetic : std::uint8_t { A16, A8, A4 };
enum class FixturePattern : std::uint8_t { TokenBlocks, Hashed };
enum class Samples : std::uint8_t { All, BoundarySeven, Uniform32 };
enum class GraphReplay : std::uint8_t {
    None,
    OriginalZeroOriginal,
    OriginalNegative,
    OriginalTwice,
};

struct Invocation {
    std::int32_t tokens;
    std::optional<ops::LinearPolicy> policy{};
    Arithmetic arithmetic = Arithmetic::A16;
    GraphReplay graph     = GraphReplay::None;
    std::optional<ops::LinearPolicy> graph_policy{};
    std::optional<ops::LinearPolicy> followup_policy{};
};

struct InvocationCase {
    std::int32_t n;
    std::int32_t k;
    std::uint32_t seed;
    std::span<const Invocation> invocations;
    FixturePattern fixture = FixturePattern::TokenBlocks;
    Samples rows           = Samples::BoundarySeven;
    Samples columns        = Samples::All;
    bool scan_all_finite   = true;
    bool interval_witness  = false;
};

struct ShapeCase {
    std::int32_t n;
    std::int32_t k;
    std::uint32_t seed;
    std::span<const std::int32_t> route_starts;
    std::span<const std::int32_t> route_interiors;
    std::span<const std::int32_t> graph_tokens{};
    bool full_output = false;
    // Revisit each boundary in the residual tail after this many whole-wave columns.
    std::int32_t composite_offset = 0;
};

int run_cases(std::string_view label, WeightFormat format, const InvocationCase& shape);
int run_shape(std::string_view label, WeightFormat format, const ShapeCase& shape);

} // namespace ninfer::test::linear_add
