#include "ops/linear/kernels/nvfp4/nvfp4_a4_plan.h"
#include <stdexcept>

namespace ninfer::ops::detail {
void launch_nvfp4_a4_quantize(const Tensor&, const Weight&, Nvfp4A4Workspace, Nvfp4ScaleLayout,
                              cudaStream_t) {
    throw std::runtime_error("NVFP4 A4 execution requires an sm_120a GPU");
}
} // namespace ninfer::ops::detail
