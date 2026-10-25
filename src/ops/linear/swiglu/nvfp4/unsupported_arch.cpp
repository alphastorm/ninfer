#include "ops/linear/swiglu/nvfp4/tma_launch.h"
#include <stdexcept>

namespace ninfer::ops::detail {
void launch_nvfp4_swiglu_a4_tma(const Nvfp4A4Operands&, __nv_bfloat16*, cudaStream_t) {
    throw std::runtime_error("NVFP4 A4 execution requires an sm_120a GPU");
}
} // namespace ninfer::ops::detail
