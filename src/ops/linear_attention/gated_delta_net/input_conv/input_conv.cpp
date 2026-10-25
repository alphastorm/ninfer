#include "ninfer/ops/gdn_input_proj.h"
#include "ops/linear_attention/gated_delta_net/input_conv/execute.h"
#include "ops/linear_attention/gated_delta_net/input_conv/workspace.h"
#include <array>
#include <stdexcept>

namespace ninfer::ops {
namespace {
namespace input = detail::gdn_input;

// Scratch is a scoped tail allocation. Live caller tensors may occupy an earlier part of
// the same arena. The zero-scratch path never constructs an empty borrowed MemoryArena.
template <class Validate, class Launch>
void execute_with_scratch(const input::WeightOperands& weights, input::ProjectionPlan plan,
                          std::int32_t columns, bool materialized_snapshot, MemoryArena& workspace,
                          Validate&& validate, Launch&& launch) {
    auto scope       = workspace.scope();
    const auto bytes = input::workspace_bytes(weights, plan, columns, materialized_snapshot);
    if (bytes == 0) {
        validate(MemorySpan{});
        launch(input::Scratch{});
        return;
    }
    const MemorySpan backing = workspace.alloc_bytes(bytes, 256);
    validate(backing);
    MemoryArena local(backing);
    launch(input::allocate_workspace(local, weights, plan, columns, materialized_snapshot));
}
} // namespace

void gdn_input_proj_conv_snapshot(const Tensor& x, const ProjectionWeights& weights,
                                  const Tensor& conv_weight, GdnConvSnapshotState state,
                                  const Tensor& valid, GdnConvOutputs out, MemoryArena& workspace,
                                  CudaExecutionView execution) {
    if (workspace.device() != execution.device()) {
        throw std::invalid_argument(
            "gdn_input_proj_conv_snapshot: workspace belongs to another device");
    }
    const auto stream   = execution.stream();
    const auto operands = input::describe_weights(weights);
    input::validate_weights(operands);
    const auto batch =
        input::validate_convolution(x, operands, conv_weight, state.history, state.source_slots,
                                    valid, out, input::Publication::Snapshot, state.snapshot_bases);
    const auto plan = input::select_conv_plan(operands, batch);
    const std::array<const Tensor*, 10> tensors{
        &x,     &conv_weight, &state.history, &state.source_slots, &state.snapshot_bases,
        &valid, &out.query,   &out.key,       &out.value,          &out.z};
    execute_with_scratch(
        operands, plan.projection, batch.columns(), plan.materialized(), workspace,
        [&](MemorySpan scratch) {
            input::validate_storage(operands, tensors, execution, scratch, &state.source_slots,
                                    &state.snapshot_bases);
        },
        [&](const input::Scratch& scratch) {
            input::launch_snapshot(x, operands, conv_weight, state, valid, out, batch, plan,
                                   scratch, stream);
        });
}

std::size_t gdn_input_proj_conv_snapshot_workspace_capacity_bytes(const ProjectionProfile& weights,
                                                                  std::int32_t batch,
                                                                  std::int32_t first,
                                                                  std::int32_t last) {
    return input::convolution_capacity(input::describe_profile(weights),
                                       input::Publication::Snapshot, batch, first, last);
}

void gdn_input_proj_conv_record(const Tensor& x, const ProjectionWeights& weights,
                                const Tensor& conv_weight, GdnConvSourceState state,
                                const Tensor& valid, GdnConvOutputs out, Tensor& record,
                                MemoryArena& workspace, CudaExecutionView execution) {
    if (workspace.device() != execution.device()) {
        throw std::invalid_argument(
            "gdn_input_proj_conv_record: workspace belongs to another device");
    }
    const auto stream   = execution.stream();
    const auto operands = input::describe_weights(weights);
    input::validate_weights(operands);
    const auto batch =
        input::validate_convolution(x, operands, conv_weight, state.history, state.source_slots,
                                    valid, out, input::Publication::Record, record);
    const auto plan = input::select_conv_plan(operands, batch);
    const std::array<const Tensor*, 10> tensors{
        &x,      &conv_weight, &state.history, &state.source_slots, &valid,
        &record, &out.query,   &out.key,       &out.value,          &out.z};
    execute_with_scratch(
        operands, plan.projection, batch.columns(), false, workspace,
        [&](MemorySpan scratch) { input::validate_storage(operands, tensors, execution, scratch); },
        [&](const input::Scratch& scratch) {
            input::launch_record(x, operands, conv_weight, state, valid, out, record, batch, plan,
                                 scratch, stream);
        });
}

std::size_t gdn_input_proj_conv_record_workspace_capacity_bytes(const ProjectionProfile& weights,
                                                                std::int32_t batch,
                                                                std::int32_t first,
                                                                std::int32_t last) {
    return input::convolution_capacity(input::describe_profile(weights), input::Publication::Record,
                                       batch, first, last);
}

} // namespace ninfer::ops
