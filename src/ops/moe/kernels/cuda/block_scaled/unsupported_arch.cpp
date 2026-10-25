#include "ops/moe/kernels/cuda/block_scaled/fp8/launch.h"
#include "ops/moe/kernels/cuda/block_scaled/nvfp4/launch.h"
#include <stdexcept>

namespace ninfer::ops::detail::moe_cuda::block_scaled {
namespace fp8 {
void launch_input(const Tensor&, const CudaSwiGLUExperts&, const Storage&, CudaExecutionView, bool) {
    throw std::runtime_error("Block-scaled MoE execution requires an sm_120a GPU");
}
void launch_experts(const Tensor&, const CudaSwiGLUExperts&, const ExpertRoutes&, const Plan&,
                    const Storage&, CudaExecutionView) {
    throw std::runtime_error("Block-scaled MoE execution requires an sm_120a GPU");
}
void launch_vector(const Tensor&, const CudaSwiGLUExperts&, const ExpertRoutes&, ExpertSubset,
                   bool, MoeEpilogue, Tensor&, const Storage&, CudaExecutionView) {
    throw std::runtime_error("Block-scaled MoE execution requires an sm_120a GPU");
}
} // namespace fp8
namespace nvfp4 {
void launch_input(const Tensor&, const CudaSwiGLUExperts&, const Storage&, CudaExecutionView, bool) {
    throw std::runtime_error("Block-scaled MoE execution requires an sm_120a GPU");
}
void launch_experts(const Tensor&, const CudaSwiGLUExperts&, const ExpertRoutes&, const Plan&,
                    const Storage&, CudaExecutionView) {
    throw std::runtime_error("Block-scaled MoE execution requires an sm_120a GPU");
}
void launch_vector(const Tensor&, const CudaSwiGLUExperts&, const ExpertRoutes&, ExpertSubset,
                   bool, MoeEpilogue, Tensor&, const Storage&, CudaExecutionView) {
    throw std::runtime_error("Block-scaled MoE execution requires an sm_120a GPU");
}
} // namespace nvfp4
} // namespace ninfer::ops::detail::moe_cuda::block_scaled
