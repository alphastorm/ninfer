#pragma once
#include "core/device.h"
#include "ninfer/ops/gated_residual.h"
#include "ops/op_tester.h"

namespace ninfer::test::hc {
inline constexpr int kWidth = 2560, kStreams = 4;
inline constexpr float kEpsilon = 1.0e-6F;
enum class Form { ExpandNorm, GroupedNorm, Combine, CombineNorm, AddNorm };
enum class Pattern { Random, Affine, Zero, Constant, Cancellation, Midpoint, Small };
const char* name(Form form);

struct Fixture {
    Fixture(Form form, int columns, bool in_place);
    void set(Pattern pattern, int phase = 0);
    void upload(cudaStream_t stream);
    void invoke(CudaExecutionView execution);
    int check(const std::string& label) const;

    Form form;
    int columns;
    bool in_place;
    float epsilon = kEpsilon;
    std::vector<float> h, block, weight, injection;
    GuardedDeviceBuffer dh, db, dw, dj, dr, dz;
    Tensor th, tb, tw, tj, tr, tz;
};
} // namespace ninfer::test::hc
