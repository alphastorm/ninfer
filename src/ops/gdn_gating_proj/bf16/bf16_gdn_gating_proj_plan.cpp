#include "ops/gdn_gating_proj/bf16/bf16_gdn_gating_proj_plan.h"

#include "ninfer/ops/rmsnorm.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace ninfer::ops::detail {
namespace {

inline constexpr std::int32_t kAnyCols = std::numeric_limits<std::int32_t>::max();

struct ColsSet {
    std::int32_t first;
    std::int32_t last;

    constexpr bool contains(std::int32_t cols) const noexcept {
        return cols >= first && cols <= last;
    }
};

struct RouteSpec {
    ColsSet cols;
    Bf16GdnGatingScheduleId schedule;
};

// Preferred schedule per column interval, tuned on the RTX 5090 (170 SMs). A cooperative split
// is only legal while its whole grid is co-resident, and that ceiling is a property of the
// device the process runs on, so a route whose grid does not fit falls through to the next
// cheaper split (and finally to the unsplit GEMM, which has no residency constraint) at plan
// resolution rather than being encoded per architecture.
constexpr std::array<RouteSpec, 6> k27Routes{{
    {{1, 1}, Bf16GdnGatingScheduleId::GemvPairedRows},
    {{2, 8}, Bf16GdnGatingScheduleId::SmallTSplit10},
    // As token tiles double, halve SplitK. This keeps the cooperative grid near 192 CTAs instead
    // of making T a launch limit. Once the unsplit grid has enough independent work, it also
    // removes the cooperative-residency constraint.
    {{9, 1024}, Bf16GdnGatingScheduleId::MmaCooperativeSplit8},
    {{1025, 2048}, Bf16GdnGatingScheduleId::MmaCooperativeSplit4},
    {{2049, 4096}, Bf16GdnGatingScheduleId::MmaCooperativeSplit2},
    {{4097, kAnyCols}, Bf16GdnGatingScheduleId::MmaUnsplit},
}};

constexpr std::array<RouteSpec, 5> k35Routes{{
    // The same progression keeps the long-range cooperative routes near 256 CTAs.
    {{1, 127}, Bf16GdnGatingScheduleId::MmaCooperativeSplit16},
    {{128, 1024}, Bf16GdnGatingScheduleId::MmaCooperativeSplit8},
    {{1025, 2048}, Bf16GdnGatingScheduleId::MmaCooperativeSplit4},
    {{2049, 4096}, Bf16GdnGatingScheduleId::MmaCooperativeSplit2},
    {{4097, kAnyCols}, Bf16GdnGatingScheduleId::MmaUnsplit},
}};

// The next cheaper cooperative split, ending at the unsplit GEMM.
constexpr Bf16GdnGatingScheduleId narrower_split(Bf16GdnGatingScheduleId schedule) noexcept {
    switch (schedule) {
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
        return Bf16GdnGatingScheduleId::MmaCooperativeSplit16;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
        return Bf16GdnGatingScheduleId::MmaCooperativeSplit8;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
        return Bf16GdnGatingScheduleId::MmaCooperativeSplit4;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
        return Bf16GdnGatingScheduleId::MmaCooperativeSplit2;
    default:
        return Bf16GdnGatingScheduleId::MmaUnsplit;
    }
}

template <std::size_t N>
constexpr bool catalog_is_closed(const std::array<RouteSpec, N>& routes,
                                 std::int32_t last) noexcept {
    std::int64_t expected = 1;
    for (const RouteSpec& route : routes) {
        if (route.cols.first != expected || route.cols.last < route.cols.first) { return false; }
        expected = static_cast<std::int64_t>(route.cols.last) + 1;
    }
    return routes.back().cols.last == last && expected == static_cast<std::int64_t>(last) + 1;
}

static_assert(catalog_is_closed(k27Routes, kAnyCols));
static_assert(catalog_is_closed(k35Routes, kAnyCols));

bool is_27(const Bf16GdnGatingProblem& problem) noexcept {
    return problem.heads == 48 && problem.input_rows == 5120;
}

bool is_35(const Bf16GdnGatingProblem& problem) noexcept {
    return problem.heads == 32 && problem.input_rows == 2048;
}

bool schedule_uses_mma(Bf16GdnGatingScheduleId schedule) noexcept {
    switch (schedule) {
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit2:
    case Bf16GdnGatingScheduleId::MmaUnsplit:
        return true;
    case Bf16GdnGatingScheduleId::GemvPairedRows:
    case Bf16GdnGatingScheduleId::SmallTSplit10:
    case Bf16GdnGatingScheduleId::SimtWarpRowC4:
    case Bf16GdnGatingScheduleId::SimtWarpRowC8:
        return false;
    }
    return false;
}

std::int32_t mma_tile_cols(const Bf16GdnGatingProblem& problem) noexcept {
    return is_35(problem) ? 64 : 128;
}

std::int32_t schedule_split_k(Bf16GdnGatingScheduleId schedule) {
    switch (schedule) {
    case Bf16GdnGatingScheduleId::SmallTSplit10:
        return 10;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
        return 32;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
        return 16;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
        return 8;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
        return 4;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit2:
        return 2;
    case Bf16GdnGatingScheduleId::GemvPairedRows:
    case Bf16GdnGatingScheduleId::SimtWarpRowC4:
    case Bf16GdnGatingScheduleId::SimtWarpRowC8:
    case Bf16GdnGatingScheduleId::MmaUnsplit:
        return 1;
    }
    throw std::logic_error("BF16 GDN gating: unknown schedule");
}

bool schedule_is_cooperative(Bf16GdnGatingScheduleId schedule) noexcept {
    switch (schedule) {
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit2:
        return true;
    default:
        return false;
    }
}

// The cooperative GEMM's grid shape per exact problem: 27B runs BN128 tiles with three 16-row
// tiles per token tile, 35B BN64 tiles with two.
struct CooperativeGeometry {
    std::int32_t tile_cols;
    std::int32_t row_tiles;
    bool geometry_35;
};

constexpr CooperativeGeometry k27Cooperative{128, 3, false};
constexpr CooperativeGeometry k35Cooperative{64, 2, true};

const CooperativeGeometry& cooperative_geometry(const Bf16GdnGatingProblem& problem) noexcept {
    return is_35(problem) ? k35Cooperative : k27Cooperative;
}

// The largest column count whose cooperative grid is co-resident for this split on the current
// device: whole column tiles up to the driver's occupancy for the exact instantiation times the
// SM count (RTX 5090: 340 CTAs for the 27B splits, 340 for the 35B split32 and 680 below). A
// split fits exactly the column counts up to this bound, so it is both the legality test and,
// for capacity sizing, the last column count at which the split is still resolved.
std::int32_t cooperative_fit_cols(Bf16GdnGatingScheduleId schedule,
                                  const CooperativeGeometry& geometry) {
    const std::int32_t split_k = schedule_split_k(schedule);
    const std::int64_t budget =
        bf16_gdn_gating_proj_cooperative_resident_ctas(split_k, geometry.geometry_35);
    const std::int64_t column_tiles =
        budget / (static_cast<std::int64_t>(geometry.row_tiles) * split_k);
    return static_cast<std::int32_t>(
        std::min<std::int64_t>(column_tiles * geometry.tile_cols, kAnyCols));
}

bool cooperative_grid_is_resident(Bf16GdnGatingScheduleId schedule,
                                  const Bf16GdnGatingProblem& problem) {
    return problem.cols <= cooperative_fit_cols(schedule, cooperative_geometry(problem));
}

bool candidate_is_legal(Bf16GdnGatingScheduleId schedule,
                        const Bf16GdnGatingProblem& problem) noexcept {
    if (!bf16_gdn_gating_admits(problem)) { return false; }
    if (is_27(problem)) {
        switch (schedule) {
        case Bf16GdnGatingScheduleId::GemvPairedRows:
            return problem.cols == 1;
        case Bf16GdnGatingScheduleId::SmallTSplit10:
            return problem.cols >= 2 && problem.cols <= 8;
        case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
        case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
        case Bf16GdnGatingScheduleId::MmaCooperativeSplit2:
            return cooperative_grid_is_resident(schedule, problem);
        case Bf16GdnGatingScheduleId::MmaUnsplit:
            return true;
        case Bf16GdnGatingScheduleId::SimtWarpRowC4:
        case Bf16GdnGatingScheduleId::SimtWarpRowC8:
        case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
        case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
            return false;
        }
    }

    switch (schedule) {
    case Bf16GdnGatingScheduleId::SimtWarpRowC4:
        return problem.cols <= 4 * 65'535;
    case Bf16GdnGatingScheduleId::SimtWarpRowC8:
        return problem.cols <= 8 * 65'535;
    case Bf16GdnGatingScheduleId::MmaUnsplit:
        return true;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit2:
        return cooperative_grid_is_resident(schedule, problem);
    case Bf16GdnGatingScheduleId::GemvPairedRows:
    case Bf16GdnGatingScheduleId::SmallTSplit10:
        return false;
    }
    return false;
}

std::size_t checked_partial_bytes(std::int32_t heads, std::int32_t split_k, std::int32_t cols) {
    const std::size_t logical_rows = static_cast<std::size_t>(2 * heads);
    const std::size_t split        = static_cast<std::size_t>(split_k);
    const std::size_t tokens       = static_cast<std::size_t>(cols);
    if (tokens > std::numeric_limits<std::size_t>::max() / logical_rows ||
        split > std::numeric_limits<std::size_t>::max() / (tokens * logical_rows)) {
        throw std::overflow_error("BF16 GDN gating workspace element count overflows size_t");
    }
    const std::size_t elements = split * tokens * logical_rows;
    if (elements > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        throw std::overflow_error("BF16 GDN gating workspace byte count overflows size_t");
    }
    return elements * sizeof(float);
}

void execute_resolved(const Bf16GdnGatingPlan& plan, const Bf16GdnGatingProblem& problem,
                      const Tensor& x, const Weight& a_weight, const Weight& b_weight,
                      const Tensor& A_log, const Tensor& dt_bias, WorkspaceArena& ws, Tensor& g,
                      Tensor& beta, cudaStream_t stream) {
    auto scratch_scope = ws.scope();
    DeviceSpan scratch{};
    if (plan.workspace_bytes != 0) { scratch = ws.alloc_bytes(plan.workspace_bytes); }

    switch (plan.schedule) {
    case Bf16GdnGatingScheduleId::GemvPairedRows:
        bf16_gdn_gating_proj_gemv_launch(x, a_weight, b_weight, A_log, dt_bias, g, beta, stream);
        return;
    case Bf16GdnGatingScheduleId::SmallTSplit10:
        bf16_gdn_gating_proj_small_t_split10_launch(x, a_weight, b_weight, A_log, dt_bias,
                                                    scratch.data, scratch.bytes, g, beta, stream);
        return;
    case Bf16GdnGatingScheduleId::SimtWarpRowC4:
        bf16_gdn_gating_proj_35_simt_c4_launch(x, a_weight, b_weight, A_log, dt_bias, g, beta,
                                               stream);
        return;
    case Bf16GdnGatingScheduleId::SimtWarpRowC8:
        bf16_gdn_gating_proj_35_simt_c8_launch(x, a_weight, b_weight, A_log, dt_bias, g, beta,
                                               stream);
        return;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
        bf16_gdn_gating_proj_35_mma_split32_launch(plan.token_variant, x, a_weight, b_weight, A_log,
                                                   dt_bias, scratch.data, g, beta, stream);
        return;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
        bf16_gdn_gating_proj_35_mma_split16_launch(plan.token_variant, x, a_weight, b_weight, A_log,
                                                   dt_bias, scratch.data, g, beta, stream);
        return;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
        if (is_35(problem)) {
            bf16_gdn_gating_proj_35_mma_split8_launch(plan.token_variant, x, a_weight, b_weight,
                                                      A_log, dt_bias, scratch.data, g, beta,
                                                      stream);
        } else {
            bf16_gdn_gating_proj_mma_split8_launch(plan.token_variant, x, a_weight, b_weight, A_log,
                                                   dt_bias, scratch.data, g, beta, stream);
        }
        return;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
        if (is_35(problem)) {
            bf16_gdn_gating_proj_35_mma_split4_launch(plan.token_variant, x, a_weight, b_weight,
                                                      A_log, dt_bias, scratch.data, g, beta,
                                                      stream);
        } else {
            bf16_gdn_gating_proj_mma_split4_launch(plan.token_variant, x, a_weight, b_weight, A_log,
                                                   dt_bias, scratch.data, g, beta, stream);
        }
        return;
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit2:
        if (is_35(problem)) {
            bf16_gdn_gating_proj_35_mma_split2_launch(plan.token_variant, x, a_weight, b_weight,
                                                      A_log, dt_bias, scratch.data, g, beta,
                                                      stream);
        } else {
            bf16_gdn_gating_proj_mma_split2_launch(plan.token_variant, x, a_weight, b_weight, A_log,
                                                   dt_bias, scratch.data, g, beta, stream);
        }
        return;
    case Bf16GdnGatingScheduleId::MmaUnsplit:
        if (is_35(problem)) {
            bf16_gdn_gating_proj_35_mma_unsplit_launch(plan.token_variant, x, a_weight, b_weight,
                                                       A_log, dt_bias, g, beta, stream);
        } else {
            bf16_gdn_gating_proj_mma_unsplit_launch(plan.token_variant, x, a_weight, b_weight,
                                                    A_log, dt_bias, g, beta, stream);
        }
        return;
    }
    throw std::logic_error("BF16 GDN gating: unknown schedule");
}

template <std::size_t N>
std::size_t route_capacity(const std::array<RouteSpec, N>& routes, const Bf16GdnGatingProblem& base,
                           std::int32_t min_cols, std::int32_t max_cols) {
    // The workspace is split_k * cols * 2 * heads floats. Inside a route the resolved split is a
    // step function of the column count on this device - the preferred split until its
    // cooperative grid stops being resident, then each narrower one in turn - and within a step
    // the workspace grows with the column count. The interval's maximum therefore sits at a
    // step's right end: the route endpoint, or the last column count at which a split in the
    // chain still fits. Resolving at exactly those points sizes what this device executes, so
    // the query equals the execution high-water instead of bounding it from an architecture
    // the process is not running on.
    const CooperativeGeometry& geometry = cooperative_geometry(base);
    const auto resolved_bytes           = [&](std::int32_t cols) {
        return bf16_gdn_gating_resolve_plan({base.heads, base.input_rows, cols}).workspace_bytes;
    };
    std::size_t maximum = 0;
    for (const RouteSpec& route : routes) {
        const std::int32_t first = std::max(route.cols.first, min_cols);
        const std::int32_t last  = std::min(route.cols.last, max_cols);
        if (first > last) { continue; }
        maximum = std::max(maximum, resolved_bytes(last));
        for (Bf16GdnGatingScheduleId schedule = route.schedule; schedule_is_cooperative(schedule);
             schedule                         = narrower_split(schedule)) {
            const std::int32_t fit = cooperative_fit_cols(schedule, geometry);
            if (fit >= first && fit < last) { maximum = std::max(maximum, resolved_bytes(fit)); }
        }
    }
    return maximum;
}

} // namespace

