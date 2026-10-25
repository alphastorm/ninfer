#include "ops/linear_attention/gated_delta_net/control/kernels/mma_launch.cuh"
#include "ops/linear_attention/gated_delta_net/control/kernels/gemv.cuh"

namespace ninfer::ops::detail::gdn_control {
template <int Tile, bool Paired>
void mma_n96_k2560(const Inputs& in, GdnControlOutputs out, Plan plan, Scratch scratch,
                   CudaExecutionView e) {
    using Geometry = MmaGeometry<48, 2560, Tile, Paired>;
    bool launched  = false;
    switch (plan.split_k) {
    case 8:
        launched = launch_mma<Geometry, 8>(in, out, scratch, e);
        break;
    case 4:
        if constexpr (Paired && Tile == 64) {
            launched = launch_mma<Geometry, 4>(in, out, scratch, e);
            break;
        } else
            throw std::logic_error("gdn_control: invalid split4");
    case 2:
        if constexpr (Paired && Tile == 64) {
            launched = launch_mma<Geometry, 2>(in, out, scratch, e);
            break;
        } else
            throw std::logic_error("gdn_control: invalid split2");
    case 1:
        launched = launch_mma<Geometry, 1>(in, out, {}, e);
        break;
    default:
        throw std::logic_error("gdn_control: invalid 96x2560 plan");
    }
    if (!launched) (void)launch_mma<Geometry, 1>(in, out, {}, e);
}

void launch_n96_k2560(const Inputs& in, GdnControlOutputs out, Plan plan, Scratch scratch,
                      CudaExecutionView e) {
    if (plan.kernel == Kernel::Gemv) {
        gemv_kernel<48, 2560><<<96, 256, 0, e.stream()>>>(
            static_cast<const __nv_bfloat16*>(in.x.data), static_cast<const __nv_bfloat16*>(in.a),
            static_cast<const __nv_bfloat16*>(in.b), static_cast<const float*>(in.a_log.data),
            static_cast<const float*>(in.dt_bias.data), static_cast<float*>(out.g.data),
            static_cast<float*>(out.beta.data));
        CUDA_CHECK(cudaGetLastError());
        return;
    }
    if (plan.kernel == Kernel::TiledGemv) {
        tiled_gemv_kernel<4, 256><<<dim3(96, (in.x.ne[1] + 3) / 4), 256, 0, e.stream()>>>(
            static_cast<const __nv_bfloat16*>(in.x.data), static_cast<const __nv_bfloat16*>(in.a),
            static_cast<const __nv_bfloat16*>(in.b), static_cast<const float*>(in.a_log.data),
            static_cast<const float*>(in.dt_bias.data), static_cast<float*>(out.g.data),
            static_cast<float*>(out.beta.data), in.x.ne[1]);
        CUDA_CHECK(cudaGetLastError());
        return;
    }
    if (plan.rows == RowMode::Merged)
        mma_n96_k2560<64, false>(in, out, plan, scratch, e);
    else if (plan.tile_cols == 64)
        mma_n96_k2560<64, true>(in, out, plan, scratch, e);
    else
        mma_n96_k2560<128, true>(in, out, plan, scratch, e);
}
} // namespace ninfer::ops::detail::gdn_control
