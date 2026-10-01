# DFlash2 fork spike — measured on the RTX 5090, 2026-09-30/10-01 (EXP-079: no-go as ported; EXP-081: go with upstream's 24-head verify route; EXP-082: two requests on the Q5 tensor cores, kept)

## State and boundaries

- Branch: `spike/dflash2-5090`, based on `e20060b6`, in `~/Development/ninfer-dflash2`.
- Built source: **`7e4d120a025eac2a5898acb0d3a4006801e5fd1c`** for EXP-082; EXP-081 built `28a1abe65f17d8790c26750669ec0014e34e9860` and EXP-079 `905101229dac18c3eabeba115d32873e7c24196f`. This report is committed separately after the built source; it does not alter the executable source.
- Port implementation and build are complete. EXP-079 measured serving, MTP3 parity and speed: MTP3 unchanged, DFlash2 drafting as well as upstream, and a decode round that grew with context. EXP-081 located that cost in the target attention route of one request's 8-token verify and took upstream's route (`4b0eb36c`). EXP-082 put two requests' 16-column verify on the fork's Q5 tensor-core route (`7e4d120a`). No cherry-pick or merge conflicts remain open.
- The branch is pushed to `alphastorm/ninfer` as `spike/dflash2-5090`. The parent checkout and omp-ninfer's sources were not changed by the port; both windows ran on production's GPU under the appliance's hold and dead-man and restored production.
- The build container `ninfer-dflash2-spike-build` holds the incremental build tree; it has no GPU request and is stopped between builds.
- This is a bounded experiment, not a production cutover. No profile changes until the durable integration, the two-request profile and a powered quality screen pass.

## GPU window, 2026-09-30 (EXP-079)

Measured on nyc-pc's RTX 5090 with production stopped from 11:27:38Z to 11:55:49Z. Image
`ninfer-5090:90510122-dflash2-spike` (`sha256:4ad7cf3a…`) is the published v0.6.14 runtime image
with this build's `ninfer` and `ninfer-serve` (`0ed2a048…`); the artifact is the one below.
Receipt: omp-ninfer `docs/measurements/2026-09-30-dflash2-fork-spike-rtx5090.json`. The
pre-registered rule said **no-go as ported**: the corpus gain was -3.6% against a +5% floor.

