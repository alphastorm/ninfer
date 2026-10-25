#include "ops/linear/kernels/fp8/fp8_a8_plan.h"
#include <stdexcept>

namespace ninfer::ops::detail {
void launch_fp8_a8_quantize(const Tensor&, const Weight&, Fp8A8Workspace, cudaStream_t) {
    throw std::runtime_error("FP8 A8 execution requires an sm_89 or newer GPU");
}
} // namespace ninfer::ops::detail
