#include "ops/gdn_input_proj/q4_q5/q4_q5_gdn_input_kernels.h"

#include "core/device.h"
#include "core/pdl.cuh"
#include "ops/common/math.h"
#include "ops/linear/q4/q4_rowsplit_gemm_simt.cuh"
#include "ops/linear/q4/q4_rowsplit_gemv.cuh"
#include "ops/linear/q5/q5_rowsplit_gemm_simt.cuh"
#include "ops/linear/q5/q5_rowsplit_gemv.cuh"

#include <cuda_bf16.h>

#include <cstdint>
#include <stdexcept>

namespace ninfer::ops::detail {
namespace {

constexpr std::int32_t kQkRows     = 4096;
constexpr std::int32_t kValueRows  = 6144;
constexpr std::int32_t kZRows      = 6144;
constexpr std::int32_t kValueZRows = kValueRows + kZRows;
constexpr std::int32_t kHidden     = 5120;

using Q4GdnSimtR8C8Schedule = Q4RowSplitSimtGemmSchedule<8, 8, 16, 2, Cache::ca, 1>;

static_assert(kQkRows % kQ4SimtRowsWarpsPerCta == 0 && kValueZRows % kQ5SimtRowsPerWarp == 0 &&
              kHidden % 1024 == 0);

void launch_q4_gemv(const Tensor& x, const Weight& weight, Tensor& out, cudaStream_t stream) {
    using Schedule = Q4GemvR1W8DirectSchedule;
    const dim3 grid(static_cast<unsigned>(div_up(kQkRows, Schedule::kRowsPerCta)), 1u, 1u);
    constexpr dim3 block(static_cast<unsigned>(Schedule::kThreads), 1u, 1u);
    q4_rowsplit_gemv_kernel<Schedule><<<grid, block, 0, stream>>>(
        static_cast<const __nv_bfloat16*>(x.data), static_cast<const std::uint8_t*>(weight.qdata),
        static_cast<const std::uint8_t*>(weight.scales), static_cast<__nv_bfloat16*>(out.data),
        nullptr, kQkRows, kHidden);
    CUDA_CHECK(cudaGetLastError());
}

template <class Schedule, bool Full>
void launch_q4_simt(const Tensor& x, const Weight& weight, Tensor& out, cudaStream_t stream) {
    const std::int32_t cols   = x.ne[1];
    const std::int32_t out_ld = static_cast<std::int32_t>(out.nb[1] / sizeof(__nv_bfloat16));
    const dim3 grid(static_cast<unsigned>(div_up(kQkRows, Schedule::kRowsPerCta)),
                    static_cast<unsigned>(div_up(cols, Schedule::kColsPerTile)), 1u);
    q4_rowsplit_gemm_simt_kernel<Schedule, Full><<<grid, Schedule::kThreads, 0, stream>>>(
        static_cast<const __nv_bfloat16*>(x.data), static_cast<const std::uint8_t*>(weight.qdata),
        static_cast<const std::uint8_t*>(weight.scales), static_cast<__nv_bfloat16*>(out.data),
        nullptr, out_ld, 0, kQkRows, kHidden, cols, weight.padded_shape[1]);
    CUDA_CHECK(cudaGetLastError());
}

template <class Schedule>
void launch_q4_simt_route(const Tensor& x, const Weight& weight, Tensor& out, cudaStream_t stream) {
    const bool full = (kQkRows % Schedule::kRowsPerCta) == 0 &&
                      ((kHidden / Q4RowSplitStorage::kGroupK) % Schedule::kGroupsPerStage) == 0 &&
                      (x.ne[1] % Schedule::kColsPerTile) == 0;
    if (full) {
        launch_q4_simt<Schedule, true>(x, weight, out, stream);
    } else {
        launch_q4_simt<Schedule, false>(x, weight, out, stream);
    }
}

// Up to four columns the query/key side runs one row per warp with direct loads (EXP-055: 19-48%
// faster than the staged R8C4 schedule on RTX 5090 at T=2-4, bit-identical per row).
template <int Cols, bool JoinPdl = false>
void launch_q4_rows(const Tensor& x, const Weight& weight, Tensor& out, cudaStream_t stream) {
    const auto* xp           = static_cast<const __nv_bfloat16*>(x.data);
    const auto* codes        = static_cast<const std::uint8_t*>(weight.qdata);
    const auto* scales       = static_cast<const std::uint8_t*>(weight.scales);
    auto* outp               = static_cast<__nv_bfloat16*>(out.data);
    const std::int32_t ld    = static_cast<std::int32_t>(out.nb[1] / sizeof(__nv_bfloat16));
    const std::int32_t pad_k = weight.padded_shape[1];
    const dim3 grid(kQkRows / kQ4SimtRowsWarpsPerCta, 1u, 1u);
    const dim3 block(kQ4SimtRowsWarpsPerCta * 32, 1u, 1u);
    auto* kernel =
        q4_rowsplit_gemm_simt_rows_kernel<Cols, kHidden / 1024, 1, false, 0, false, JoinPdl>;
    if constexpr (JoinPdl) {
        CUDA_CHECK(pdl::launch_dependent({grid, block, 0, stream}, kernel, xp, codes, scales, outp,
                                         nullptr, ld, 0, kHidden, pad_k));
    } else {
        kernel<<<grid, block, 0, stream>>>(xp, codes, scales, outp, nullptr, ld, 0, kHidden, pad_k);
        CUDA_CHECK(cudaGetLastError());
    }
}

void launch_q4(const Tensor& x, const Weight& weight, Tensor& out, cudaStream_t stream) {
    switch (x.ne[1]) {
    case 1:
        launch_q4_gemv(x, weight, out, stream);
        return;
    case 2:
        launch_q4_rows<2>(x, weight, out, stream);
        return;
    case 3:
        launch_q4_rows<3>(x, weight, out, stream);
        return;
    case 4:
        launch_q4_rows<4>(x, weight, out, stream);
        return;
    default:
        break;
    }
    if (x.ne[1] <= 16) {
        launch_q4_simt_route<Q4GdnSimtR8C8Schedule>(x, weight, out, stream);
        return;
    }
    throw std::invalid_argument("Q4/Q5 GDN independent launch requires T in [1,16]");
}

void launch_q5_gemv(const Tensor& x, const Weight& weight, Tensor& value, Tensor& z,
                    cudaStream_t stream) {
    constexpr int kRowsPerBlock = 16;
    constexpr int kThreads      = kRowsPerBlock * 32;
    q5_rowsplit_gemv_kernel<kValueZRows, kHidden, kRowsPerBlock, 2, true, false, true, kValueRows>
        <<<kValueZRows / kRowsPerBlock, kThreads, 0, stream>>>(
            static_cast<const __nv_bfloat16*>(x.data),
            static_cast<const std::uint8_t*>(weight.qdata),
            static_cast<const std::uint8_t*>(weight.qhigh),
            static_cast<const std::uint8_t*>(weight.scales),
            static_cast<__nv_bfloat16*>(value.data), static_cast<__nv_bfloat16*>(z.data));
    CUDA_CHECK(cudaGetLastError());
}

// From three columns the value/z side runs two rows per warp (EXP-055: 6-11% faster than one row
// on RTX 5090 at T=3-6, bit-identical per row); at T=2 the one-row kernel is as fast.
template <int Cols, bool TriggerPdl = false>
void launch_q5_split4(const Tensor& x, const Weight& weight, Tensor& value, Tensor& z,
                      cudaStream_t stream) {
    constexpr int kThreads    = 4 * 32;
    const std::int32_t out_ld = static_cast<std::int32_t>(value.nb[1] / sizeof(__nv_bfloat16));
    if constexpr (Cols >= 3) {
        const dim3 grid(static_cast<unsigned>(kValueZRows / kQ5SimtRowsPerWarp), 1u, 1u);
        q5_rowsplit_gemm_simt_split4_rows_kernel<Cols, 5, kHidden, true, kValueRows, TriggerPdl>
            <<<grid, kThreads, 0, stream>>>(static_cast<const __nv_bfloat16*>(x.data),
                                            static_cast<const std::uint8_t*>(weight.qdata),
                                            static_cast<const std::uint8_t*>(weight.qhigh),
                                            static_cast<const std::uint8_t*>(weight.scales),
                                            static_cast<__nv_bfloat16*>(value.data),
                                            static_cast<__nv_bfloat16*>(z.data), kValueZRows,
                                            out_ld, weight.padded_shape[1]);
    } else {
        static_assert(!TriggerPdl);
        const dim3 grid(static_cast<unsigned>(kValueZRows), 1u, 1u);
        q5_rowsplit_gemm_simt_split4_kernel<Q5RowSplitSimtSchedule, Cols, 5, kHidden, true,
                                            kValueRows><<<grid, kThreads, 0, stream>>>(
            static_cast<const __nv_bfloat16*>(x.data),
            static_cast<const std::uint8_t*>(weight.qdata),
            static_cast<const std::uint8_t*>(weight.qhigh),
            static_cast<const std::uint8_t*>(weight.scales),
            static_cast<__nv_bfloat16*>(value.data), static_cast<__nv_bfloat16*>(z.data),
            kValueZRows, out_ld, kHidden, Cols, weight.padded_shape[1], 5);
    }
    CUDA_CHECK(cudaGetLastError());
}

void launch_q5_split4_exact(const Tensor& x, const Weight& weight, Tensor& value, Tensor& z,
                            cudaStream_t stream) {
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
    default:
        throw std::invalid_argument("GDN Q5 split4 requires T in [2,6]");
    }
}

void launch_q5_simt_r8_c8(const Tensor& x, const Weight& weight, Tensor& value, Tensor& z,
                          cudaStream_t stream) {
    constexpr int kColsPerTile  = 8;
    constexpr int kRowsPerBlock = 8;
    constexpr int kStages       = 2;
    constexpr int kThreads      = kRowsPerBlock * 32;
    const std::int32_t cols     = x.ne[1];
    const std::int32_t out_ld   = static_cast<std::int32_t>(value.nb[1] / sizeof(__nv_bfloat16));
    const dim3 grid(static_cast<unsigned>(div_up(kValueZRows, kRowsPerBlock)),
                    static_cast<unsigned>(div_up(cols, kColsPerTile)), 1u);
    q5_rowsplit_gemm_simt_kernel<Q5RowSplitSimtSchedule, kColsPerTile, kRowsPerBlock, kStages, true,
                                 kValueRows><<<grid, kThreads, 0, stream>>>(
        static_cast<const __nv_bfloat16*>(x.data), static_cast<const std::uint8_t*>(weight.qdata),
        static_cast<const std::uint8_t*>(weight.qhigh),
        static_cast<const std::uint8_t*>(weight.scales), static_cast<__nv_bfloat16*>(value.data),
        static_cast<__nv_bfloat16*>(z.data), kValueZRows, out_ld, kHidden, cols,
        weight.padded_shape[1], 5);
    CUDA_CHECK(cudaGetLastError());
}

void launch_q5(const Tensor& x, const Weight& weight, Tensor& value, Tensor& z,
               cudaStream_t stream) {
    if (x.ne[1] == 1) {
        launch_q5_gemv(x, weight, value, z, stream);
        return;
    }
    if (x.ne[1] <= 6) {
        launch_q5_split4_exact(x, weight, value, z, stream);
        return;
    }
    if (x.ne[1] <= 16) {
        launch_q5_simt_r8_c8(x, weight, value, z, stream);
        return;
    }
    throw std::invalid_argument("Q4/Q5 GDN independent launch requires T in [1,16]");
}

void launch_t4_pdl(const Tensor& x, const Weight& qk_weight, const Weight& value_z_weight,
                   Tensor& qk, Tensor& value, Tensor& z, cudaStream_t stream) {
    // Q5 and Q4 publish disjoint row ranges. Q4 can execute while Q5 drains and joins Q5 only at
    // exit, before the following convolution/snapshot kernel becomes runnable.
    launch_q5_split4<4, true>(x, value_z_weight, value, z, stream);
    launch_q4_rows<4, true>(x, qk_weight, qk, stream);
}

} // namespace

void q4_q5_gdn_input_independent_launch(const Tensor& x, const Weight& qk_weight,
                                        const Weight& value_z_weight, Tensor& qk, Tensor& value,
                                        Tensor& z, cudaStream_t stream) {
    if (x.ne[1] == 4) {
        launch_t4_pdl(x, qk_weight, value_z_weight, qk, value, z, stream);
        return;
    }
    launch_q4(x, qk_weight, qk, stream);
    launch_q5(x, value_z_weight, value, z, stream);
}

} // namespace ninfer::ops::detail