- GPU tests (logs in the build root's `gpu-tests/`): the DFlash2 real-model test passed at K=7,
  CUDA graph, B=1, BF16 KV with both LM heads (accepted 20/20 each), and 12 of 13 op tests passed.
  `ninfer_kv_cache_append_test` failed 12 cyclic cases on exact FP16 V mismatches. `90510122`
  added `finite_patterned_bf16_bits` but used it in one fixture; the others fed BF16 NaN patterns,
  which the device converts to canonical `0x7fff` while the host oracle keeps the sign (`0x7e00`).
  `139903c4` feeds finite V to all five fixtures, as upstream does; the rebuilt test passed on the
  GPU at the end of the next window (EXP-080, 12:20Z).
- MTP3 preservation: v0.8.7's one-request arguments with the durable store on answered all 89
  role-corpus cases byte-identically to shipped v0.9.0 and v0.8.7, at 238.41 against 237.37 tok/s.
- DFlash2 K=7, one request: 228.90 tok/s on the corpus (-3.6% against shipped), 4.596 tokens per
  round (upstream 4.631), 20.08 ms per round (upstream 18.26; this binary's MTP3 14.02). Behind
  0/32K/64K/120K tokens a code answer decoded at 355.15/126.78/83.74/44.54 tok/s: rounds of
  16.2/45.6/75.0/126.3 ms, about 16 ms plus 0.92 ms per 1,000 context tokens, where upstream's
  DFlash2 rounds rise from 17.7 to 23.0 ms. Prefill matches shipped.
- Two requests with two device state slots: 131,520 KV tokens, as upstream; a pair decoded at
  276.53 tok/s against shipped 388.42 and upstream DFlash2's 453.95.
- Next: a kernel timeline of one DFlash2 round at 64K to find the cost that grows with context.
  Two candidates, neither measured: the target attention for one request's 8-token verify, a shape
  this fork had not run at long context (MTP3 verifies 4 tokens; v0.6.14's 8-row rounds are two
  4-token requests), or a draft-side operation that scales with the prefix rather than the
  2,048-token draft window.

## GPU window, 2026-09-30 evening (EXP-081)

Measured on nyc-pc's RTX 5090 with production stopped from 20:10:07Z to 20:30:10Z; the driver
(`exp081-window.sh`) ran on the appliance itself. Image `ninfer-5090:28a1abe6-dflash2-spike`
(`sha256:c0f3db62…`) is EXP-079's image with this build's `ninfer` and `ninfer-serve`
(`d9fbae37…`). Receipt: omp-ninfer
`docs/measurements/2026-09-30-dflash2-verify-route-rtx5090.json`. The pre-registered rule
(EXP-079's) said **go**: corpus decode +21.4% against a +15% bar, MTP3 89/89 identical, no
DFlash2 request errors.

- Cause: the 27B's full attention has 24 query and 4 KV heads. One request's 7- or 8-column call
  on that geometry resolved to the Prompt kernel (one CTA per query head per 64 query rows, no KV
  split), so all 16 full-attention layers grew with context. `28a1abe6` cherry-picks upstream
  `4b0eb36c`: above 320 visible keys that call takes ChunkedSmallT. The first port skipped it as
  35B retuning; the 35B-A3B has 16 query heads.
- GPU tests: `ninfer_softmax_attention_test` passed (its 24-head width-7 cases at 512 keys now
  take ChunkedSmallT); the DFlash2 real-model test passed at K=7, graph, B=1 with both heads
  (20/20 each).
- DFlash2 K=7, one request: 288.09 tok/s on the corpus (+21.4% over shipped, +13.6% over
  upstream's DFlash2), 4.679 tokens per round, 16.24 ms per round. Behind 0/32K/64K/120K tokens a
  code answer decoded at 367.67/303.18/290.63/222.95 tok/s in rounds of 15.6/18.6/21.3/25.8 ms,
  about 15.7 ms plus 0.085 ms per 1,000 context tokens (upstream 0.044). Prefill matches shipped.
- MTP3: 89/89 byte-identical to shipped v0.9.0 and v0.8.7, 238.17 tok/s. DFlash2 matched shipped
  MTP3 on 86 of 89 cases (EXP-079: 41).
- Two requests with two device state slots: unchanged from EXP-079, a pair at 269.17 tok/s in
  37.1 ms rounds against upstream DFlash2's 453.95 in 22.0-22.4 ms.
- Next: the two-request verify is 16 columns, and the fork's Q5 tensor-core route accepts only 4
  and 8 (`q5_small_t_mma_tokens`). Read from source, at 16 columns the MLP-down, GDN value/z and
  attention gate/value projections fall back to SIMT; upstream has 16-column routes in `aa429f28`
  (GDN input), `f6a658ef` with `9e163eee` (attention input) and `fc62790a` (Q5 linear-add, after
  `385b30ce`). Port those for 16 columns and recheck MTP3 identity, since 16-token prefill pieces
  share the routes. The remaining long-context slope, twice upstream's, is consistent with
  ChunkedSmallT reading the KV once per query chunk (6 and 2 columns) where upstream's
  `a7818988`/`c71795e1` attend 8 columns in one SmallT pass; not measured.

## GPU window, 2026-10-01 (EXP-082)

Measured on nyc-pc's RTX 5090 with production stopped from 01:09:16Z to 01:21:38Z; the driver
(`exp082-window.sh`) ran on the appliance. Image `ninfer-5090:7e4d120a-dflash2-spike`
(`sha256:06ac429f…`, `ninfer-serve` `4a559dda…`). Receipt: omp-ninfer
`docs/measurements/2026-10-01-dflash2-pair-q5-tensor-cores-rtx5090.json`. The pre-registered rule
said **keep**: 9/9 GPU tests, MTP3 89/89 identical, no request errors, pair round 22.7 ms against
a 33.0 ms bar.

- Cause: `q5_small_t_mma_tokens` accepted 4 and 8 columns, so two requests' 16-column verify ran
  the MLP-down (Q5 linear-add, K=17408), GDN value/z and attention gate/value Q5 projections on
  SIMT row kernels. `7e4d120a` lets the kernel take 16 columns (a second m16n8k16 MMA per k16
  step, two pipeline stages inside 48 KiB of static shared memory) and routes 16 columns to it at
  the three call sites. Per-column arithmetic is unchanged; T=4 and T=8 compute what they did.
- GPU tests: linear_add_q5_a16 (16 registered as a region start at K=17408), gdn_input_proj,
  gdn_input_proj_conv_record, gdn_input_proj_conv_snapshot, attn_input_proj, softmax_attention,
  and the DFlash2 real-model test at K=7 with both heads at batch 1 and the full head at batch 2.
- Two requests with two device state slots: a pair at 420.86 tok/s (223.86 and 220.97) in 22.7 ms
  rounds, against 269.17 in 37.1 ms before, upstream DFlash2's 453.95 in 22.0-22.4 ms and shipped
  MTP3's 388.42; a lone request on the same server at 369.64.
- MTP3: 89/89 byte-identical to shipped v0.9.0 and v0.8.7, 238.38 tok/s.
- An Nsight Systems trace of the pair (`/home/sunil/builds/exp082/nsys/`) is kept, not attributed.
- Next: the durable context ring (checkpoints, fanout, restore) so DFlash2 can keep the durable
  store, then the two-request profile with NVFP4 KV and four device state slots, and a powered
  quality screen.

## Artifact

Use `/home/sunil/builds/models/v2-dflash2/hf-dc370fb6/qwen3_8_27b.ninfer` on nyc-pc-wsl.
HF revision: `neroued/Qwen3.8-27B-NInfer@dc370fb6`; 20,437,336,576 bytes; SHA-256
`0634abb07024221de141456cf04a42ab74b18bc38e1b781c6eb2e062a467eec3`.
The manifest requires upstream `385b30ce`. The lead verified that its 1,118 base tensors and
6 frontend resources are byte-identical to pinned `eec39564`, with 66 additional `dflash2/*`
tensors. The CPU smoke here independently bound all 66 companion tensors in both full-head
and optimized-head modes, verified the requested optimized head and ID map were selected, and verified that
the MTP input projection was not selected. No converter changes were made.

## Implemented integration and deliberate hand ports

1. Cherry-picked 51 of the 79 commits in `863aa8a5^..385b30ce` with `-x` for the first port, and a 52nd (`4b0eb36c`) after EXP-079; three already existed in the fork and 24 remain skipped. The complete ledger is below.
2. Additional prerequisite/fix cherry-picks:
- `03177b910e70f783b00c4f980ce0d1896a6b8592` → `50239f857c0f4f8372e52990c0682627b67d003d`: fix(runtime): preserve kv coverage during speculative terminal settlement.
- `9f0575bb2fa97adfe869740c1ae7f137f53e9c17` → `b418f32430fadc0d6353aba2ebf47d6b19d7d5db`: fix(build): include bf16 definitions in q4 topk kernel.
3. `21a0e85f8819edc644a3bc036fca6d05cf52ac6e` was **not** imported wholesale because it changes target KV V to FP16. Hand ports `e84a1a65` and `90510122` carry `Port-of` trailers: cyclic draft V alone is FP16; paged target V stays BF16. The full-context helper supports BF16 and cyclic-context helper supports FP16 without changing the target causal-attention path.
4. `70434721b1ae29d0616f3de9b376c8a4d91590b5` was **not** imported wholesale because it changes ordinary/MTP greedy sampling. `97d10e1c` ports only the explicit accepted-token increment operation. Sparse greedy acceptance ignores penalties and does not publish counts, matching this fork; positive-temperature counts are published only for the Frontend-committed prefix. Existing one-hot kernels and ordering helpers are retained.
5. **4201b5d2 decision: the defect exists in the pre-v3 fork.** The old prefill sink sliced destination slots and KV rows from the last decode ingress. Decode compaction or a retained-state fork could leave those controls stale. `ee25a0b9` ports `4201b5d2d0f6afe235f4ed8e70cd753800770eca`: independent `DFlashPrefillIngress`, pinned host backing, separate local append scratch, and per-chunk current destination-slot / full-KV-row upload. Forced-token prefill and normal chunk prefill both bind their current controls. This is source-confirmed and compiled; its on-device state-transition regression remains for the GPU window.
6. `c99204b4` rejects `--spec dflash2` plus `--session-checkpoint-dir` at startup with a clear error. Direct continuation export returns `DFlash2StateUnsupported` before touching the continuation writer. By code inspection, the resource manager propagates that refusal and the existing checkpoint store removes its staging directory before publication; it may have staged the Response envelope, but no checkpoint generation is published. Durable DFlash2 import/export is deliberately not implemented.
7. `95142b1f` removes the obsolete produced-count-member test assertion. `31a86a08` replaces an upstream-only `Engine::tokenize_text` call with the actual raw token IDs obtained by CPU tokenization of this artifact; no new Engine API is introduced. `78a9b985` records spike limits and removes the superseded September 4 checklist.

## Build and exercised evidence

Build root: `/home/sunil/builds/dflash2-spike`.
The container was based on `nvidia/cuda:13.1.2-devel-ubuntu24.04`, with `--memory 8g --memory-swap 8g`
and `-j8`. Inspection showed both limits equal to 8,589,934,592 bytes and DeviceRequests `null`.
C: had **597,213,163,520 bytes free** at the final check (above the required 64 GiB).
CUDA was only used as a compiler/toolkit. CPU executables used the toolkit's driver stub;
that environment cannot validate GPU execution and must not be reused for a GPU run.

Configuration:

```sh
cmake -S /workspace/src -B /workspace/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=120a -DNINFER_BUILD_APPS=ON -DBUILD_TESTING=ON -DNINFER_BUILD_BENCHMARKS=OFF -DNINFER_BUILD_PROFILE=dflash2-spike-5090 -DNINFER_UPSTREAM_BASE_SHA=6e8b2e2ad5d53597c3ba8e7989f9546d40b921fc -DNINFER_PATCH_STACK_SHA=905101229dac18c3eabeba115d32873e7c24196f
```

- Successful Release/sm_120a build: core/ops/engine libraries, `ninfer`, `ninfer-serve`, and the 14 GPU fixture executables listed below. Final log: `build-final.log`; preceding complete fixture log: `build-gpu-fixtures-2.log`.
- Six CPU tests **passed, zero failed**, recorded in `cpu-tests.log`:
  `ninfer_artifact_reader_test`, `ninfer_checkpoint_io_contract_test`, `ninfer_cli_options_test`,
  `ninfer_qwen3_6_runtime_mechanisms_test`, `ninfer_qwen3_6_state_image_layout_test`,
  `ninfer_serve_options_test`.
- Actual `ninfer-serve --help` and `--version` ran without a GPU. A real executable invocation with
  `/nonexistent.ninfer --spec dflash2 --draft-tokens 7 --session-checkpoint-dir /unused`
  stopped in option validation with `DFlash2 does not support --session-checkpoint-dir; omit it for this backend`.
- A throwaway C++ smoke called the real artifact Reader/Binder and fork tokenizer in a CPU-only
  container with the artifact mounted read-only. Both head modes passed the 66-tensor residency
  checks. `binding-smoke.log` preserves output; the throwaway source/binary have been removed.
- `serve-version.txt` reports exact patch stack `905101229dac18c3eabeba115d32873e7c24196f`, upstream base
  `6e8b2e2ad5d53597c3ba8e7989f9546d40b921fc`, profile `dflash2-spike-5090`, GNU 13.3.0, CUDA 13.1.115, and architecture `120a`.
  **`source_dirty=true` is intentional conservative archive-build metadata:** the source was shipped
  with `git archive` and the build tree has no .git checkout. The binary is not stamped as a clean production build.
- At the CPU-only pause, no GPU numerical oracle, real-engine execution, HTTP generation, or performance comparison had run.
  Tests that require a CUDA device were compiled, not invoked or counted as skipped passes.

Binaries:

| Binary | SHA-256 |
|---|---|
| `/home/sunil/builds/dflash2-spike/build/apps/ninfer-serve` | `0ed2a0485474574a400ac45b99f5ccab7ea9990aea9359696ee374994bfea5d5` |
| `/home/sunil/builds/dflash2-spike/build/apps/ninfer` | `d7f5541467c91b51ad99124cd0d22d52b10c3dc45107a098bd71b267777c2ac3` |

Earlier build failures are retained in `build-stage1.log` through `build-stage5.log` and
`build-gpu-fixtures.log`. They exposed and led to correction of mixed one-hot/sparse kernels,
BF16/FP16 helper deduction, absent NVTX/count APIs, an obsolete prefill assertion, the missing
public tokenizer method, and the finite cyclic-V test helper. The final logs supersede those
failures; none is an open compiler error.

## MTP3 preservation and remaining risk

- Target paged KV V remains BF16. Target GDN, Q5, RMSNorm, embeddings, vocabulary schedules, and
  optional target retunes in the skipped commits were not imported. One target causal-attention
  route was: `28a1abe6` (upstream `4b0eb36c`) sends one-request calls of width 7 or 8 on 24 query
  heads above 320 keys to ChunkedSmallT. MTP3 decode never issues that call (it verifies 4 tokens
  per request); a one-request prefill piece of exactly 7 or 8 tokens can, and EXP-081's corpus
  stayed 89/89 identical.
- A source comparison against `e20060b6` found all six original global kernels in
  `src/ops/kernel/speculative_round.cuh` byte-identical, including the original one-hot partial
  and group-finalize kernels. Sparse acceptance has separate kernels. The original
  `sampling_ordered_float`, sort-key and key-decoding helpers were restored verbatim.
- Required shared source extensions still exist in `src/ops/linear/q4/q4_small_t_mma.cuh`,
  `src/ops/linear/w8/w8_small_t_mma.cuh`, `w8_rowsplit_gemm_mma.cuh`, `w8_small_t.cu`,
  `w8_config.h`, and W8 SwiGLU/attention dispatch headers: top-k epilogues, masked live columns,
  and new DFlash2 shapes/buckets. Existing unmasked/default schedules were retained, but this is
  **not a claim of identical generated SASS or observed MTP3 tokens/performance**.
- The DFlash RoPE helper was shared with the new RMSNorm/RoPE op; ordinary target RoPE constants
  and math remain unchanged. Batched feature scatter and ragged-prefix changes are draft-context
  operations; the ordinary continuation scatter kernel was not replaced.
- `03177b91` necessarily changes shared terminal-settlement bookkeeping in
  `src/targets/qwen3_6/impl/runtime/logical_kv_store.h`, `program.h`, and `program_impl.h`:
  keep physical KV coverage until the final speculative prefix is settled. This also reaches MTP
  terminal cases and requires regression coverage, although it does not retune MTP kernels.
- `round_state`, `layouts_impl.h`, `program_impl.h`, and sampling declarations also add
  backend-specific storage/control branches. No new persistent DFlash2 checkpoint representation
  is claimed. A reserved GPU window must establish target correctness, MTP3 parity, and performance
  before any production promotion.

## Resume safely

The next CPU-only command is:

```sh
ssh nyc-pc-wsl docker start ninfer-dflash2-spike-build
```

To continue compilation, use `docker exec ninfer-dflash2-spike-build cmake --build /workspace/build -j8 --target ...`.
The existing container remains memory-capped and has no GPU request. The auxiliary local image
`ninfer-dflash2-spike-cpu:20260930` was used only for the read-only artifact smoke and is retained.
It is not a published runtime image.

**Do not run the following GPU work while production owns the device.** In a separately authorized
window, use real CUDA driver libraries, not the CPU stub LD_LIBRARY_PATH, and the pinned artifact.
Start with one request and the two head choices:

```sh
NINFER_QWEN3_8_27B_DFLASH2_WEIGHTS=/home/sunil/builds/models/v2-dflash2/hf-dc370fb6/qwen3_8_27b.ninfer \
  build/tests/ninfer_qwen3_8_27b_dflash2_real_test 7 1 0 1 bf16 0 0
NINFER_QWEN3_8_27B_DFLASH2_WEIGHTS=/home/sunil/builds/models/v2-dflash2/hf-dc370fb6/qwen3_8_27b.ninfer \
  build/tests/ninfer_qwen3_8_27b_dflash2_real_test 7 1 1 1 bf16 0 0
```

The real-test arguments are K, graph enabled, optimized head enabled, maximum B, target KV,
Vision enabled, and extra Device StateImage slots. Repeat eager, K=1/15, retained-prefix/fork,
and ring-wrap cases as capacity allows. The dedicated post-v3 4201 stale-control regression was
not ported; add or adapt that exact decode-row/state-fork scenario before treating the fix as
GPU-qualified. The default CTest real-test invocation uses a wider
batch than this spike's one-request target.

Server flags for that authorized window are `--spec dflash2 --draft-tokens 7`, optionally
`--lm-head-draft`, with `--max-concurrency 1` and **no** `--session-checkpoint-dir`.
Use explicit bounded context/KV capacity appropriate to the reserved GPU; do not reuse the
production checkpoint/window directories. An HTTP generation smoke and role-corpus comparison
against shipped MTP3 still remain.

Exact CTest names of the **14 compiled GPU fixture executables** (none run):

- `ninfer_qwen3_8_27b_dflash2_real_test`
- `ninfer_qwen3_6_35b_a3b_dflash_real_test`
- `ninfer_context_kv_materialize_test`
- `ninfer_candidate_selector_test`
- `ninfer_linear_topk_test`
- `ninfer_sliding_window_attention_test`
- `ninfer_kv_cache_append_test`
- `ninfer_speculative_round_test`
- `ninfer_dynamic_grouped_conv_prepare_test`
- `ninfer_linear_dynamic_grouped_conv_add_test`
- `ninfer_rmsnorm_pack_tail_test`
- `ninfer_rmsnorm_rope_test`
- `ninfer_prepare_ragged_prefix_test`
- `ninfer_scatter_bf16_batch_test`

Additional exact registered regression names for the GPU window (not claimed run):

- `ninfer_qwen3_6_context_store_test`
- `ninfer_qwen3_6_27b_prefix_real_test`
- `ninfer_attn_input_proj_test`
- `ninfer_linear_swiglu_w8_a16_test`
- `ninfer_gated_delta_net_replay_record_test`
- `ninfer_gdn_replay_fold_test`
- `ninfer_gated_rmsnorm_test`
- `ninfer_argmax_test`
- `ninfer_rope_test`
- `ninfer_sigmoid_mul_test`
- `ninfer_position_test`
- `ninfer_prepare_masked_block_test`
- `ninfer_softmax_attention_test`
- `ninfer_scatter_test`

## Conflict and adaptation ledger

- **bench/CMakeLists.txt** — Retain the fork output-stage benchmark and add the new top-k benchmark.
- **bench/README.md; bench/targets/qwen3_6_27b/ninfer_bench_support.cpp** — Merge DFlash2/spec/draft-count help while preserving fork KV choices and benchmark controls.
- **bench/ops/gdn_replay_bench.cu** — Keep the fork CommitSelection::Exp003 destination behavior; add upstream width qualification.
- **bench/ops/kv_cache_append_bench.cu** — Keep the fork paged-cache benchmark, excluding inherited NVFP4/K8V4 changes; only cyclic V becomes FP16.
- **docs/cli.md; docs/serving.md** — Preserve fork checkpoint/logging surfaces and KV names; add DFlash2 and explicit spike/checkpoint limitations.
- **docs/maintainer/2026-09-04-qwen3.8-27b-dflash2-algorithm.md** — Carry the upstream algorithm text through its later rename to qwen3.8-27b-dflash2.md; mark fork qualification pending.
- **docs/maintainer/2026-09-04-qwen3.8-27b-dflash2-support-plan.md** — Carry intermediate historical patches, then accept deletion by 385b30ce.
- **docs/maintainer/2026-09-05-qwen3.8-27b-dflash2-op-checklist.md** — Resolve interleaved progress conflicts without importing skipped target retunes; retain the operator reference. The superseded September 4 checklist is removed. Upstream qualification text is not evidence for this fork.
- **docs/maintainer/qwen3.8-27b-artifact.md; model-cards/Qwen3.8-27B-NInfer/README.md; model-cards/Qwen3.8-27B-nvfp4-NInfer/README.md** — Add the companion inventory and backend selection; retain fork KV-format restrictions.
- **model-cards/Qwen3.6-35B-A3B-NInfer/README.md** — Retain bf16/int8/fp8 KV names and add DFlash+Vision support.
- **src/ops/kernel/speculative_round.cuh** — Restore all original e20060b6 one-hot/MTP kernels verbatim; put variable-width sparse acceptance in separate new kernels. Sparse greedy semantics remain those of the fork.
- **src/ops/kv_cache/append/kernel.cuh; tests/ops/test_kv_cache_append.cpp** — Keep paged BF16 V and its exact-copy oracle. Port FP16 conversion only for cyclic draft V, capacities, and cyclic fixtures; add the finite-input helper required by those fixtures.
- **src/ops/linear/q4/q4_small_t_mma.cuh** — Add masked live-column/top-k support using the fork st.activations buffer, retaining the unmasked path and fork double buffering.
- **src/ops/softmax_attention/common/context_query.cuh** — Retain the upstream draft tuning, but make V arithmetic type-directed: BF16 for legacy full context, FP16 for cyclic draft context.
- **src/ops/softmax_attention/sliding_window/launch.cu; tests/ops/test_sliding_window_attention.cpp** — Take the consolidated variable-width/2048-window draft route and matching independent oracles.
- **src/targets/qwen3_6/impl/runtime/dflash_impl.h** — Merge coherent selector/proposal_q/candidate_ids and DFlash2 forward; retain fork target wrappers and omit unsupported upstream NVTX names.
- **src/targets/qwen3_6/impl/runtime/layouts_impl.h** — Make full draft KV allocation conditional on full_layers != 0, while retaining the fork paged BF16 storage geometry for existing backends.
- **src/targets/qwen3_6/impl/runtime/program_impl.h** — Merge DFlash2 orchestration and terminal coverage without importing unrelated causal_score or unavailable capture-planner fields. Add isolated prefill ingress and explicit checkpoint refusal.
- **tests/CMakeLists.txt** — Add only the required DFlash2 operations and real-engine target; do not import unrelated NVFP4/K8V4/scoring targets.
- **tests/README.md** — Keep fork host-only and protocol-suite instructions; add DFlash2 real-test and terminal-page-boundary coverage.
- **tests/ops/linear/linear_test_common.cpp** — Keep the fork excluded-path handling and add the upstream graph/device includes needed by the DFlash2 feature projection cases.
- **tests/ops/linear_swiglu/linear_swiglu_test_common.cpp** — Keep native-lane exclusions inside the new test lambda (return, not continue), preserve labels, and omit a duplicated workspace assertion.
- **tests/ops/test_attn_input_proj.cpp** — Keep original cases and exclusions under the non-DFlash2 mode, and add the new DFlash2-specific width selector.
- **tests/targets/qwen3_6/test_context_store.cpp** — Keep the terminal-settlement KV coverage regression, excluding unrelated mixed-snapshot tests for a different store API.
- **tests/test_cli_options.cpp** — Keep fork-only option contracts, add Vision+DFlash and DFlash2 K=1,2,7,15 acceptance / 0,16 rejection, and exclude unrelated incoming options.

## Complete upstream interval classification

Every taken row has a `cherry picked from commit` trailer. Skipped commits were intentionally
not cherry-picked. Existing rows cite their already-present fork commits.

| Upstream commit | Subject | Disposition / fork commit | Reason |
|---|---|---|---|
| `863aa8a5f1e866db74f29f8999b83b4021398dee` | fix(dflash): allow vision prompts | taken → `eb00c8deb2aed64de0782068aa921b1bd1e9a257` | DFlash2 implementation, qualification, or required integration. |
| `ad0f3d384b5cbcec4a48a3951c287b4e9831443e` | chore: add project funding information | skipped | Funding metadata, unrelated to runtime. |
| `4df5e0b42bf4a1d30ab538b8d86eae841fa5f111` | feat(converter): add qwen3.8 dflash2 weights | skipped | Converter work unnecessary for the supplied complete v2 artifact. |
| `917d4c89eceabac6de9e00184133f4039b33bed2` | docs(maintainer): record architecture and dflash2 plans | skipped | Historical design plans; retained active DFlash2 documentation instead. |
| `377f71a11bed096f6a5ed24715a765876f31dfd5` | perf(ops): add dflash2 w8 feature projection | taken → `a3e9c2427c285859917ebfbd25c081490a0ee14d` | DFlash2 implementation, qualification, or required integration. |
| `426d129fe6adb5bdad2cdeaee1e5487280ceb3be` | perf(ops): add dflash2 selector projection | taken → `21d25841af299d1d413d4ee80aec2dd92b6e1104` | DFlash2 implementation, qualification, or required integration. |
| `efd012fd427097d56f4e705cc618b695a305b9d6` | feat(ops): add dflash2 tail rmsnorm | taken → `09f5a802eaa4be9f881975a1bf9dc9708b2fb6f8` | DFlash2 implementation, qualification, or required integration. |
| `5499799dc512c9fb87da6059fd2bfd60f785b989` | feat(ops): add fused linear top-k | taken → `e9afb6ecff752bbd729bc9c5b551bb26c4ed46f2` | DFlash2 implementation, qualification, or required integration. |
| `0af0b79a56cabfdb0eda4c68440d7e5a31dcf2ff` | feat(ops): add dynamic grouped conv prepare | taken → `8a0e04500c2f231bc647ca13907d486fa3d241f1` | DFlash2 implementation, qualification, or required integration. |
| `221baf55836b9b24943b449835e73d5d798ed628` | perf(ops): add dflash2 w8 attention projection | taken → `6c85114ab7241b9b0c7273f4a49b208eca2d175c` | DFlash2 implementation, qualification, or required integration. |
| `95037fe4d2b7be69929f26b7bc48b8956d6c8ed4` | perf(ops): add dflash2 w8 linear swiglu | taken → `72eaa6399d1940ac8112def1dfa581a9400dd6e6` | DFlash2 implementation, qualification, or required integration. |
| `3ba9d4a6cb01e3090d4689ce80a98296fbc4003a` | feat(ops): support 2048 cyclic kv append | taken → `8e23993c6b918cea39c626439b3a26e61de86f8d` | DFlash2 implementation, qualification, or required integration. |
| `d51a07f3a69ea3abb988b9409ebde65e437ecaa0` | feat(ops): add fused dflash2 rmsnorm rope | taken → `65eff0e9cb76b632f80981bdca80853e736729c3` | DFlash2 implementation, qualification, or required integration. |
| `67a90a5c51f3bc39d53e87eba08be3f3224e4ac0` | perf(ops): tune k5120 linear pair routes | skipped | Optional linear-pair retuning; existing route suffices; preserve fork schedules. |
| `45f9b4d05480886bbdebee10c7936b2781843f6a` | feat(ops): add dflash2 context kv materialization | taken → `23b2d667e8718e90266fbd82d89466d6284ff0fe` | DFlash2 implementation, qualification, or required integration. |
| `9f61a0ca37d8261e991e9569df1b687dc4f622c9` | feat(ops): add 2048 sliding window attention | taken → `bc16a4209fada7fdb2e1a0bcd89170c34829f44f` | DFlash2 implementation, qualification, or required integration. |
| `d01e6a09cf7a8f2c9f569b644d3c3f70a38f7e04` | feat(ops): add dflash2 speculative acceptance | taken → `a39a40afd36451defa5796c743cb00c65156018c` | DFlash2 implementation, qualification, or required integration. |
| `1efaa84679c9ac312e75e7c59292f73eda473048` | docs(maintainer): consolidate dflash2 op progress | skipped | Historical checklist progress only. |
| `050b21382783e2be0642ae9cdde77229147a70b5` | feat(ops): add dflash2 dynamic grouped conv finish | taken → `a1bfa5cd18e8b56b6ed2b081abf7a399325d7d90` | DFlash2 implementation, qualification, or required integration. |
| `3447146d2ff25f56be68d3b62e636148e8d2a68a` | feat(ops): add dflash2 candidate selector | taken → `f32946471ab567c99cdaaeeed0aa0c042e3e56c6` | DFlash2 implementation, qualification, or required integration. |
| `6d1da9cef7d3d596730478c5ddd02b0ae6f82245` | perf(ops): remove q5 linear add aggregate cliff | skipped | Optional Q5 retuning; preserve fork target schedules. |
| `4b0eb36ccd29dc1b96c6a02c77570f2937eb2c9b` | perf(ops): route h24 verify attention through small t | taken → `28a1abe65f17d8790c26750669ec0014e34e9860` | First skipped as 35B retuning; 24 query heads is the 27B target (the 35B-A3B has 16). Taken after EXP-079: the 27B's one-request DFlash2 verify ran on the Prompt kernel, whose cost grew with context (EXP-081). |
| `1c8f8acc7f315fd020091c88a388d43d7884dbc2` | perf(ops): fuse nvfp4 swiglu through t96 | skipped | NVFP4 SwiGLU optimization; not a groupwise artifact dependency. |
| `aa429f28b4de0c0872c25fab50b31f6afe70b428` | perf(ops): route gdn input t16 through grouped mma | skipped | Optional GDN retuning; preserve fork MTP replay schedules. |
| `22d8a1d3600bbc7ca385f045f5d709a5b8796875` | perf(ops): optimize w8 vocabulary t64 route | already present → `e5551e442e0a3a0cea704106b36cb55403516c22` | Existing fork provenance; do not duplicate. |
| `3b77ea12d42490d338781e9ce5debb2655f6a1d0` | feat(qwen3.8): bind optional dflash2 weights | taken → `f358ef90c070419379047c3998c82fad5c9aef34` | DFlash2 implementation, qualification, or required integration. |
| `3be58d8ac1c748bbb1e1ee804894e411028626bf` | docs(dflash2): correct draft width contract and replace op checklist | skipped | Historical documentation/checklist cleanup. |
| `96a83127b06a75fa3bd1053e7bcf83c91d6d914b` | perf(ops): qualify variable-width dynamic conv prepare | taken → `95b39c7d589276757d050809aa7fbbd44e67af91` | DFlash2 implementation, qualification, or required integration. |
| `99627b307dbe2051ff116f25f4e81c4afeb5971b` | perf(ops): extend and tune variable-width dynamic conv add | taken → `1328c47317ae545ded26a0ced1d98ed00da3ce36` | DFlash2 implementation, qualification, or required integration. |
| `b1dbbcfe5b250bf8c6d9541ad947a68c5bfb9559` | perf(ops): widen rmsnorm rope and parallelize head groups | taken → `c607dd93fbfdbaaddf6cacb1e32d432729bf380c` | DFlash2 implementation, qualification, or required integration. |
| `844333eae5310a886a1fc63c97491e59fb17f3d3` | fix(ops): support variable-width rmsnorm tail packing | taken → `5e6c4f5e8092c977a035cc3cd444b76baeb4b842` | DFlash2 implementation, qualification, or required integration. |
| `ea360239d89306731a08a3a3c90639c9ca64bc0e` | docs(agents): streamline guidance and consolidate project contracts | skipped | Unrelated contributor/architecture document rewrite. |
| `e4e81015cbf98b56c2fa76bacd62e3ae14def4aa` | perf(ops): qualify variable-column projection top-k | taken → `9a6a4fff4ac3b6af820ecac74def775aa47f0085` | DFlash2 implementation, qualification, or required integration. |
| `17e48a443c09f4a06e6880362d35a6d3a650a283` | perf(ops): qualify variable-length candidate selector paths | taken → `958f3ed178254884776d8ec7f3ae42b9a52b5594` | DFlash2 implementation, qualification, or required integration. |
| `07b9f20aa290a4120d6ee2ff569728299f0ae394` | perf(ops): qualify variable-width context kv materialization | taken → `2c7b7ed07a9a93e550ed202909d49b6c24ea9384` | DFlash2 implementation, qualification, or required integration. |
| `5bde5ed53921dc2c17df9c9b220bbb26440c3785` | docs(architecture): define model weight and execution contracts | skipped | Unrelated architecture documentation. |
| `5323fad5b24f3cf86e6fc0e7b6f52bd02065e226` | perf(ops): parallelize variable-length sparse draft acceptance | taken → `993a8593faa8d2111a1b0bb64e7783a2e96e744c` | DFlash2 implementation, qualification, or required integration. |
| `74f83878cdff12501fe565f3489516e6b7895a47` | perf(ops): tune variable-column dflash2 feature projection | taken → `04f04b9f603f685e2eedfa7ab31457ce4d3712e2` | DFlash2 implementation, qualification, or required integration. |
| `fd72759a8dbc3137ce6e9205e7cb1b71e8485a1e` | perf(ops): parallelize variable-column selector projection | taken → `ff79bb4518c21c21d2e9705c036b843ca772ad22` | DFlash2 implementation, qualification, or required integration. |
| `1539a6e0ba1a1b3dc2992e16e9149dc3b25b707b` | perf(ops): qualify and tune variable-column draft qkv projection | taken → `2f1b9e523c40abdf54d02345394352a8aa9afadb` | DFlash2 implementation, qualification, or required integration. |
| `b3316bad471f245203a261b739100bd87248adcb` | perf(ops): qualify and retune variable-column draft swiglu | taken → `cb70c921f0afeced70b8aedf1e14116406cb26ad` | DFlash2 implementation, qualification, or required integration. |
| `47f9d1216a48b5e347e7f92e15fafe724adc9e99` | perf(ops): qualify and tune variable-width sliding attention | taken → `f2a83f393011be3d1e02d585b1c15e9b46f97b07` | DFlash2 implementation, qualification, or required integration. |
| `eef7467d85f1fe7715d5779b225269e4dc532330` | perf(ops): qualify and parallelize cyclic prefix append | taken → `28b2187c652e8c15437c21be4aa544e0c426d6d3` | DFlash2 implementation, qualification, or required integration. |
| `0ded3d6e06bfad40a3944af18f0a9448a32361a9` | docs(ops): close dflash2 linear pair dependency review | skipped | Historical linear-pair review notes. |
| `113b9a4939fd44b2bde92ad8848419caed8342f2` | perf(ops): parallelize ragged feature prefix preparation | taken → `0b88fd17d5a52ee9425b7f93e0347567c19028d9` | DFlash2 implementation, qualification, or required integration. |
| `55ca1d777c908a650c3d5779001509aa55c192d2` | perf(ops): qualify and vectorize dflash2 embedding gathers | skipped | Optional embedding vectorization; existing implementation supports required widths. |
| `a84a066fec834abd1d47ffa1f97a41bf34cfc4ce` | perf(ops): qualify and tune dflash2 rmsnorm row geometries | skipped | Optional RMSNorm retuning; preserve target arithmetic/schedules. |
| `8e20a5a25d06966d514b3bc5ad148806523cb263` | perf(ops): qualify and tile batched feature capture | taken → `002a17b54619958041d2802cbae7e4edfd956176` | DFlash2 implementation, qualification, or required integration. |
| `f6a658ef2e8f0487637012dff331a09a65307ee7` | perf(ops): retune variable-width target attention projections | skipped | Optional target attention-input retuning; fork routes cover required widths. |
| `a7818988a56dbde406c3d284e1497d93eb6d7fc0` | perf(ops): qualify variable-width causal cache attention | skipped | Optional causal-attention retuning; preserve target KV/attention. |
| `02be37cbc7395b8b176adee2cf33436d358e57d0` | perf(ops): fuse and qualify variable-width gdn norm control | skipped | Optional GDN norm fusion; preserve fork target path. |
| `8b3fefbc0647f060c41609dece8c6aad5c2eb037` | docs(ops): define contract-specific dflash2 acceptance and cleanup | skipped | Historical operator-cleanup requirements. |
| `99b6084e0d59206f038b06a3a6b739c2aaa90a1e` | refactor(ops): bucket dynamic conv prepare widths | taken → `13a5e7fbffb76b4fe0286772f1ec1104ea095c0f` | DFlash2 implementation, qualification, or required integration. |
| `012d5be28b9aeb3cae83f6b75a0d1879c217ce94` | refactor(ops): replace dynamic conv exact-token projections with tiles | taken → `38b81b1159c413aae879bc99f8c9e329104efd96` | DFlash2 implementation, qualification, or required integration. |
| `00f020554a99a0e6de783ff45716fe4a4b0659dd` | refactor(ops): bucket feature projection live columns | taken → `71d716eec837a3e3ae874057719ad74c1b8f8a4b` | DFlash2 implementation, qualification, or required integration. |
| `647c9c9095e96b5c690657d2101293f3d186dacc` | refactor(ops): use one target head rmsnorm row layout | skipped | Optional target head-norm layout retuning; preserve fork target path. |
| `edb2aabf163d666bf505d59d1a8c0e83e6992a5c` | refactor(ops): bucket draft swiglu columns and mask pair outputs | taken → `2001a3d66517d60b3d696b02b83d9485df5d152a` | DFlash2 implementation, qualification, or required integration. |
| `a1b9396c902c76c4a354331be57ca9f338da242d` | refactor(ops): bound draft qkv specialization to the latency regime | taken → `29b2c0eba79baccaf62f2e4d0aec5022fe27bb05` | DFlash2 implementation, qualification, or required integration. |
| `f5ae87909b15f778227f683b91cba8460c2bd8ae` | docs(ops): close context materialization route review | skipped | Historical materializer-route review notes. |
| `a20789e7ebd2d69c9e9f75b982ec88919a7f9217` | refactor(ops): bucket top-k producer columns across head formats | taken → `7c8cc3637f2abd12dcfa4f0c01de77d4785341a3` | DFlash2 implementation, qualification, or required integration. |
| `d5aaa202f5d7af209d7ab7e3b50c6d6e0d800232` | refactor(ops): bucket target fp8 attention projection columns | already present → `cbd27cd70c18f6e3442e59e38567cccc9dbb7b36` | Existing fork provenance; do not duplicate. |
| `5a5dbfb6c47eb2bebf5acbd858e84ba3f4c84cb1` | refactor(ops): share sliding attention kernels across window sizes | taken → `2d69a2b93f826efa369065b133594fbc9a2cedce` | DFlash2 implementation, qualification, or required integration. |
| `c71795e1fd5c8bd3fb9bd5e8029aeacd6f8d682d` | refactor(ops): consolidate causal attention prompt thresholds | skipped | Optional causal prompt threshold refactor; preserve fork target scheduling. |
| `d139d3388c350981dde61b33126ed93eb11a72c2` | fix(ops): preserve gdn record snapshot bits and batch fp8 projection | already present → `06ea96f04f33425c9ffb8cb389d74e198cdf2e03` | Existing fork provenance; do not duplicate. |
| `a07a901783d4d603aa4d3cd5d9ccd74f7b4bc994` | test(ops): qualify gdn replay records across draft widths | taken → `f2171111225ade661a281279afae9ad861de0e41` | DFlash2 implementation, qualification, or required integration. |
| `c29c9583cd3b506bb9c721607dab83e602d0d026` | test(ops): qualify target gated rmsnorm through 128 columns | taken → `04e78c3f1edb8deaae9936320d68ea2ef64fb9e6` | DFlash2 implementation, qualification, or required integration. |
| `0f0899c9c2e8e05c8afc0f0cd2f92b99d22d7297` | perf(ops): extend fused target swiglu and bound q4 column templates | skipped | Optional target SwiGLU/Q4 retuning; existing routes cover required widths. |
| `73e5732d9aab2fb760991306e6dd0ed6b226f0df` | refactor(ops): simplify fp8 residual schedules and qualify target widths | skipped | Optional FP8 residual schedules; not needed by the pinned groupwise profile. |
| `37242d3a70f2e3f5327771c8c1365615eb914046` | perf(ops): bound vocabulary templates and use c96 for target head | skipped | Optional target vocabulary projection schedule; existing schedule also supports new top-k producer. |
| `5a6a54d9d9bab36a7416f17b00450e62e1475824` | test(ops): qualify target argmax masking and replay | taken → `ccd90f2d733b8a92b31eaa2dd842c4560185de65` | DFlash2 implementation, qualification, or required integration. |
| `898603fd43f53b1005a0b9cf7912c7a752b9d9e8` | test(ops): qualify target partial rope across speculative lanes | taken → `167c336e734589ac9278b391a30d36ec45eb1843` | DFlash2 implementation, qualification, or required integration. |
| `5c6dad35ed0d2c3fa35118cfa4741c1ad2945c65` | test(ops): qualify target sigmoid gating through 128 columns | taken → `6ff819d95ac3ca0677b9718b18d63b791adf472c` | DFlash2 implementation, qualification, or required integration. |
| `d1d28c67334d4cc6df4566bfd1004fd55c835df1` | test(ops): qualify per-lane position offsets and graph updates | taken → `1eb55f613d4dfd799ac73d7478db233340f9729b` | DFlash2 implementation, qualification, or required integration. |
| `e81b0924349b0c9e3de68158a71f645d1992d2ac` | test(ops): qualify dflash2 masked blocks with ragged lanes | taken → `a3795d827a8d9445a370bd0fe93f323b558938e1` | DFlash2 implementation, qualification, or required integration. |
| `fb8cea32aa03819b56b76eed1b8da543cff7ece7` | test(ops): qualify verify input preparation for every draft prefix | taken → `d7949a77b1627f25ee8966df82fedf880f08b04a` | DFlash2 implementation, qualification, or required integration. |
| `81b2d050f803bf4d29588e23d9c53f0183aff361` | test(ops): qualify committed hidden selection for each prefix | taken → `d49bff38cb90dc660791c8678b43999097461094` | DFlash2 implementation, qualification, or required integration. |
| `f2faab70a2478c4ca2fc2e4e63e7c654a087a9c7` | test(ops): qualify continuation gather and scatter composition | taken → `60e4d8e1dec7091960929941b4f27beba03551cc` | DFlash2 implementation, qualification, or required integration. |
| `f9a3b1e799b47c8981424c74e4e2d3bc68467731` | test(ops): qualify replay fold against full-block snapshots | taken → `dae106804f4ccf0ad51570fe918d93e2b4390d46` | DFlash2 implementation, qualification, or required integration. |
| `385b30ce1757bafe5a82680e9b5aeb940b14eec1` | feat(engine): integrate dflash2 with configurable draft counts | taken → `241b219155f00c2a7769e3432399d496805ac4a5` | DFlash2 implementation, qualification, or required integration. |
