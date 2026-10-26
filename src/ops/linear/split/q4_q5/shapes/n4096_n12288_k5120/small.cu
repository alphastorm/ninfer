#include "ops/linear/kernels/q5/q5_instances.cuh"
#include "ops/linear/kernels/q4/q4_instances.cuh"
#include "core/weight.h"
#include "ops/linear/split/q4_q5/shapes/n4096_n12288_k5120/launch.h"

#include "core/device.h"
#include "core/pdl.cuh"
#include "ops/common/numeric/math.h"
#include "ops/linear/kernels/q4/q4_sliced_k_launch.cuh"
#include "ops/linear/kernels/q4/q4_simt_launch.cuh"
#include "ops/linear/kernels/q4/q4_gemv_launch.cuh"
#include "ops/linear/kernels/q5/q5_simt_launch.cuh"
#include "ops/linear/kernels/q5/q5_gemv_launch.cuh"
#include "ops/linear/kernels/q5/q5_sliced_k_launch.cuh"
#if defined(NINFER_SM86) || defined(NINFER_SM89)
#include "ops/linear/kernels/small_t/q45_small_t_mma.cuh"
#include "ops/linear/kernels/small_t/q5_a16_split2.cuh"
#endif

#include <cuda_bf16.h>

#include <cstdint>
#include <stdexcept>