const char* bf16_gdn_gating_schedule_name(Bf16GdnGatingScheduleId schedule) noexcept {
    switch (schedule) {
    case Bf16GdnGatingScheduleId::GemvPairedRows:
        return "gdn_gating_proj.bf16.gemv.paired_rows";
    case Bf16GdnGatingScheduleId::SmallTSplit10:
        return "gdn_gating_proj.bf16.small_t.split10";
    case Bf16GdnGatingScheduleId::SimtWarpRowC4:
        return "gdn_gating_proj.bf16.simt.warp_row.c4";
    case Bf16GdnGatingScheduleId::SimtWarpRowC8:
        return "gdn_gating_proj.bf16.simt.warp_row.c8";
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit32:
        return "gdn_gating_proj.bf16.mma.cooperative_split32";
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit16:
        return "gdn_gating_proj.bf16.mma.cooperative_split16";
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit8:
        return "gdn_gating_proj.bf16.mma.cooperative_split8";
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit4:
        return "gdn_gating_proj.bf16.mma.cooperative_split4";
    case Bf16GdnGatingScheduleId::MmaCooperativeSplit2:
        return "gdn_gating_proj.bf16.mma.cooperative_split2";
    case Bf16GdnGatingScheduleId::MmaUnsplit:
        return "gdn_gating_proj.bf16.mma.unsplit";
    }
    return "gdn_gating_proj.bf16.unknown";
}

