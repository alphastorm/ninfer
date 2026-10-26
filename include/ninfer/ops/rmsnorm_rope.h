#pragma once

#include "core/device.h"
#include "core/tensor.h"


namespace ninfer::ops {

/**
 * Apply plain per-head RMSNorm followed by full-head split-half 1-D RoPE in place.
 *
 * The pair profile is q BF16 [128,32,W,B], k BF16 [128,8,W,B], q_norm_weight and
 * k_norm_weight BF16 [128], and positions I32 [W,B], with W=2..16 and B=1..8. For each head and
 * physical token, the complete mathematical operation is
 *
 *   inv       = 1 / sqrt(sum_d x[d]^2 / 128 + 1e-6)
 *   n[d]      = x[d] * inv * norm_weight[d]
 *   angle(i)  = position * (1e7)^(-2*i/128), 0<=i<64
 *   out[i]    = n[i]    * cos(angle(i)) - n[i+64] * sin(angle(i))
 *   out[i+64] = n[i+64] * cos(angle(i)) + n[i]    * sin(angle(i)).
 *
 * Normalization and rotation form one semantic operation: there is no observable BF16
 * materialization of n. q and k are completely overwritten in place and must not overlap each
 * other, positions, or either norm weight. Read-only norm weights may overlap each other.
 * positions and weights remain unchanged. All tensors are contiguous and 4-byte aligned.
 * The Op owns no workspace or persistent state.
 */
void rmsnorm_rope(const Tensor& positions, const Tensor& q_norm_weight, const Tensor& k_norm_weight,
                  Tensor& q, Tensor& k, CudaExecutionView execution);

/**
 * Single-K form of the same formula and effects. x is BF16 [128,8,T], norm_weight is BF16 [128],
 * and positions is I32 [T], with T=1..2048. x is completely overwritten in place and must not
 * overlap either read-only input.
 */
void rmsnorm_rope(const Tensor& positions, const Tensor& norm_weight, Tensor& x,
                  CudaExecutionView execution);

/**
 * Text full-attention query/key preparation: unit-offset RMSNorm materialized in BF16, then
 * partial split-half RoPE over the first rotary_dim elements of each head.
 *
 * Results are bitwise identical to
 *
 *   rmsnorm(q, q_norm_weight, eps, true, qn); rmsnorm(k, k_norm_weight, eps, true, kn);
 *   rope(positions, rotary_dim, theta, qn, kn).
 *
 * q/qn are BF16 [D,Hq,T], k/kn are BF16 [D,Hkv,T], the weights are BF16 [D], and positions is I32
 * [T] or [T,3] with the same validity rules as those Ops. The D=256, rotary_dim=64, theta=1e7
 * geometries with (Hq,Hkv) = (24,4) or (16,2) and contiguous operands run as one kernel; every
 * other valid call issues the three Ops above. q, k, positions and weights remain unchanged; qn
 * and kn are fully written and must not overlap any input or each other.
 */
void rmsnorm_offset_rope(const Tensor& positions, int rotary_dim, float theta, const Tensor& q,
                         const Tensor& k, const Tensor& q_norm_weight, const Tensor& k_norm_weight,
                         float eps, Tensor& qn, Tensor& kn, CudaExecutionView execution);

} // namespace ninfer::ops
