#include "ops/linear/split/q4_q5/shapes/n4096_n12288_k5120/launch.h"

namespace ninfer::ops::detail::linear_split::q4_q5_n4096_n12288_k5120 {
namespace {
void launch(const Tensor& x, Weights weights, std::span<const Tensor> outputs, MemoryArena*,
            cudaStream_t stream) {
    bool small = x.ne[1] <= 12;
#if defined(NINFER_SM86) || defined(NINFER_SM89)
    small = x.ne[1] <= 32;
#endif
#if !defined(NINFER_SM86) && !defined(NINFER_SM89)
    small = small || x.ne[1] == 16;
#endif
    if (small)
        launch_small(x, weights.first, *weights.second, outputs, stream);
    else
        launch_matrix(x, weights.first, *weights.second, outputs, stream);
}
} // namespace
} // namespace ninfer::ops::detail::linear_split::q4_q5_n4096_n12288_k5120

namespace ninfer::ops::detail::linear_split {
const SplitShape kQ4Q5SplitN4096N12288K5120{
    4096, 5120, kSplitRows<10240, 6144>, q4_q5_n4096_n12288_k5120::launch, nullptr, 12288};
} // namespace ninfer::ops::detail::linear_split