const char* bf16_gdn_norm_gating_schedule_name(Bf16GdnNormGatingScheduleId schedule) noexcept {
    switch (schedule) {
    case Bf16GdnNormGatingScheduleId::Composed:
        return "gdn_norm_gating_proj.bf16.composed";
    case Bf16GdnNormGatingScheduleId::MmaCooperativeSplit32:
        return "gdn_norm_gating_proj.bf16.mma.cooperative_split32";
    }
    return "gdn_norm_gating_proj.bf16.unknown";
}

bool bf16_gdn_gating_admits(const Bf16GdnGatingProblem& problem) noexcept {
    if (problem.cols < 1) { return false; }
    return is_27(problem) || is_35(problem);
}

Bf16GdnGatingPlan bf16_gdn_gating_resolve_candidate(Bf16GdnGatingScheduleId schedule,
                                                    const Bf16GdnGatingProblem& problem) {
    if (!candidate_is_legal(schedule, problem)) {
        throw std::invalid_argument("BF16 GDN gating: candidate is not legal for exact problem");
    }
    const bool mma                          = schedule_uses_mma(schedule);
    const Bf16GdnGatingTokenVariant variant = !mma ? Bf16GdnGatingTokenVariant::None
                                                   : ((problem.cols % mma_tile_cols(problem)) == 0
                                                          ? Bf16GdnGatingTokenVariant::Full
                                                          : Bf16GdnGatingTokenVariant::Predicated);
    const std::int32_t split_k              = schedule_split_k(schedule);
    const std::size_t workspace =
        split_k > 1 ? checked_partial_bytes(problem.heads, split_k, problem.cols) : 0;
    return {schedule, variant, workspace};
}

