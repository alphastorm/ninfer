#pragma once

#include "ninfer/ops/ple.h"

namespace ninfer::ops::detail::ple {

inline constexpr int kFeatures = 2560;
inline constexpr int kStreams  = 4;
inline constexpr int kChannels = kFeatures * kStreams;
inline constexpr int kHistory  = 9;

inline bool pair_aligned(const Tensor& t) {
    if (reinterpret_cast<std::uintptr_t>(t.data) % 4) return false;
    for (int d = 1; d < 4; ++d)
        if (t.ne[d] > 1 && t.nb[d] % 4) return false;
    return true;
}

void validate_gate(const PleProjectionView& projected, const Tensor& residual,
                   const PleGateParameters& parameters, const PleBatchView& batch,
                   const PleConvRecordWindowView& records, const Tensor& gated,
                   CudaExecutionView execution);
void validate_conv(const Tensor& residual, const Tensor& gated, const Tensor& kernel,
                   const PleBatchView& batch, const PleConvStateView& before,
                   const PleConvRecordWindowView& current, const Tensor& updated,
                   const Tensor* norm_weight, float epsilon, const Tensor* normalized,
                   CudaExecutionView execution);
void validate_extract(const PleConvStateView& before, const PleConvRecordReadView& records,
                      const Tensor& accepted, const PleConvStateView& after,
                      CudaExecutionView execution);

} // namespace ninfer::ops::detail::ple
