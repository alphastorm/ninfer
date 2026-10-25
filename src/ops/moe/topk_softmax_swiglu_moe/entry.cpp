#include "core/nvtx.h"
#include "ninfer/ops/moe.h"
#include "ops/moe/operands.h"
#include "ops/moe/topk_softmax_swiglu_moe/dispatch.h"
#include "ops/moe/workspace.h"
#include "ops/moe/kernels/cuda/groupwise_int/workspace.h"
#include "ops/moe/kernels/cuda/block_scaled/workspace.h"

namespace ninfer::ops {
namespace {
namespace kernel       = detail::moe_cuda;
namespace integer      = kernel::groupwise_int;
namespace block_scaled = kernel::block_scaled;
namespace routing      = kernel::routing;
namespace impl         = detail::moe_complete;

struct CallStorage {
    ExpertRoutes routes;
    Tensor shared;
    routing::RouteStorage route;
    integer::ExpertStorage experts;
    block_scaled::Storage block_scaled;
};

template <class Arena>
CallStorage allocate(Arena& arena, const impl::Plan& plan, int tokens, int experts, int top_k,
                     bool split_activation) {
    CallStorage out;
    out.routes = detail::allocate_routes(arena, top_k, tokens);
    out.shared = arena.alloc(DType::FP32, {tokens});
    // Router scratch is independent of expert activations, including during PDL overlap.
    out.route = routing::allocate_route(arena, plan.router, tokens, experts);
    if (plan.implementation == kernel::ExpertImplementation::BlockScaled) {
        out.block_scaled = block_scaled::allocate(arena, plan.block_scaled, tokens, true, true);
        return out;
    }
    out.experts =
        plan.integer.grouped
            ? integer::allocate_grouped(arena, tokens, plan.integer.group, true, split_activation)
            : integer::allocate_token(arena, tokens, true);
    if (plan.router.count_assignments) {
        out.route.local_ranks = out.experts.groups.local_ranks;
        out.route.tile_counts = out.experts.groups.tile_counts;
    }
    return out;
}
} // namespace

std::size_t topk_softmax_swiglu_moe_workspace_capacity_bytes(const MoeProfile& profile,
                                                             MoeEpilogue mode, int first,
                                                             int last) {
    const auto& shape = impl::shape_dispatch(profile);
    detail::validate_output(DType::BF16, mode);
    detail::validate_interval(first, last);
    return detail::interval_capacity(
        first, last, shape.allocation_boundaries, impl::kSliceTokens, [&](int tokens) {
            WorkspaceLayoutBuilder layout;
            (void)allocate(layout, shape.select(tokens), tokens, profile.routed.experts,
                           profile.routed.top_k,
                           profile.routed.ffn.gate_up_type == QType::Q8_G32_FP16);
            return layout.peak_bytes(1);
        });
}

void topk_softmax_swiglu_moe(const Tensor& x, const MoeProfile& profile, const MoeWeights& weights,
                             MoeEpilogue mode, Tensor& destination, MemoryArena& workspace,
                             CudaExecutionView execution, const MoeExecutionHints& hints) {
    const auto& shape = impl::shape_dispatch(profile);
    if (destination.dtype != DType::BF16) {
        throw std::invalid_argument("complete MoE requires BF16 destination");
    }
    detail::MoeOperands operands("topk_softmax_swiglu_moe", execution);
    const auto& p    = profile.routed;
    const int tokens = operands.input(x, p.ffn.hidden);
    operands.output(destination, p.ffn.hidden, tokens, mode);
    operands.control(weights.control, {p.ffn.hidden, p.experts, p.top_k});
    operands.experts(weights.routed, p);
    operands.shared(weights.shared, p.ffn.hidden);
    operands.workspace(
        workspace, topk_softmax_swiglu_moe_workspace_capacity_bytes(profile, mode, tokens, tokens));
    operands.finish();
    nvtx::ScopedRange range(nvtx::Name::TopkSoftmaxSwiGLUMoe, nvtx::Category::Moe, tokens);
    for (std::int64_t start = 0; start < tokens; start += impl::kSliceTokens) {
        const int offset   = static_cast<int>(start);
        const int count    = std::min(tokens - offset, impl::kSliceTokens);
        const auto plan    = shape.select(count);
        const auto input   = x.slice(1, offset, count);
        auto output        = destination.slice(1, offset, count);
        auto scope         = workspace.scope();
        const auto storage = allocate(workspace, plan, count, p.experts, p.top_k,
                                      p.ffn.gate_up_type == QType::Q8_G32_FP16);
        const ScaledSwiGLUTerm shared{weights.shared, storage.shared};
        if (plan.implementation == kernel::ExpertImplementation::BlockScaled) {
            block_scaled::launch_control_shared(input, weights, storage.routes, storage.shared,
                                                storage.route.scores, storage.block_scaled,
                                                plan.block_scaled, execution);
            block_scaled::launch(input, weights.routed, storage.routes, {}, &shared, mode, output,
                                 plan.block_scaled, storage.block_scaled, execution, true);
            continue;
        }
        const MoeExecutionHints shared_prefetch{weights.shared.down.weight.qdata,
                                                std::size_t(p.ffn.hidden) *
                                                    profile.shared.intermediate};
        const MoeExecutionHints next_prefetch{
            hints.read_ahead, std::min(hints.read_ahead_bytes, std::size_t{8} << 20)};
        routing::launch_route(input, weights.control, storage.routes, storage.shared, plan.router,
                              storage.route, count == 1 ? shared_prefetch : MoeExecutionHints{},
                              execution);
        if (plan.integer.grouped) {
            if (!plan.router.count_assignments) {
                integer::launch_group_count(storage.routes, {}, storage.experts.groups, execution);
            }
            integer::launch_group_scan(storage.routes, plan.integer.group, storage.experts.groups,
                                       execution);
            if (plan.integer.group.adaptive) {
                integer::launch_token_swiglu(input, p, weights.routed, storage.routes, {}, &shared,
                                             mode, output, plan.integer.token, storage.experts, {},
                                             execution, true);
            }
            integer::launch_group_index(storage.routes, storage.experts.groups, execution);
            integer::launch_grouped_swiglu(input, p, weights.routed, storage.routes, &shared, mode,
                                           output, plan.integer.group, storage.experts, execution);
        } else {
            integer::launch_token_swiglu(input, p, weights.routed, storage.routes, {}, &shared,
                                         mode, output, plan.integer.token, storage.experts,
                                         count == 1 ? next_prefetch : MoeExecutionHints{},
                                         execution);
        }
    }
}
} // namespace ninfer::ops
