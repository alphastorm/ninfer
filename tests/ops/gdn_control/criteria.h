#pragma once

#include "ops/op_check.h"

#include <limits>

namespace ninfer::test::gdn_control {

inline constexpr double kFp32UnitRoundoff = std::numeric_limits<float>::epsilon() / 2.0;

// Against the complete FP64 formula, expf contributes up to two ULP from the correctly
// rounded result and log1pf one ULP. Allow 16u for decay's addition, two exponentials,
// logarithm and multiplication; sigmoid's exponential, addition and reciprocal need 8u.
// Absolute floors cover small outputs, where relative error is poorly conditioned.
inline constexpr PointwiseCriterion kDecay{1.0e-7, 16.0 * kFp32UnitRoundoff};
inline constexpr PointwiseCriterion kSigmoid{1.0e-8, 8.0 * kFp32UnitRoundoff};

// The longest registered BF16 dot has K=5120, with about 320 FP32 MMA accumulation
// steps. gamma(320) supplies a 19.1 ppm accumulation-depth estimate; add decay's 16u
// epilogue budget. This finite gross cap is a numerical quality limit, not a worst-case
// bound for arbitrary dot-product conditioning. Keep the stricter 1.4 ppm relative L2
// criterion to reject distributed error. The whole projection family uses this budget.
inline constexpr double kProjectionAccumulationDepth = 5120.0 / 16.0;
inline constexpr double kProjectionAccumulationError =
    kProjectionAccumulationDepth * kFp32UnitRoundoff /
    (1.0 - kProjectionAccumulationDepth * kFp32UnitRoundoff);
inline constexpr ReductionCriterion kProjection{1.4e-6, 5.0e-7,
                                                kProjectionAccumulationError + kDecay.relative};

inline constexpr ReductionCriterion kNormalized{1.75e-3, 1.0e-4, 4.0e-3};
inline constexpr ReductionCriterion kNormControl{8.0e-4, 1.5e-4, 2.0e-3};

} // namespace ninfer::test::gdn_control
