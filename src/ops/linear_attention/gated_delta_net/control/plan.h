#pragma once

#include "ops/linear_attention/gated_delta_net/control/operands.h"

namespace ninfer::ops::detail::gdn_control {

enum class Kernel { Gemv, TiledGemv, SmallSplit, Mma };
enum class RowMode { Paired, Merged };
enum class NormKernel { Simt, Mma, Materialized };

struct Plan {
    Kernel kernel          = Kernel::Gemv;
    std::int32_t tile_cols = 1, split_k = 1;
    RowMode rows = RowMode::Paired;
};

struct NormPlan {
    NormKernel kernel;
    Plan projection;
    Plan fallback;
    int tile_cols = 0, threads = 0, capacity = 0;
};

Plan select_projection_plan(Geometry geometry, std::int32_t columns);
NormPlan select_norm_plan(Geometry geometry, std::int32_t columns);
std::size_t capacity(Geometry geometry, bool norm, std::int32_t first, std::int32_t last);

} // namespace ninfer::ops::detail::gdn_control
