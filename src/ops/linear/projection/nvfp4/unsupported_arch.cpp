#include "ops/linear/projection/nvfp4/nvfp4_a4_tma_launch.h"
#include <stdexcept>

namespace ninfer::ops::detail {
void launch_nvfp4_a4_tma_linear(Nvfp4GeometryId, const Nvfp4A4Operands&,
                                __nv_bfloat16*, cudaStream_t) {
    throw std::runtime_error("NVFP4 A4 execution requires an sm_120a GPU");
}
} // namespace ninfer::ops::detail
