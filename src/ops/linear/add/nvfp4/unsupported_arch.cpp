#include "ops/linear/add/nvfp4/dispatch.h"
#include <stdexcept>

namespace ninfer::ops::detail {
void launch_nvfp4_add_n5120_k6144_tma(const Tensor&, const Weight&, Tensor&, Nvfp4A4Workspace,
                                      cudaStream_t) {
    throw std::runtime_error("NVFP4 A4 execution requires an sm_120a GPU");
}
void launch_nvfp4_add_n5120_k17408_tma(const Tensor&, const Weight&, Tensor&, Nvfp4A4Workspace,
                                       cudaStream_t) {
    throw std::runtime_error("NVFP4 A4 execution requires an sm_120a GPU");
}
} // namespace ninfer::ops::detail
