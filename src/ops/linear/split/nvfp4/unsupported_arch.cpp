#include "ops/linear/split/nvfp4/shapes/n14336_k5120/launch.h"
#include "ops/linear/split/nvfp4/shapes/n16384_k5120/launch.h"
#include <stdexcept>

namespace ninfer::ops::detail::linear_split {
namespace nvfp4_n14336_k5120 {
void launch_a4_tma(const Nvfp4A4Operands&, std::span<const Tensor>, cudaStream_t) {
    throw std::runtime_error("NVFP4 A4 execution requires an sm_120a GPU");
}
} // namespace nvfp4_n14336_k5120
namespace nvfp4_n16384_k5120 {
void launch_a4_tma(const Nvfp4A4Operands&, std::span<const Tensor>, cudaStream_t) {
    throw std::runtime_error("NVFP4 A4 execution requires an sm_120a GPU");
}
} // namespace nvfp4_n16384_k5120
} // namespace ninfer::ops::detail::linear_split
