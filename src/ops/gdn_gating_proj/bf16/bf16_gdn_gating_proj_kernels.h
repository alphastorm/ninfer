#pragma once

#include "core/tensor.h"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace ninfer::ops::detail {

enum class Bf16GdnGatingTokenVariant {
    None,
    Full,
    Predicated,
};

// Device-wide count of CTAs the current device can keep resident for one cooperative BF16 GDN
// gating GEMM launch with the given split-K on the 27B (128-column) or 35B (64-column) geometry:
// the occupancy the driver reports for that exact kernel instantiation times the SM count. The
// cooperative reduction needs every CTA co-resident, so a plan must never launch a larger grid.
// Cached per process; returns 0 for splits the geometry does not instantiate.
[[nodiscard]] std::int32_t bf16_gdn_gating_proj_cooperative_resident_ctas(int split_k,
                                                                          bool geometry_35);

// The cooperative split-K launches enforce that budget themselves: a grid that exceeds it is run
// as several cooperative launches over disjoint token-tile intervals (token tiles share no
// reduction, so each interval reuses the same partial workspace in stream order). They return
// false only when one token tile alone exceeds the budget; the caller then runs the unsplit GEMM.
// The plan therefore resolves the same schedule on every device and its workspace query equals
// what executes.

void bf16_gdn_gating_proj_gemv_launch(const Tensor& x, const Weight& a_weight,
                                      const Weight& b_weight, const Tensor& A_log,
                                      const Tensor& dt_bias, Tensor& g, Tensor& beta,
                                      cudaStream_t stream);
void bf16_gdn_gating_proj_small_t_split10_launch(const Tensor& x, const Weight& a_weight,
                                                 const Weight& b_weight, const Tensor& A_log,
                                                 const Tensor& dt_bias, void* workspace,
                                                 std::size_t workspace_bytes, Tensor& g,
                                                 Tensor& beta, cudaStream_t stream);
[[nodiscard]] bool
bf16_gdn_gating_proj_mma_split8_launch(Bf16GdnGatingTokenVariant variant, const Tensor& x,
                                       const Weight& a_weight, const Weight& b_weight,
                                       const Tensor& A_log, const Tensor& dt_bias, void* workspace,
                                       Tensor& g, Tensor& beta, cudaStream_t stream);
[[nodiscard]] bool
bf16_gdn_gating_proj_mma_split4_launch(Bf16GdnGatingTokenVariant variant, const Tensor& x,
                                       const Weight& a_weight, const Weight& b_weight,
                                       const Tensor& A_log, const Tensor& dt_bias, void* workspace,
                                       Tensor& g, Tensor& beta, cudaStream_t stream);
[[nodiscard]] bool
bf16_gdn_gating_proj_mma_split2_launch(Bf16GdnGatingTokenVariant variant, const Tensor& x,
                                       const Weight& a_weight, const Weight& b_weight,
                                       const Tensor& A_log, const Tensor& dt_bias, void* workspace,
                                       Tensor& g, Tensor& beta, cudaStream_t stream);
void bf16_gdn_gating_proj_mma_unsplit_launch(Bf16GdnGatingTokenVariant variant, const Tensor& x,
                                             const Weight& a_weight, const Weight& b_weight,
                                             const Tensor& A_log, const Tensor& dt_bias, Tensor& g,
                                             Tensor& beta, cudaStream_t stream);

void bf16_gdn_gating_proj_35_simt_c4_launch(const Tensor& x, const Weight& a_weight,
                                            const Weight& b_weight, const Tensor& A_log,
                                            const Tensor& dt_bias, Tensor& g, Tensor& beta,
                                            cudaStream_t stream);
void bf16_gdn_gating_proj_35_simt_c8_launch(const Tensor& x, const Weight& a_weight,
                                            const Weight& b_weight, const Tensor& A_log,
                                            const Tensor& dt_bias, Tensor& g, Tensor& beta,
                                            cudaStream_t stream);
[[nodiscard]] bool bf16_gdn_gating_proj_35_mma_split32_launch(
    Bf16GdnGatingTokenVariant variant, const Tensor& x, const Weight& a_weight,
    const Weight& b_weight, const Tensor& A_log, const Tensor& dt_bias, void* workspace, Tensor& g,
    Tensor& beta, cudaStream_t stream);
[[nodiscard]] bool bf16_gdn_norm_gating_proj_35_mma_split32_launch(
    Bf16GdnGatingTokenVariant variant, const Tensor& x, const Tensor& norm_weight, float eps,
    Tensor& h, const Weight& a_weight, const Weight& b_weight, const Tensor& A_log,
    const Tensor& dt_bias, void* workspace, Tensor& g, Tensor& beta, cudaStream_t stream);
[[nodiscard]] bool bf16_gdn_gating_proj_35_mma_split16_launch(
    Bf16GdnGatingTokenVariant variant, const Tensor& x, const Weight& a_weight,
    const Weight& b_weight, const Tensor& A_log, const Tensor& dt_bias, void* workspace, Tensor& g,
    Tensor& beta, cudaStream_t stream);
[[nodiscard]] bool bf16_gdn_gating_proj_35_mma_split8_launch(
    Bf16GdnGatingTokenVariant variant, const Tensor& x, const Weight& a_weight,
    const Weight& b_weight, const Tensor& A_log, const Tensor& dt_bias, void* workspace, Tensor& g,
    Tensor& beta, cudaStream_t stream);
[[nodiscard]] bool bf16_gdn_gating_proj_35_mma_split4_launch(
    Bf16GdnGatingTokenVariant variant, const Tensor& x, const Weight& a_weight,
    const Weight& b_weight, const Tensor& A_log, const Tensor& dt_bias, void* workspace, Tensor& g,
    Tensor& beta, cudaStream_t stream);
[[nodiscard]] bool bf16_gdn_gating_proj_35_mma_split2_launch(
    Bf16GdnGatingTokenVariant variant, const Tensor& x, const Weight& a_weight,
    const Weight& b_weight, const Tensor& A_log, const Tensor& dt_bias, void* workspace, Tensor& g,
    Tensor& beta, cudaStream_t stream);
void bf16_gdn_gating_proj_35_mma_unsplit_launch(Bf16GdnGatingTokenVariant variant, const Tensor& x,
                                                const Weight& a_weight, const Weight& b_weight,
                                                const Tensor& A_log, const Tensor& dt_bias,
                                                Tensor& g, Tensor& beta, cudaStream_t stream);

} // namespace ninfer::ops::detail
