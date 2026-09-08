// FP8 e4m3 MMA needs sm_89 or newer tensor cores. On Ampere (sm_86) builds the five FP8 A8
// contraction kernels and the two FP8-KV causal Attention kernels are excluded and these
// launchers stand in for them: FP8 artifacts still load and run through the A16 decode and
// small-T paths under a policy that never admits A8, the KV cache serves BF16 and INT8, and any
// plan that reaches one of these launchers is a policy error, reported rather than silently
// degraded.
#include "ops/attn_input_proj/fp8/fp8_attn_input_plan.h"
#include "ops/gdn_input_proj/fp8/fp8_gdn_input_plan.h"
#include "ops/linear/fp8/fp8_a8_plan.h"
#include "ops/linear_add/fp8/fp8_linear_add_plan.h"
#include "ops/linear_swiglu/fp8/fp8_linear_swiglu_plan.h"
#include "ops/softmax_attention/dense/causal_cache/launch.h"

#include <cstdint>
#include <stdexcept>

namespace ninfer::ops::detail {
namespace {

[[noreturn]] void reject_fp8_a8() {
    throw std::invalid_argument("FP8 A8 execution requires an sm_89 or newer GPU");
}

[[noreturn]] void reject_fp8_kv() {
    throw std::invalid_argument("FP8 KV-cache Attention requires an sm_89 or newer GPU");
}

} // namespace

void launch_fp8_a8_quantize(const Tensor&, const Weight&, Fp8A8Workspace, cudaStream_t) {
    reject_fp8_a8();
}

void launch_fp8_a8(const Tensor&, const Weight&, Tensor&, Fp8A8Workspace, cudaStream_t) {
    reject_fp8_a8();
}

void fp8_attn_input_a8_launch(const Tensor&, const Weight&, Tensor&, Tensor&, Tensor&, Tensor&,
                              Fp8A8Workspace, cudaStream_t) {
    reject_fp8_a8();
}

void fp8_gdn_input_a8_launch(const Tensor&, const Weight&, Tensor&, Tensor&, Fp8A8Workspace,
                             cudaStream_t) {
    reject_fp8_a8();
}

void fp8_linear_add_a8_launch(const Tensor&, const Weight&, Tensor&, WorkspaceArena&,
                              cudaStream_t) {
    reject_fp8_a8();
}

void fp8_linear_swiglu_a8_launch(const Tensor&, const Weight&, Tensor&, WorkspaceArena&,
                                 cudaStream_t) {
    reject_fp8_a8();
}

void causal_attention_small_t_fp8_launch(const Tensor&, const Tensor&, const Tensor&, const Tensor&,
                                         const Tensor&, const Tensor&, float,
                                         PagedKVBatchLayerView, CausalAttentionExecutionEnvelope,
                                         std::int32_t, std::int32_t, Tensor&, Tensor&, Tensor&,
                                         Tensor&, cudaStream_t) {
    reject_fp8_kv();
}

void causal_attention_cached_small_t_fp8_launch(const Tensor&, const Tensor&, float,
                                                const PagedKVLayerView&,
                                                CausalAttentionExecutionEnvelope, Tensor&, Tensor&,
                                                Tensor&, Tensor&, cudaStream_t) {
    reject_fp8_kv();
}

void causal_attention_prompt_fp8_launch(const Tensor&, const Tensor&, const Tensor&, const Tensor&,
                                        const Tensor&, const Tensor&, float, PagedKVBatchLayerView,
                                        Tensor&, cudaStream_t) {
    reject_fp8_kv();
}

void causal_attention_prompt_fp8_attention_launch(const Tensor&, const Tensor&, float,
                                                  const PagedKVLayerView&, Tensor&, cudaStream_t) {
    reject_fp8_kv();
}

} // namespace ninfer::ops::detail