namespace ninfer::ops::detail::linear_split::q4_q5_n4096_n12288_k5120 {
namespace {

constexpr std::int32_t kValueRows = 6144;
constexpr std::int32_t kHidden    = 5120;

#if defined(NINFER_SM86) || defined(NINFER_SM89)
template <class Output>
void launch_native_q4(const Q4LinearOperands& op, Output output, cudaStream_t stream) {
    const auto launch = [&]<int Columns, int KWarps, int Stages>() {
        launch_q45_small_t_mma<SmallTMmaSchedule<SmallTFormat::Q4, kHidden, Columns, KWarps, Stages>>(
            op, output, LinearIdentityEpilogue{}, stream);
    };
#if defined(NINFER_SM86)
    if (op.tokens <= 4) return launch.template operator()<4, 8, 2>();
    if (op.tokens <= 8) return launch.template operator()<8, 4, 2>();
    if (op.tokens <= 16) return launch.template operator()<16, 4, 2>();
    return launch.template operator()<32, 4, 1>();
#else
    if (op.tokens <= 8) return launch.template operator()<8, 8, 2>();
    if (op.tokens <= 16) return launch.template operator()<16, 8, 1>();
    return launch.template operator()<32, 8, 1>();
#endif
}

template <class Output>
void launch_native_q5(const Q5LinearOperands& op, Output output, cudaStream_t stream) {
    const auto launch = [&]<int Columns, int KWarps, int Stages>() {
        launch_q45_small_t_mma<SmallTMmaSchedule<SmallTFormat::Q5, kHidden, Columns, KWarps, Stages>>(
            op, output, LinearIdentityEpilogue{}, stream);
    };
    if (op.tokens == 1)
        return launch_q5_a16_split2<kHidden, 1>(op, output, LinearIdentityEpilogue{}, stream);
#if defined(NINFER_SM86)
    if (op.tokens == 2)
        return launch_q5_a16_split2<kHidden, 2>(op, output, LinearIdentityEpilogue{}, stream);
    if (op.tokens <= 4) return launch.template operator()<4, 8, 2>();
    if (op.tokens <= 8) return launch.template operator()<8, 8, 2>();
    if (op.tokens <= 13) return launch.template operator()<16, 8, 1>();
    if (op.tokens <= 16) return launch.template operator()<16, 4, 2>();
    return launch.template operator()<32, 4, 1>();
#else
    if (op.tokens == 2)
        return launch_q5_a16_split2<kHidden, 2>(op, output, LinearIdentityEpilogue{}, stream);
    if (op.tokens <= 8) return launch.template operator()<8, 8, 2>();
    if (op.tokens <= 16) return launch.template operator()<16, 8, 1>();
    if (op.tokens <= 28) return launch.template operator()<32, 8, 1>();
    return launch.template operator()<32, 4, 1>();
#endif
}

void launch_native_parts(const Tensor& x, const Weight& qk_weight, const Weight& value_z_weight,
                         const Tensor& qk, const Tensor& value, const Tensor& z,
                         cudaStream_t stream) {
    launch_native_q4(q4_linear_operands(x, qk_weight),
        LinearBf16StridedOutput{static_cast<__nv_bfloat16*>(qk.data),
            std::int64_t(qk.nb[1] / sizeof(__nv_bfloat16)), 0}, stream);
    launch_native_q5(q5_linear_operands(x, value_z_weight),
        LinearBf16SplitOutput2<kValueRows>{
            {static_cast<__nv_bfloat16*>(value.data),
             std::int64_t(value.nb[1] / sizeof(__nv_bfloat16)), 0},
            {static_cast<__nv_bfloat16*>(z.data),
             std::int64_t(z.nb[1] / sizeof(__nv_bfloat16)), 0}}, stream);
}
#else
using SmallR8T4 = Q4A16SimtSchedule<8, 4, 1, 16, 2, Cache::ca, 1>;
using SmallR8T8 = Q4A16SimtSchedule<8, 8, 1, 16, 2, Cache::ca, 1>;

void launch_q4_gemv(const Tensor& x, const Weight& weight, const Tensor& out, cudaStream_t stream) {
    launch_q4_a16_gemv<q4_instances::GemvR1W8K5120>(
        q4_linear_operands(x, weight),
        LinearBf16StridedOutput{static_cast<__nv_bfloat16*>(out.data),
                                static_cast<std::int64_t>(out.nb[1] / sizeof(__nv_bfloat16)), 0},
        LinearIdentityEpilogue{}, stream);
}

template <class Schedule>
void launch_q4_simt(const Tensor& x, const Weight& weight, const Tensor& out, cudaStream_t stream) {
    launch_q4_a16_simt<Schedule>(
        q4_linear_operands(x, weight),
        LinearBf16StridedOutput{static_cast<__nv_bfloat16*>(out.data),
                                static_cast<std::int64_t>(out.nb[1] / sizeof(__nv_bfloat16)), 0},
        LinearIdentityEpilogue{}, stream);
}

template <std::int32_t Capacity,
          class Schedule = Q4A16SlicedKMmaSchedule<16, (Capacity + 7) / 8 * 8, 8, 1,
                                                   Cache::cg, Cache::ca, 6, kHidden, Capacity>>
void launch_q4_sliced_exact(const Tensor& x, const Weight& weight, const Tensor& out,
                            cudaStream_t stream) {
    launch_q4_a16_sliced_k_mma<Schedule>(
        q4_linear_operands(x, weight),
        LinearBf16StridedOutput{static_cast<__nv_bfloat16*>(out.data),
                                static_cast<std::int64_t>(out.nb[1] / sizeof(__nv_bfloat16)), 0},
        LinearIdentityEpilogue{}, stream);
}

void launch_q4_sliced_band(const Tensor& x, const Weight& weight, const Tensor& out,
                           cudaStream_t stream) {
    if (weight.padded_shape[1] != kHidden) {
        throw std::invalid_argument("Q4/Q5 split K-split requires padded K == hidden");
    }
    switch (x.ne[1]) {
    case 7:
        launch_q4_sliced_exact<7>(x, weight, out, stream);
        return;
    case 8:
#if !defined(NINFER_SM86) && !defined(NINFER_SM89)
        launch_q4_sliced_exact<8, q4_instances::SlicedK5120T8PairwisePad16>(x, weight, out, stream);
        return;
    case 16:
        launch_q4_sliced_exact<16, q4_instances::SlicedK5120T16PairwisePad16>(x, weight, out, stream);
#else
        launch_q4_sliced_exact<8>(x, weight, out, stream);
#endif
        return;
    case 9:
        launch_q4_sliced_exact<9>(x, weight, out, stream);
        return;
    case 10:
        launch_q4_sliced_exact<10>(x, weight, out, stream);
        return;
    case 11:
        launch_q4_sliced_exact<11>(x, weight, out, stream);
        return;
    case 12:
        launch_q4_sliced_exact<12>(x, weight, out, stream);
        return;
    default:
        throw std::invalid_argument("unsupported Q4/Q5 split K-split extent");
    }
}

void launch_q4(const Tensor& x, const Weight& weight, const Tensor& out, cudaStream_t stream) {
    switch (x.ne[1]) {
    case 1:
        launch_q4_gemv(x, weight, out, stream);
        return;
    case 2:
    case 3:
    case 4:
        launch_q4_simt<SmallR8T4>(x, weight, out, stream);
        return;
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
#if !defined(NINFER_SM86) && !defined(NINFER_SM89)
    case 16:
#endif
        launch_q4_sliced_band(x, weight, out, stream);
        return;
    default:
        if (x.ne[1] <= 15) {
            launch_q4_simt<SmallR8T8>(x, weight, out, stream);
            return;
        }
        throw std::invalid_argument("unsupported Q4/Q5 split independent extent");
    }
}

auto q5_projection_output(const Tensor& first, const Tensor& second) {
    return LinearBf16SplitOutput2<kValueRows>{
        {static_cast<__nv_bfloat16*>(first.data), std::int64_t(first.nb[1] / sizeof(__nv_bfloat16)),
         0},
        {static_cast<__nv_bfloat16*>(second.data),
         std::int64_t(second.nb[1] / sizeof(__nv_bfloat16)), 0}};
}

void launch_q5_gemv(const Tensor& x, const Weight& weight, const Tensor& value, const Tensor& z,
                    cudaStream_t stream) {
    launch_q5_a16_gemv<q5_instances::GemvR16W1G16S2XK5120>(q5_linear_operands(x, weight),
                                                           q5_projection_output(value, z),
                                                           LinearIdentityEpilogue{}, stream);
}

template <int Cols>
void launch_q5_split4(const Tensor& x, const Weight& weight, const Tensor& value, const Tensor& z,
                      cudaStream_t stream) {
    using Schedule = Q5A16DirectSimtSchedule<1, Cols, 4, 4, 10, kHidden, true>;
    launch_q5_a16_direct_simt<Schedule>(q5_linear_operands(x, weight),
                                        q5_projection_output(value, z), LinearIdentityEpilogue{},
                                        stream);
}

void launch_q5_split4_exact(const Tensor& x, const Weight& weight, const Tensor& value,
                            const Tensor& z, cudaStream_t stream) {
    switch (x.ne[1]) {
    case 2:
        launch_q5_split4<2>(x, weight, value, z, stream);
        return;
    case 3:
        launch_q5_split4<3>(x, weight, value, z, stream);
        return;
    case 4:
        launch_q5_split4<4>(x, weight, value, z, stream);
        return;
    case 5:
        launch_q5_split4<5>(x, weight, value, z, stream);
        return;
    case 6:
        launch_q5_split4<6>(x, weight, value, z, stream);
        return;
    case 7:
        launch_q5_split4<7>(x, weight, value, z, stream);
        return;
    case 8:
        launch_q5_split4<8>(x, weight, value, z, stream);
        return;
    case 9:
        launch_q5_split4<9>(x, weight, value, z, stream);
        return;
    case 10:
        launch_q5_split4<10>(x, weight, value, z, stream);
        return;
    default:
        throw std::invalid_argument("Q5 split split4 requires T in [2,10]");
    }
}

template <int kColsPerTile>
void launch_q5_simt_cols(const Tensor& x, const Weight& weight, const Tensor& value,
                         const Tensor& z, cudaStream_t stream) {
    using Schedule = Q5A16SimtSchedule<8, kColsPerTile, 1, 16, 2, Cache::ca, 1>;
    launch_q5_a16_simt<Schedule>(q5_linear_operands(x, weight), q5_projection_output(value, z),
                                 LinearIdentityEpilogue{}, stream);
}

#if !defined(NINFER_SM86) && !defined(NINFER_SM89)
template <int Tokens>
void launch_q5_verify(const Tensor& x, const Weight& weight, const Tensor& value,
                      const Tensor& z, cudaStream_t stream) {
    using Schedule = Q5A16SlicedKMmaSchedule<16, Tokens, 8, Tokens == 8 ? 3 : 2,
                                             Cache::cg, Cache::ca, 1, kHidden, Tokens,
                                             Q5SlicedKReduction::Linear, 16, false>;
    launch_q5_a16_sliced_k_mma<Schedule>(
        q5_linear_operands(x, weight), q5_projection_output(value, z),
        LinearIdentityEpilogue{}, stream);
}
#endif

void launch_q5(const Tensor& x, const Weight& weight, const Tensor& value, const Tensor& z,
               cudaStream_t stream) {
    if (x.ne[1] == 1) {
        launch_q5_gemv(x, weight, value, z, stream);
        return;
    }
#if !defined(NINFER_SM86) && !defined(NINFER_SM89)
    if (x.ne[1] == 8) {
        launch_q5_verify<8>(x, weight, value, z, stream);
        return;
    }
    if (x.ne[1] == 16) {
        launch_q5_verify<16>(x, weight, value, z, stream);
        return;
    }
#endif
    if (x.ne[1] <= 10) {
        launch_q5_split4_exact(x, weight, value, z, stream);
        return;
    }
    if (x.ne[1] <= 12) {
        launch_q5_simt_cols<4>(x, weight, value, z, stream);
        return;
    }
    if (x.ne[1] <= 15) {
        launch_q5_simt_cols<8>(x, weight, value, z, stream);
        return;
    }
    throw std::invalid_argument("unsupported Q4/Q5 split independent extent");
}

void launch_t4_pdl(const Tensor& x, const Weight& qk_weight, const Weight& value_z_weight,
                   const Tensor& qk, const Tensor& value, const Tensor& z, cudaStream_t stream) {
    using Q4Schedule = SmallR8T4;
    using Q5Schedule = Q5A16DirectSimtSchedule<1, 4, 4, 4, 10, kHidden, true>;
    // Q5 and Q4 publish disjoint rows; Q4 joins the Q5 producer at kernel exit.
    launch_q5_a16_direct_simt<Q5Schedule, true>(q5_linear_operands(x, value_z_weight),
                                                q5_projection_output(value, z),
                                                LinearIdentityEpilogue{}, stream);
    launch_q4_a16_simt<Q4Schedule, false, true, true>(
        q4_linear_operands(x, qk_weight),
        LinearBf16StridedOutput{static_cast<__nv_bfloat16*>(qk.data),
                                static_cast<std::int64_t>(qk.nb[1] / sizeof(__nv_bfloat16)), 0},
        LinearIdentityEpilogue{}, stream);
}
#endif

} // namespace

void launch_parts(const Tensor& x, const Weight& qk_weight, const Weight& value_z_weight,
                  const Tensor& qk, const Tensor& value, const Tensor& z, cudaStream_t stream) {
#if defined(NINFER_SM86) || defined(NINFER_SM89)
    launch_native_parts(x, qk_weight, value_z_weight, qk, value, z, stream);
#else
    if (x.ne[1] == 4) {
        launch_t4_pdl(x, qk_weight, value_z_weight, qk, value, z, stream);
        return;
    }
    launch_q4(x, qk_weight, qk, stream);
    launch_q5(x, value_z_weight, value, z, stream);
#endif
}

void launch_small(const Tensor& x, const Weight& first, const Weight& second,
                  std::span<const Tensor> out, cudaStream_t stream) {
    Tensor first_rows = out[0].slice(0, 0, 4096);
    Tensor tail_rows  = out[0].slice(0, 4096, 6144);
    launch_parts(x, first, second, first_rows, tail_rows, out[1], stream);
}
} // namespace ninfer::ops::detail::linear_split::q4_q5_n4096_n12288_k5120