Bf16GdnGatingPlan bf16_gdn_gating_resolve_plan(const Bf16GdnGatingProblem& problem) {
    if (!bf16_gdn_gating_admits(problem)) {
        throw std::invalid_argument(
            "BF16 GDN gating: exact problem or column count is not admitted");
    }
    const auto resolve_route = [&](Bf16GdnGatingScheduleId preferred) {
        Bf16GdnGatingScheduleId schedule = preferred;
        while (!candidate_is_legal(schedule, problem)) {
            if (schedule == Bf16GdnGatingScheduleId::MmaUnsplit) { break; }
            schedule = narrower_split(schedule);
        }
        return bf16_gdn_gating_resolve_candidate(schedule, problem);
    };
    if (is_27(problem)) {
        for (const RouteSpec& route : k27Routes) {
            if (route.cols.contains(problem.cols)) { return resolve_route(route.schedule); }
        }
    } else {
        for (const RouteSpec& route : k35Routes) {
            if (route.cols.contains(problem.cols)) { return resolve_route(route.schedule); }
        }
    }
    throw std::logic_error("BF16 GDN gating: admitted problem has no covering route");
}

std::size_t bf16_gdn_gating_capacity_workspace_bytes(std::int32_t heads, std::int32_t input_rows,
                                                     std::int32_t min_cols, std::int32_t max_cols) {
    if (min_cols <= 0 || max_cols < min_cols) {
        throw std::invalid_argument("BF16 GDN gating: invalid column interval");
    }
    const Bf16GdnGatingProblem base{heads, input_rows, 1};
    (void)bf16_gdn_gating_resolve_plan({heads, input_rows, min_cols});
    (void)bf16_gdn_gating_resolve_plan({heads, input_rows, max_cols});
    return is_27(base) ? route_capacity(k27Routes, base, min_cols, max_cols)
                       : route_capacity(k35Routes, base, min_cols, max_cols);
}

