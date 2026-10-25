#include "models/qwen3_5/execution/mtp.h"

#include "core/layout.h"
#include "ninfer/ops/linear_split.h"
#include "ninfer/ops/linear_pair.h"
#include "ninfer/ops/indexing.h"

#include <algorithm>

namespace ninfer::models::qwen3_5::execution {

std::size_t mtp_projection_workspace_bytes(const MtpProjectionProfile& parameters,
                                           std::int32_t first, std::int32_t last) {
    const auto& p = parameters.packed;
    if (!parameters.rows) {
        return ops::linear_split_workspace_capacity_bytes(ops::ProjectionProfile{p, {}},
                                                          parameters.output_rows, first, last);
    }
    WorkspaceLayoutBuilder layout;
    (void)layout.alloc(DType::BF16, {p.n, last});
    (void)layout.alloc_bytes(
        ops::linear_workspace_capacity_bytes(p.qtype, p.n, p.k, p.policy, first, last));
    return layout.peak_bytes(1);
}

std::size_t mtp_kv_workspace_bytes(const MtpProjectionProfile& parameters,
                                   const AttentionConfig& config, std::int32_t first,
                                   std::int32_t last) {
    if (parameters.rows) {
        return ops::linear_pair_workspace_capacity_bytes((*parameters.rows)[1],
                                                         (*parameters.rows)[3], first, last);
    }
    WorkspaceLayoutBuilder layout;
    (void)layout.alloc(DType::BF16, {dimension(config.query_width()), last});
    (void)layout.alloc(DType::BF16, {dimension(config.query_width()), last});
    (void)layout.alloc_bytes(mtp_projection_workspace_bytes(parameters, first, last));
    return layout.peak_bytes(1);
}

std::size_t mtp_query_gate_workspace_bytes(const MtpProjectionProfile& parameters,
                                           const AttentionConfig& config, std::int32_t first,
                                           std::int32_t last) {
    if (parameters.rows) {
        const auto bytes = [&](std::size_t index) {
            const auto& p = (*parameters.rows)[index];
            return ops::linear_workspace_capacity_bytes(p.qtype, p.n, p.k, p.policy, first, last);
        };
        return std::max(bytes(0), bytes(2));
    }
    WorkspaceLayoutBuilder layout;
    (void)layout.alloc(DType::BF16, {dimension(config.key_width()), last});
    (void)layout.alloc(DType::BF16, {dimension(config.key_width()), last});
    (void)layout.alloc_bytes(mtp_projection_workspace_bytes(parameters, first, last));
    return layout.peak_bytes(1);
}

void mtp_projection(const Tensor& hidden, const MtpProjectionParameters& parameters,
                    const AttentionConfig& config, Tensor& query, Tensor& gate, Tensor& key,
                    Tensor& value, MemoryArena& workspace, CudaExecutionView execution) {
    const auto& p = parameters.packed;
    if (!parameters.rows) {
        std::array outputs{query, key, gate, value};
        ops::linear_split(hidden, p, outputs, workspace, execution);
        return;
    }
    auto scope         = workspace.scope();
    const auto columns = hidden.ne[1];
    Tensor packed      = workspace.alloc(DType::BF16, {p.weight.n, columns});
    ops::linear(hidden, p.weight, packed, p.policy, workspace, execution);
    Tensor q =
        query.view({dimension(config.head_dim), dimension(config.num_attention_heads), columns});
    Tensor k =
        key.view({dimension(config.head_dim), dimension(config.num_key_value_heads), columns});
    Tensor g =
        gate.view({dimension(config.head_dim), dimension(config.num_attention_heads), columns});
    Tensor v =
        value.view({dimension(config.head_dim), dimension(config.num_key_value_heads), columns});
    ops::split_bf16_features(packed, q, k, g, v, execution);
}

void mtp_kv_projection(const Tensor& hidden, const MtpProjectionParameters& parameters,
                       const AttentionConfig& config, Tensor& key, Tensor& value,
                       MemoryArena& workspace, CudaExecutionView execution) {
    if (parameters.rows) {
        ops::linear_pair(hidden, (*parameters.rows)[1].weight, (*parameters.rows)[3].weight, key,
                         value, execution);
        return;
    }
    auto scope   = workspace.scope();
    Tensor query = workspace.alloc(DType::BF16, {dimension(config.query_width()), hidden.ne[1]});
    Tensor gate  = workspace.alloc(DType::BF16, {dimension(config.query_width()), hidden.ne[1]});
    mtp_projection(hidden, parameters, config, query, gate, key, value, workspace, execution);
}

void mtp_query_gate_projection(const Tensor& hidden, const MtpProjectionParameters& parameters,
                               const AttentionConfig& config, Tensor& query, Tensor& gate,
                               MemoryArena& workspace, CudaExecutionView execution) {
    if (parameters.rows) {
        const auto& q = (*parameters.rows)[0];
        const auto& g = (*parameters.rows)[2];
        {
            auto scope = workspace.scope();
            ops::linear(hidden, q.weight, query, q.policy, workspace, execution);
        }
        ops::linear(hidden, g.weight, gate, g.policy, workspace, execution);
        return;
    }
    auto scope   = workspace.scope();
    Tensor key   = workspace.alloc(DType::BF16, {dimension(config.key_width()), hidden.ne[1]});
    Tensor value = workspace.alloc(DType::BF16, {dimension(config.key_width()), hidden.ne[1]});
    mtp_projection(hidden, parameters, config, query, gate, key, value, workspace, execution);
}

} // namespace ninfer::models::qwen3_5::execution
