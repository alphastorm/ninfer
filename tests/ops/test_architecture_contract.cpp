#include "ops/op_tester.h"
#include "ops/linear/kernels/fp8/fp8_a8_plan.h"
#include "ops/linear/kernels/nvfp4/nvfp4_a4_plan.h"
#include "ops/moe/kernels/cuda/block_scaled/fp8/launch.h"
#include "ops/moe/kernels/cuda/block_scaled/nvfp4/launch.h"
#include "ops/softmax_attention/dense/causal_cache/fp8/launch.h"
#include "ops/softmax_attention/dense/causal_cache/k8v4/launch.h"
#include "ops/softmax_attention/dense/causal_cache/nvfp4/launch.h"

namespace {
template <class Run>
int refuses(std::string_view expected, Run&& run) {
    try {
        run();
    } catch (const std::runtime_error& error) {
        if (expected == error.what() &&
            ninfer::test::report_excluded_path("architecture refusal", error.what())) return 0;
        std::cerr << "wrong architecture refusal: " << error.what() << '\n';
        return 1;
    }
    std::cerr << "unsupported route accepted: " << expected << '\n';
    return 1;
}
} // namespace

int main() {
    using namespace ninfer;
    using namespace ninfer::ops::detail;
    int failures = 0;
#if defined(NINFER_SM86) || defined(NINFER_SM89)
    if (test::cuda_unavailable()) return 77;
    const auto execution = test::cuda_environment().default_execution;
    Tensor tensor;
    Weight weight;
    MemoryArena workspace(MemorySpan{});
    failures += refuses("NVFP4 A4 execution requires an sm_120a GPU", [&] {
        launch_nvfp4_a4_quantize(tensor, weight, {}, Nvfp4ScaleLayout::RowMajor, nullptr);
    });
    failures += refuses("NVFP4 KV attention requires an sm_120a GPU", [&] {
        nvfp4_kv_cached_attention(tensor, tensor, 1.0f, {}, {}, workspace, tensor, execution);
    });
    failures += refuses("K8V4 KV attention requires an sm_120a GPU", [&] {
        k8v4_kv_cached_attention(tensor, tensor, 1.0f, {}, {}, workspace, tensor, execution);
    });
    failures += refuses("Block-scaled MoE execution requires an sm_120a GPU", [&] {
        moe_cuda::block_scaled::fp8::launch_input(tensor, {}, {}, execution, false);
    });
    failures += refuses("Block-scaled MoE execution requires an sm_120a GPU", [&] {
        moe_cuda::block_scaled::nvfp4::launch_input(tensor, {}, {}, execution, false);
    });
#if defined(NINFER_SM86)
    failures += refuses("FP8 A8 execution requires an sm_89 or newer GPU", [&] {
        launch_fp8_a8_quantize(tensor, weight, {}, nullptr);
    });
    failures += refuses("FP8 KV attention requires an sm_89 or newer GPU", [&] {
        fp8_kv_cached_attention(tensor, tensor, 1.0f, {}, {}, workspace, tensor, execution);
    });
#endif
#endif
    if (test::report_excluded_path("invalid exclusion", "CUDA launch failed")) ++failures;
    return failures == 0 ? 0 : 1;
}
