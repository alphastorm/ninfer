#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace ninfer::test::linear_pair {

enum class WeightParents : std::uint8_t {
    Separate,
    Shared,
};

struct ShapeCase {
    std::int32_t k;
    std::uint32_t seed;
    WeightParents parents;
    std::int32_t parent_rows;
    std::int32_t first_row;
    std::int32_t second_row;
    std::span<const std::int32_t> route_starts;
    std::span<const std::int32_t> route_interiors;
};

int run_q8_a16_shape(std::string_view label, const ShapeCase& shape);

} // namespace ninfer::test::linear_pair
