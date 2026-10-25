#include "ninfer/ops/softmax_attention.h"

#include "core/layout.h"
#include "ops/softmax_attention/kernels/noncausal/operands.h"
#include "ops/softmax_attention/dense/packed/launch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace ninfer::ops {
namespace {

std::int32_t scratch_tiles(std::int32_t tokens, std::int32_t segments) {
    if (segments == 1) { return 0; }
    constexpr std::int64_t tile_rows = detail::kPackedDescriptorRows;
    const std::int64_t tiles =
        (static_cast<std::int64_t>(tokens) + tile_rows - 1) / tile_rows + segments - 1LL;
    if (tiles > std::numeric_limits<std::int32_t>::max()) {
        throw std::overflow_error("packed_softmax_attention: descriptor tile count exceeds int32");
    }
    return static_cast<std::int32_t>(tiles);
}

template <class Allocator>
Tensor allocate_workspace(Allocator& allocator, std::int32_t tokens, std::int32_t segments) {
    const std::int32_t tiles = scratch_tiles(tokens, segments);
    return tiles == 0 ? Tensor{} : allocator.alloc(DType::I32, {4, tiles});
}

} // namespace

std::size_t packed_softmax_attention_workspace_capacity_bytes(AttentionHeadGeometry geometry,
                                                              std::int32_t min_tokens,
                                                              std::int32_t max_tokens,
                                                              std::int32_t min_segments,
                                                              std::int32_t max_segments) {
    detail::noncausal::require_profile(geometry, detail::noncausal::kExpectedScale,
                                       "packed_softmax_attention workspace");
    if (min_tokens <= 0 || max_tokens < min_tokens || min_segments <= 0 ||
        max_segments < min_segments || min_segments > max_tokens) {
        throw std::invalid_argument(
            "packed_softmax_attention workspace: invalid execution envelope");
    }
    const std::int32_t segments = std::min(max_segments, max_tokens);
    WorkspaceLayoutBuilder layout;
    (void)allocate_workspace(layout, max_tokens, segments);
    return layout.peak_bytes(1);
}

void packed_softmax_attention(const Tensor& q, const Tensor& k, const Tensor& v,
                              AttentionHeadGeometry geometry, float scale, const Tensor& cu_seqlens,
                              MemoryArena& workspace, Tensor& out, CudaExecutionView execution) {
    const auto stream         = execution.stream();
    const std::int32_t tokens = detail::noncausal::validate_qkv(
        q, k, v, out, geometry, scale, "packed_softmax_attention", execution.device());
    common::operand::device(cu_seqlens.device, execution.device(), "packed attention", "segments");
    common::operand::device(workspace.device(), execution.device(), "packed attention",
                            "workspace");
    const std::int32_t segments = cu_seqlens.ne[0] - 1;
    if (cu_seqlens.dtype != DType::I32 || segments <= 0 || segments > tokens ||
        cu_seqlens.ne[1] != 1 || cu_seqlens.ne[2] != 1 || cu_seqlens.ne[3] != 1 ||
        !cu_seqlens.is_contiguous() || cu_seqlens.data == nullptr) {
        throw std::invalid_argument(
            "packed_softmax_attention: cu_seqlens must be contiguous I32 [S+1]");
    }
    auto scratch_scope = workspace.scope();
    Tensor tiles       = allocate_workspace(workspace, tokens, segments);
    Tensor* tiles_ptr  = tiles.data == nullptr ? nullptr : &tiles;
    detail::launch_packed_attention(q, k, v, cu_seqlens, tiles_ptr, out, stream);
}

void packed_softmax_attention(const Tensor& q, const Tensor& k, const Tensor& v,
                              AttentionHeadGeometry geometry, float scale,
                              std::int32_t segment_length, Tensor& out,
                              CudaExecutionView execution) {
    const auto stream         = execution.stream();
    const std::int32_t tokens = detail::noncausal::validate_qkv(
        q, k, v, out, geometry, scale, "packed_softmax_attention", execution.device());
    if (segment_length <= 0 || tokens % segment_length != 0) {
        throw std::invalid_argument("packed_softmax_attention: invalid uniform segment length");
    }
    detail::launch_packed_attention_uniform(q, k, v, segment_length, out, stream);
}

} // namespace ninfer::ops
