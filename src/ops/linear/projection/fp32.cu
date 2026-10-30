#include "ops/linear/projection/fp32.h"
#include "ops/linear/common/output.cuh"
#include "ops/linear/kernels/bf16/bf16_template_launch.cuh"
#include "ops/linear/kernels/fp8/fp8_template_launch.cuh"
#include "ops/linear/kernels/nvfp4/nvfp4_template_launch.cuh"
#include "ops/linear/kernels/q4/q4_instance_launch.cuh"
#include "ops/linear/kernels/q5/q5_instance_launch.cuh"
#include "ops/linear/kernels/q6/q6_instances.cuh"
#include "ops/linear/kernels/q6/q6_simt_launch.cuh"
#include "ops/linear/kernels/q6/q6_sliced_k_launch.cuh"
#include "ops/linear/kernels/q6/q6_mma_launch.cuh"
#include "ops/linear/kernels/q8/q8_output_launch.cuh"
#include <stdexcept>

namespace ninfer::ops::detail {
namespace {

void q4_fp32(const Tensor& x, const Weight& w, LinearFp32Output out, cudaStream_t stream) {
    const auto p = q4_linear_operands(x, w);
    if (x.ne[1] == 1)
        return launch_q4_a16_gemv<q4_instances::GemvR4W1>(p, out, LinearIdentityEpilogue{}, stream);
    if (x.ne[1] <= 8)
        return launch_q4_a16_sliced_k_mma<q4_instances::SlicedR16T8W4S2>(
            p, out, LinearIdentityEpilogue{}, stream);
    if (x.ne[1] <= 16)
        return launch_q4_a16_sliced_k_mma<q4_instances::SlicedR16T16W4S2>(
            p, out, LinearIdentityEpilogue{}, stream);
    if (x.ne[1] <= 32)
        return launch_q4_a16_sliced_k_mma<q4_instances::SlicedR32T32W4S1>(
            p, out, LinearIdentityEpilogue{}, stream);
    launch_q4_a16_mma<q4_instances::MmaR64T128K64Wr64Wt32S2A2B1>(
        p, out, LinearIdentityEpilogue{}, stream);
}

void q5_fp32(const Tensor& x, const Weight& w, LinearFp32Output out, cudaStream_t stream) {
    const auto p = q5_linear_operands(x, w);
    if (x.ne[1] <= 4)
        return launch_q5_a16_direct_simt<q5_instances::DirectR2T4W2G8B4>(
            p, out, LinearIdentityEpilogue{}, stream);
    if (x.ne[1] <= 8)
        return launch_q5_a16_sliced_k_mma<q5_instances::SlicedR16T8W4S2>(
            p, out, LinearIdentityEpilogue{}, stream);
    if (x.ne[1] <= 16)
        return launch_q5_a16_sliced_k_mma<q5_instances::SlicedR16T16W4S2>(
            p, out, LinearIdentityEpilogue{}, stream);
    if (x.ne[1] <= 32)
        return launch_q5_a16_sliced_k_mma<q5_instances::SlicedR32T32W4S1>(
            p, out, LinearIdentityEpilogue{}, stream);
    launch_q5_a16_mma<q5_instances::MmaR64T128>(p, out, LinearIdentityEpilogue{}, stream);
}

void q6_fp32(const Tensor& x, const Weight& w, LinearFp32Output out, cudaStream_t stream) {
    const auto p = q6_linear_operands(x, w);
    // Retain the vocabulary head's existing small-T reduction schedules, changing only the store.
    if (x.ne[1] <= 2)
        return launch_q6_a16_simt<q6_instances::SimtR8T4>(p, out, LinearIdentityEpilogue{}, stream);
    if (x.ne[1] <= 8)
        return launch_q6_a16_sliced_k_mma<q6_instances::SlicedR16T8W4S2>(
            p, out, LinearIdentityEpilogue{}, stream);
    if (x.ne[1] <= 16)
        return launch_q6_a16_sliced_k_mma<q6_instances::SlicedR32T16W4S2>(
            p, out, LinearIdentityEpilogue{}, stream);
    if (x.ne[1] <= 32)
        return launch_q6_a16_sliced_k_mma<q6_instances::SlicedR32T32W4S1>(
            p, out, LinearIdentityEpilogue{}, stream);
    launch_q6_a16_mma<q6_instances::MmaR64T128>(p, out, LinearIdentityEpilogue{}, stream);
}

template <int Tokens>
void q8_tile(const Tensor& x, const Weight& w, LinearFp32Output out, cudaStream_t stream) {
    using S = Q8A16SlicedKMmaSchedule<Tokens, 8, 1, 2, Q8ScaleAccess::Shared>;
    launch_q8_a16_sliced_k_mma<S>(q8_linear_operands(x, w), out, LinearIdentityEpilogue{}, stream);
}

void q8_fp32(const Tensor& x, const Weight& w, LinearFp32Output out, cudaStream_t stream) {
    if (x.ne[1] <= 8) return q8_tile<8>(x, w, out, stream);
    if (x.ne[1] <= 16) return q8_tile<16>(x, w, out, stream);
    if (x.ne[1] <= 24) return q8_tile<24>(x, w, out, stream);
    if (x.ne[1] <= 32) return q8_tile<32>(x, w, out, stream);
    using S = Q8A16MmaSchedule<64, 128, 64, 64, 32, 2, 1>;
    launch_q8_a16_mma<S>(q8_linear_operands(x, w), out, LinearIdentityEpilogue{}, stream);
}

} // namespace

void linear_fp32(const Tensor& x, const Weight& w, Tensor& out, cudaStream_t stream) {
    const LinearFp32Output output{static_cast<float*>(out.data), w.n};
    switch (w.qtype) {
    case QType::Q4_G64_FP16: return q4_fp32(x, w, output, stream);
    case QType::Q5_G64_FP16: return q5_fp32(x, w, output, stream);
    case QType::Q6_G64_FP16: return q6_fp32(x, w, output, stream);
    case QType::Q8_G32_FP16: return q8_fp32(x, w, output, stream);
    case QType::BF16: {
        const auto p = bf16_a16_operands(x, w);
        if (x.ne[1] <= 8)
            return launch_bf16_a16_sliced_k_mma<Bf16A16SlicedKMmaSchedule<16, 8, 4>>(
                p, output, LinearIdentityEpilogue{}, stream);
        using S = Bf16A16MmaSchedule<64, 64, 64, 32, 32, 2, 1, Cache::cg, Cache::cg,
                                     Bf16MmaFragmentPipeline::Serial, Bf16MmaRaster::TokenFast>;
        return launch_bf16_a16_mma<S>(p, output, LinearIdentityEpilogue{}, stream);
    }
    case QType::FP8_E4M3FN_ROW_BF16: {
        const auto p = fp8_a16_operands(x, w);
        if (x.ne[1] <= 8)
            return launch_fp8_a16_sliced_k_mma<Fp8A16SlicedKMmaSchedule<16, 8, 1>>(
                p, output, LinearIdentityEpilogue{}, stream);
        using S = Fp8A16MmaSchedule<64, 64, 64, 32, 32, 2, 1>;
        return launch_fp8_a16_mma<S>(p, output, LinearIdentityEpilogue{}, stream);
    }
    case QType::NVFP4: {
        const auto p = nvfp4_a16_operands(x, w);
        if (x.ne[1] <= 8)
            return launch_nvfp4_a16_sliced_k_mma<Nvfp4A16SlicedKMmaSchedule<16, 8, 1>>(
                p, output, LinearIdentityEpilogue{}, stream);
        using S = Nvfp4A16MmaSchedule<64, 64, 64, 32, 32, 2, 1>;
        return launch_nvfp4_a16_mma<S>(p, output, LinearIdentityEpilogue{}, stream);
    }
    default: throw std::invalid_argument("linear: weight format cannot write FP32");
    }
}

} // namespace ninfer::ops::detail