Bf16GdnNormGatingPlan bf16_gdn_norm_gating_resolve_plan(const Bf16GdnGatingProblem& problem) {
    Bf16GdnGatingPlan control            = bf16_gdn_gating_resolve_plan(problem);
    Bf16GdnNormGatingScheduleId schedule = Bf16GdnNormGatingScheduleId::Composed;
    std::int32_t norm_splits             = 0;
    if (is_35(problem) && problem.cols <= 16) {
        control  = bf16_gdn_gating_resolve_candidate(Bf16GdnGatingScheduleId::MmaCooperativeSplit32,
                                                     problem);
        schedule = Bf16GdnNormGatingScheduleId::MmaCooperativeSplit32;
        norm_splits = 32;
    }
    const std::size_t norm_partial_bytes =
        static_cast<std::size_t>(norm_splits) * problem.cols * sizeof(float);
    return {schedule, control, control.workspace_bytes + norm_partial_bytes};
}

std::size_t bf16_gdn_norm_gating_capacity_workspace_bytes(std::int32_t heads,
                                                          std::int32_t input_rows,
                                                          std::int32_t min_cols,
                                                          std::int32_t max_cols) {
    std::size_t maximum =
        bf16_gdn_gating_capacity_workspace_bytes(heads, input_rows, min_cols, max_cols);
    if (heads == 32 && input_rows == 2048 && min_cols <= 16) {
        const std::int32_t fused_cols = std::min<std::int32_t>(max_cols, 16);
        maximum                       = std::max(
            maximum,
            bf16_gdn_norm_gating_resolve_plan({heads, input_rows, fused_cols}).workspace_bytes);
    }
    return maximum;
}

