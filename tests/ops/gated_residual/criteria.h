#pragma once
#include "ops/op_check.h"

namespace ninfer::test::hc {
// BF16 RNE has unit roundoff 2^-8. Coherent storage error can approach that
// bound even on constant, unit-scale inputs; random-input RMS limits are not
// universal bounds. Leave the small remaining margin for FP32 FMA/reduction/rsqrt.
// Nonfinite results fail. These budgets cover all forms/routes and all cases.
inline constexpr PointwiseCriterion kResidual{1.0e-7, 4.0e-3};
inline constexpr ReductionCriterion kNormalized{4.0e-3, 1.0e-6, 4.0e-3};
// Mix includes BF16 u/output storage and FP32 dot reductions.
// sigmoid is 1/4-Lipschitz; injection's derivative with respect to its dot is at most 1/8.
// The output L2 budget covers staging and sigmoid sensitivity, with a finite gross cap. Injection
// uses its own pointwise budget. The same A16 budgets apply to both forms and every production
// route.
inline constexpr ReductionCriterion kMixOutput{8.0e-3, 1.0e-5, 1.6e-2};
inline constexpr PointwiseCriterion kMixInjection{2.0e-6, 2.0e-5};
} // namespace ninfer::test::hc
