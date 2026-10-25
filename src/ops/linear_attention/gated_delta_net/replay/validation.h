#pragma once

#include "ops/common/host/operands.h"
#include "ops/linear_attention/gated_delta_net/operands.h"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace ninfer::ops::detail::gated_delta_net::replay {
constexpr std::int32_t kStateDim    = ::ninfer::ops::detail::gated_delta_net::kStateDim;
constexpr std::int32_t kMaximumRows = 8;

inline common::operand::Blocks layer_blocks(Tensor layer0, std::int64_t stride_bytes,
                                            std::int32_t layer, const char* label) {
    const auto offset  = common::operand::bytes(layer, stride_bytes, label);
    const auto address = common::operand::range(layer0.data, offset, label).end;
    layer0.data        = reinterpret_cast<void*>(address);
    return common::operand::blocks(layer0, label);
}

} // namespace ninfer::ops::detail::gated_delta_net::replay