void bf16_gdn_gating_execute_plan(const Bf16GdnGatingPlan& plan, const Tensor& x,
                                  const Weight& a_weight, const Weight& b_weight,
                                  const Tensor& A_log, const Tensor& dt_bias, WorkspaceArena& ws,
                                  Tensor& g, Tensor& beta, cudaStream_t stream) {
    const Bf16GdnGatingProblem problem{g.ne[0], x.ne[0], x.ne[1]};
    const Bf16GdnGatingPlan resolved = bf16_gdn_gating_resolve_plan(problem);
    if (resolved.schedule != plan.schedule || resolved.token_variant != plan.token_variant ||
        resolved.workspace_bytes != plan.workspace_bytes) {
        throw std::invalid_argument("BF16 GDN gating: plan does not match the exact problem");
    }
    execute_resolved(plan, problem, x, a_weight, b_weight, A_log, dt_bias, ws, g, beta, stream);
}

void bf16_gdn_gating_execute_candidate(Bf16GdnGatingScheduleId schedule, const Tensor& x,
                                       const Weight& a_weight, const Weight& b_weight,
                                       const Tensor& A_log, const Tensor& dt_bias,
                                       WorkspaceArena& ws, Tensor& g, Tensor& beta,
                                       cudaStream_t stream) {
    const Bf16GdnGatingProblem problem{g.ne[0], x.ne[0], x.ne[1]};
    const Bf16GdnGatingPlan plan = bf16_gdn_gating_resolve_candidate(schedule, problem);
    execute_resolved(plan, problem, x, a_weight, b_weight, A_log, dt_bias, ws, g, beta, stream);
}

void bf16_gdn_gating_dispatch(const Tensor& x, const Weight& a_weight, const Weight& b_weight,
                              const Tensor& A_log, const Tensor& dt_bias, WorkspaceArena& ws,
                              Tensor& g, Tensor& beta, cudaStream_t stream) {
    const Bf16GdnGatingPlan plan = bf16_gdn_gating_resolve_plan({g.ne[0], x.ne[0], x.ne[1]});
    bf16_gdn_gating_execute_plan(plan, x, a_weight, b_weight, A_log, dt_bias, ws, g, beta, stream);
}

void bf16_gdn_norm_gating_dispatch(const Tensor& x, const Tensor& norm_weight, float eps, Tensor& h,
                                   const Weight& a_weight, const Weight& b_weight,
                                   const Tensor& A_log, const Tensor& dt_bias, WorkspaceArena& ws,
                                   Tensor& g, Tensor& beta, cudaStream_t stream) {
    const Bf16GdnGatingProblem problem{g.ne[0], x.ne[0], x.ne[1]};
    const Bf16GdnNormGatingPlan plan = bf16_gdn_norm_gating_resolve_plan(problem);
    if (plan.schedule == Bf16GdnNormGatingScheduleId::Composed) {
        rmsnorm(x, norm_weight, eps, true, h, stream);
        execute_resolved(plan.control, problem, h, a_weight, b_weight, A_log, dt_bias, ws, g, beta,
                         stream);
        return;
    }

    auto scratch_scope = ws.scope();
    DeviceSpan scratch{};
    if (plan.workspace_bytes != 0) { scratch = ws.alloc_bytes(plan.workspace_bytes); }
    bf16_gdn_norm_gating_proj_35_mma_split32_launch(plan.control.token_variant, x, norm_weight, eps,
                                                    h, a_weight, b_weight, A_log, dt_bias,
                                                    scratch.data, g, beta, stream);
}

} // namespace ninfer::ops::detail
