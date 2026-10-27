#include "ops/linear/split/q4_q5/shapes/n7168_n7168_k5120/launch.h"
#include "ops/linear/kernels/group64_a8/launch.h"

namespace ninfer::ops::detail::linear_split::q4_q5_n7168_n7168_k5120 {
namespace {
void launch(const Tensor& x, Weights weights, std::span<const Tensor> outputs, MemoryArena* workspace,
            cudaStream_t stream) {
#if defined(NINFER_NATIVE_PREFILL_GROUP64_A8)
    if (group64_a8::selected(weights.policy, x.ne[1])) {
        const std::array first{
            group64_a8::Destination{outputs[0].data, 6144, 0, 0, 6144},
            group64_a8::Destination{outputs[1].data, 1024, 0, 6144, 1024}};
        const std::array second{
            group64_a8::Destination{outputs[2].data, 6144, 0, 0, 6144},
            group64_a8::Destination{outputs[3].data, 1024, 0, 6144, 1024}};
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
} // namespace ninfer::ops::detail::linear_split::q4_q5_n7168_n7168_k5120

namespace ninfer::ops::detail::linear_split {
const SplitShape kQ4Q5SplitN7168N7168K5120{
    7168, 5120, kSplitRows<6144, 1024, 6144, 1024>, q4_q5_n7168_n7168_k5120::launch,
    q4_q5_n7168_n7168_k5120::capacity, 7168};
} // namespace ninfer::ops::detail::linear_split
