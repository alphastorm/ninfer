#include "ops/linear/split/q4_q5/shapes/n4096_n12288_k5120/launch.h"
#include "ops/linear/kernels/group64_a8/launch.h"

namespace ninfer::ops::detail::linear_split::q4_q5_n4096_n12288_k5120 {
namespace {
void launch(const Tensor& x, Weights weights, std::span<const Tensor> outputs, MemoryArena* workspace,
            cudaStream_t stream) {
#if defined(NINFER_NATIVE_PREFILL_GROUP64_A8)
    if (group64_a8::selected(weights.policy, x.ne[1])) {
        const std::array first{group64_a8::Destination{outputs[0].data, 10240, 0, 0, 4096}};
        const std::array second{
            group64_a8::Destination{outputs[0].data, 10240, 4096, 0, 6144},
            group64_a8::Destination{outputs[1].data, 6144, 0, 6144, 6144}};
        return group64_a8::split(x, weights.first, *weights.second, first, second, *workspace, stream);
    }
#endif
    bool small = x.ne[1] <= 12;
#if !defined(NINFER_SM86) && !defined(NINFER_SM89)
    small = small || x.ne[1] == 16;
#endif
    if (small)
        launch_small(x, weights.first, *weights.second, outputs, stream);
    else
        launch_matrix(x, weights.first, *weights.second, outputs, stream);
}
std::size_t capacity(LinearPolicy policy, int, int last) {
#if defined(NINFER_NATIVE_PREFILL_GROUP64_A8)
    if (group64_a8::selected(policy, last)) return group64_a8::workspace_bytes(5120, last);
#endif
    return 0;
}
} // namespace
} // namespace ninfer::ops::detail::linear_split::q4_q5_n4096_n12288_k5120

namespace ninfer::ops::detail::linear_split {
const SplitShape kQ4Q5SplitN4096N12288K5120{
    4096, 5120, kSplitRows<10240, 6144>, q4_q5_n4096_n12288_k5120::launch,
    q4_q5_n4096_n12288_k5120::capacity, 12288};
} // namespace ninfer::ops::detail::linear_split
