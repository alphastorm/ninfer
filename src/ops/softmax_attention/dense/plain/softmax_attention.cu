#include "ninfer/ops/softmax_attention.h"
#include "ops/softmax_attention/kernels/noncausal/operands.h"
#include "ops/softmax_attention/kernels/noncausal/kernel.cuh"

namespace ninfer::ops {
namespace {

std::int32_t select_tile(std::int32_t segment_length) {
    // Measured issued-MMA rates are approximately 3:6:8 for the 16/32/64
    // tiles. Minimize padded tile area divided by that rate.
    constexpr std::int32_t tiles[] = {16, 32, 64};
    constexpr std::int32_t rates[] = {3, 6, 8};
    std::int32_t best_tile         = tiles[0];
    std::int64_t best_padded       = ((segment_length + best_tile - 1) / best_tile) * best_tile;
    std::int64_t best_square       = best_padded * best_padded;
    std::int32_t best_rate         = rates[0];
    for (int i = 1; i < 3; ++i) {
        const std::int64_t padded = ((segment_length + tiles[i] - 1) / tiles[i]) * tiles[i];
        const std::int64_t square = padded * padded;
        if (square * best_rate < best_square * rates[i]) {
            best_tile   = tiles[i];
            best_square = square;
            best_rate   = rates[i];
        }
    }
    return best_tile;
}

} // namespace

void softmax_attention(const Tensor& q, const Tensor& k, const Tensor& v,
                       AttentionHeadGeometry geometry, float scale, MemoryArena& workspace,
                       Tensor& out, CudaExecutionView execution) {
    const auto stream         = execution.stream();
    const std::int32_t tokens = detail::noncausal::validate_qkv(
        q, k, v, out, geometry, scale, "softmax_attention", execution.device());
    common::operand::device(workspace.device(), execution.device(), "attention", "workspace");
    auto scope            = workspace.scope();
    const int tile        = select_tile(tokens);
    const int query_tiles = (tokens + tile - 1) / tile;
    switch (tile) {
    case 16:
        return detail::launch_flash<16, 16>(q, k, v, nullptr, tokens, query_tiles, out, stream);
    case 32:
        return detail::launch_flash<32, 32>(q, k, v, nullptr, tokens, query_tiles, out, stream);
    case 64:
        return detail::launch_flash<64, 64>(q, k, v, nullptr, tokens, query_tiles, out, stream);
    }
}

} // namespace ninfer::ops
