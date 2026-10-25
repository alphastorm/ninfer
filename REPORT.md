# EXP-097: final KernelRoutes G2 result

## Verdict and frozen identity

**G2 PASS under the founder's amended shipping criterion.** Final candidate
`c2dbcbae4bbdd07f73c8aad299fa0552ba17dbf3` decodes the 84-case role corpus at
**342.150 tok/s**, versus contemporaneous shipped **338.448 tok/s**:
**101.0939% of shipped**. It also clears the original 98% median requirement.
The original combined median-plus-p95 gate would still fail: structured-output
p95 is **12.56% higher**. The founder explicitly made attributed p95 regressions
non-blocking and lowered the shipping median floor to 96%; both criteria and the
amendment are preserved in `bench/measurements/exp097/g2-protocol.json`.

The full priority-1 A/B/B/A completed in 2,514 lease seconds: **336/336 role
requests and 48/48 supplementary depth requests returned HTTP 200**. No second
pass is required: 1.01093856 is outside the original near-boundary interval
[0.96, 1.00]. No additional optimization was pursued after the amended gate passed.

- Candidate binary SHA256: `2f8498d9afa8b2d447a05b610cf3a9a50973c3436c18d20190224946c22f1c5b`.
- Shipped source: `1302d639`; image: `sha256:6a02feba4163d992cc6a46baf28e0ece2ffe6a2ead91939e566c1b3080f5bc02`.
- Build/runtime environment: `ov-build-env:master-64d10fbf-r2`, image `sha256:e7f34ca2f41ecea14da5cce0b2a30cae6b0877e3b3eb371bd92d103bca1121f1`.
- V2 model: `0634abb07024221de141456cf04a42ab74b18bc38e1b781c6eb2e062a467eec3`; byte-preserving v3 upgrade: `facba3bd318139c50899d4a0d74576837f73f0f498a461961467d56df10f4944`.
- This is **not** the published `81f924d4` v3 model. The matched-artifact proof preserves all 1,184 tensor payloads / 20,424,296,864 tensor bytes and the complete original payload range. This does not imply identical frontend rendering or full-model arithmetic.

No production container, release tag, published image, native schedule, Program
round structure or engine scheduler was changed. This result is a performance
gate, not integrated durability, quality/leak, native-Windows or release acceptance.

## Full A/B/B/A measurements

DFlash2 K=7; BF16 KV; max context and fixed KV capacity 131,072; prefill chunk
1,024; max concurrency two. The original 84 wire requests, frozen corpus and
EXP-097 runner were unchanged. Roles run serially; the supplementary protocol
also issues concurrent pairs. Low reasoning, temperature zero, no added
reasoning headroom; reasoning-only completions remain included.

| Arm | Role cases | Median decode tok/s | Decode ms/round |
|---|---:|---:|---:|
| A1 shipped | 84 | 338.366 | 15.5133 |
| B1 candidate | 84 | 342.260 | 15.4093 |
| B2 candidate | 84 | 342.040 | 15.4094 |
| A2 shipped | 84 | 338.530 | 15.5354 |

The aggregate takes each case's median across its two runtime repetitions,
then the median across **84 role cases only**. The 12 depth requests per arm
are excluded. Workload p95 is nearest-rank p95 of per-case median server decode
milliseconds/completion token, not a distribution of repeated individual-token
latencies. The four raw content-safe arm receipts and `G2-final-summary.json`
retain every case, acceptance count, request/output hash and identity.

| Workload | Cases | Shipped p95 ms/token | Candidate p95 ms/token | Ratio | Original 1.10 bound |
|---|---:|---:|---:|---:|---|
| Log triage | 24 | 4.51013 | 4.38985 | 0.97333 | pass |
| Regression contract | 8 | 4.14088 | 4.46495 | 1.07826 | pass |
| Repository evidence | 30 | 3.37177 | 3.33715 | 0.98973 | pass |
| Structured output | 2 | 2.78123 | 3.13055 | 1.12560 | attributed, non-blocking |
| Work-log extraction | 20 | 3.31892 | 3.44222 | 1.03715 | pass |

The previous `9b391d51` candidate achieved 309.651 tok/s / 91.46% of the earlier
338.573 shipped baseline. Its historical receipts remain separate; they are not
mixed into the final interleaved aggregate.

## Attribution first: one valid Nsight capture

`profile-attribution.json` contains the per-round timeline, every kernel name,
component sums, host-gap accounting, request receipts and hashes of the private
Nsight artifacts. Nsight Systems 2026.1.3 traced CUDA/NVTX/OSRT and graph nodes
for shipped versus the previous `9b391d51` candidate at **32,047 prompt tokens**
(log-009 plus the frozen depth fixture), after a 128-token warmup. The measured
request requested 1,536 tokens. A preliminary setup failed before capture
because shipped lacks `/tokenize`; it is not a measured arm.

GPU kernel milliseconds per speculative round:

| Component | Shipped | Previous candidate | Delta |
|---|---:|---:|---:|
| Q5 residual projections | 3.60997 | 4.53384 | +0.92387 |
| Q4 fused SwiGLU | 4.03831 | 4.62189 | +0.58358 |
| Q5 input projections | 1.75456 | 2.02420 | +0.26963 |
| Draft shortlist/top-k | 0.25345 | 0.28149 | +0.02804 |
| Target output head | 0.80215 | 0.80330 | +0.00115 |
| Sampling | 0.01639 | 0.01680 | +0.00041 |
| Draft selector | 0.00564 | 0.00587 | +0.00023 |
| Draft Q8 projections | 1.16182 | 1.16202 | +0.00020 |
| Draft attention context | 0.15939 | 0.15945 | +0.00006 |
| Draft fused norm/RoPE | 0.01147 | 0.01129 | -0.00019 |
| Draft dynamic convolution | 0.13055 | 0.12990 | -0.00064 |
| Target RoPE | 0.05144 | 0.05050 | -0.00095 |
| Other kernels | 0.09538 | 0.09430 | -0.00108 |
| Q4 input projections | 0.83577 | 0.79539 | -0.04038 |
| GDN | 0.98212 | 0.91851 | -0.06360 |
| BF16 attention verify plus merge | 1.82869 | 1.63772 | -0.19097 |
| Other normalization | 0.50920 | 0.28938 | -0.21982 |
| **Total GPU kernel time** | **16.24630** | **17.53585** | **+1.28955** |

Between-round idle was **0.35756/0.37113 ms**, of which only
**0.01929/0.02075 ms** overlapped `engine.commit_output`. Within-round non-kernel
gaps were 0.19448/0.18099 ms. Graph-launch API duration was 0.16387/0.20150 ms
per round, with one/two launches respectively. API time overlaps GPU execution
and must not be added to kernel time. The second graph's four tiny finish kernels
do not explain the gap. Profiler overhead makes these diagnostic timings, not
the release throughput measurement.

Attention was already faster; a BF16 h24 fixed-width transplant was therefore
not justified. Upstream already had the relevant draft operations and fp16 draft
V. No Program/Engine changes were requested from Persistence or EngineCache.
The measured projection contributors, not attention or host idle, drove this port.

## Retained route table and bounded experiments

All new dispatches exclude `NINFER_SM86` and `NINFER_SM89`. Neighboring extents
retain their previous selectors. Q5's new shared-row padding and rolled-loop
parameters default to the previous behavior for every other instantiation.
The separate materialized GDN path is included; it does not go through the
ordinary split dispatcher.

The table uses CUDA Graph operator medians in microseconds, 10 warmups and
61 samples. Each family has its own bounded before/after role comparison and
numerical receipt; these are not substituted for the final ABBA measurement.

| Route | Extent | Before | Retained candidate |
|---|---|---:|---:|
| Q5 residual K=6,144 | T=8 / 16 | 21.792 / 25.824 | 19.680 / 25.792 |
| Q5 residual K=17,408 | T=8 / 16 | 52.480 / 58.624 | 42.240 / 56.576 |
| Q4 fused SwiGLU | T=8 / 16 | 70.912 / 93.408 | 62.720 / 91.488 |
| Q4 shortlist | U=7 / 14 | 277.728 / 357.664 | 249.056 / 333.088 |
| Attention Q4/Q5 input split | T=8 / 16 | 38.080 / 48.352 | 36.064 / 44.288 |
| GDN Q4/Q5 input split | T=8 / 16 | 46.336 / 60.640 | 40.224 / 54.528 |
| GDN snapshot | B1/W8 / B2/W8 | 50.432 / 64.800 | 46.304 / 58.656 |
| GDN record | B1/W8 / B2/W8 | 50.400 / 64.736 | 44.320 / 58.592 |

Q5 residual and input projections use eight K warps, the shipped linear
reduction order, 16-byte padding in both shared code/high planes, three stages
at T=8 and two at T=16, with a rolled K loop. Q4 SwiGLU and U=7/14 shortlist
reuse the existing padded pairwise schedules, two stages at tile eight and one
at tile sixteen. The prior Q4 input routes remain unchanged.

The first Q5 residual experiment (`9242f521`) restored eight-warps arithmetic
without the padding/rolled pipeline: it passed numerics but regressed K=17,408/T8
from 52.480 to 59.552 microseconds. It was superseded by `740329be`, not retained
as a standalone performance improvement. `q5-iteration1.json` preserves this
negative result. Subsequent receipts are `swiglu-iteration.json`,
`q5-padded-iteration.json`, `topk-iteration.json` and `input-iteration.json`.

## Acceptance and the two originally failing tails

**Acceptance is not exactly equal between runtimes.** In the profiled request,
shipped accepted 1,195/2,376 drafts versus candidate 1,198/2,353: the candidate's
slightly better acceptance did not cause its slower round. Even the identical
128-token warmup output used different round counts. Restoring shortlist FP32
identity did not change the eight original bounded cases' acceptance, rejecting
that specific numerical hypothesis. Restoring Q5 residual arithmetic changed
some outputs; padding-only refinements did not change outputs or acceptance.

Final weighted accepted/drafted rates by workload:

| Workload | Shipped | Candidate |
|---|---:|---:|
| Log triage | 0.43903 | 0.44598 |
| Regression contract | 0.49979 | 0.49284 |
| Repository evidence | 0.68032 | 0.68027 |
| Structured output | 0.65826 | 0.59714 |
| Work-log extraction | 0.63633 | 0.63416 |

### Structured output: frontend serialization, not slow verify kernels

Both cases use function tools. In the final gate, struct-002 sets the p95:
shipped has 1,114 prompt tokens, 110 completion tokens, 20 rounds and acceptance
90/140; candidate has 1,115 prompt tokens, 102 completion tokens, 21 rounds and
acceptance 81/147. Candidate round cost is actually lower, **15.2055 versus
15.2968 ms**, but fewer tokens per round produce the 1.12560 ms/token ratio.
Struct-005 is 2.81729 versus 2.70732 ms/token (1.04062).

The Main-approved one-line diagnostic changed only the local JSON type inside
`src/serve/translate.cpp:90` from ordered `RequestJson` to `nlohmann::json`.
It canonically sorts the complete rendered tool definition, including outer,
function and nested schema keys, reproducing the shipped serialization policy.
This removes the extra prompt token in both cases. Struct-002 becomes 110 output
tokens / 19 rounds; struct-005 becomes 176 / 31. On the bounded diagnostic,
structured p95 falls from **3.08911 to 2.65130 ms/token**, versus shipped control
**2.74480** (ratio **0.96594**). Request-only function reordering did nothing;
recursive schema ordering alone changed behavior but did not remove the prompt
count discrepancy. Those incomplete counterfactuals are preserved, not treated
as proof that serialization is irrelevant.

`canonical-tools-diagnostic.json` binds the patch, both binary hashes and all
observations. **The diagnostic was reverted locally and remotely, and the frozen
G2 binary restored. It is not in the mergeable route set or the final gate.**
Whether to canonicalize tool serialization is a frontend behavior decision for
Main and the Frontend/ServePort owners via the integration owner, not a kernel fix.
This bounded result is not a full-corpus qualification of canonical serialization.

### Work-log extraction: recovered projection latency, residual acceptance cost

The earlier workload p95 ratio was 1.11822. It is now **1.03715**, within the
original 10% limit. Shipped p95 is worklog-020; candidate p95 is worklog-005.
For worklog-005 itself, both produce 850 tokens: shipped uses 181 rounds and
accepts 668/1,266; candidate uses 189 rounds and accepts 660/1,318. Candidate
round cost is **15.4809 versus 15.5584 ms**, while ms/token is **3.44222 versus
3.31301**. Thus the remaining case-level 3.90% gap comes from extra speculative
rounds, not slower round execution. The original failure combined the measured
projection latency deficit with an acceptance penalty and a changed p95 rank.
`tail-attribution-initial.json` and the final summary retain all per-case numbers.

Full-model numerical/acceptance identity remains unproven. For example, repo-002
has identical output hashes but 52/54 rounds. Op identity proofs do not localize
all prefill/draft hidden-state differences; no exact global equality is claimed.

## Supplementary depth protocol: do not hide acceptance regressions

These fixed-continuation probes are outside the 84-case role statistic. Each
single emits 2,048 tokens; each pair emits two 1,024-token responses. Paired
requests overlap prefill/decode and are not a constant-B2 kernel measurement.

| Nominal depth | Single shipped/candidate tok/s | Single shipped/candidate rounds | Single shipped/candidate ms/round | Paired-request shipped/candidate decode tok/s |
|---|---:|---:|---:|---:|
| 0 | 353.617 / 385.371 | 379 / 350 | 15.2812 / 15.1839 | 236.568 / 279.078 |
| 32K | 342.017 / 319.766 | 357 / 384 | 16.7731 / 16.6789 | 213.649 / 222.137 |
| 64K | 335.332 / 290.781 | 338 / 392 | 18.0692 / 17.9671 | 228.760 / 199.417 |
| 120K | 274.260 / 259.585 | 368 / 391 | 20.2917 / 20.1778 | 263.857 / 256.118 |

Deep fixed-continuation throughput does regress despite slightly faster rounds:
at 64K, acceptance is 0.72262/0.60379 and the candidate needs 54 extra rounds.
These observations are retained explicitly; the amended shipping decision is
about the role-corpus median, not a claim of universal workload parity. No
further lease time was spent optimizing after the founder's stopping criterion.

## Correctness, integration and reproduction

Eight affected scoped GPU executables passed with a 600-second per-test cap:
`ninfer_linear_add_q5_a16_test`, `ninfer_linear_swiglu_q4_a16_test`,
`ninfer_linear_topk_test`, `ninfer_linear_split_test`,
`ninfer_linear_split_multi_output_test`, `ninfer_linear_split_two_output_test`,
`ninfer_gdn_input_proj_conv_record_test`, and
`ninfer_gdn_input_proj_conv_snapshot_test`. The tests use represented-input FP64
oracles, graph replay, guard/immutability checks, mapped top-k IDs and tie cases.
T=8/16 and U=7/14 replay coverage was added. No tolerance was relaxed.

| Source-to-source proof | Cases | Compared words | Bit mismatches |
|---|---:|---:|---:|
| Final Q5 residual, K=6,144/17,408, T=8/16 | 16 | 983,040 BF16 | 0 |
| Fused Q4 SwiGLU, T=8/16 | 8 | 1,671,168 BF16 | 0 |
| Q4 shortlist projection, U=7/14 | 8 | 11,010,048 FP32 | 0 |
| Final Q5 input, N=7,168/12,288, T=8/16 | 16 | 1,867,776 BF16 | 0 |

Each proof uses four fixed seeds and unmodified Apache-2.0 `1302d639` reference
headers, archive SHA256 `ba4b5a389887ed592d4799600ee802ec1a751a8374430590afe2e49b8f9e7c67`.
Complete replay source is embedded in each receipt. The earlier Q4 input proof
in `fork-bit-identity.json` remains applicable to its unchanged schedule.
These are tested-vector source-schedule proofs, not every-input/cross-toolchain
or complete-model identity. No formatter, linter or project-wide suite was run.

After `9bee163e`, the route commits are `9242f521`, `24c93a36`, `740329be`,
`db007825`, and **`c2dbcbae`**. Integrate them as a set; do not stop at the
rejected intermediate Q5 schedule. The diagnostic serving alias is excluded.
The evidence-only handoff commit adds this report and the sanitized receipts.

All builds and GPU runs used the D2 lease. Reproduction uses the existing
supervised `build.sh` / `run-in-env.sh` with the frozen environment above, and
`tools/bench/run_exp097.py` with the identity, frozen corpus, case IDs, probe and
long fixture hashes in `g2-protocol.json`. **All four final arms run all 84 roles**;
the earlier baseline's `--skip-roles` optimization was not used. The standard
server flags were `--max-context 131072 --kv-capacity 131072 --prefill-chunk 1024
--kv-dtype bf16 --max-concurrency 2 --spec dflash2 --draft-tokens 7
--lm-head-draft --vision --preserve-thinking --pending-timeout-ms 180000`.
Shipped uses device states2, host states24, host KV16,384MiB and keep-warm60,000ms;
master uses device states4 and host context16,384MiB. These different historical
slot APIs express the same intended two resident lanes plus spare state.

The public receipts contain no prompts, generated text, credentials, host paths
or device UUIDs. Private raw captures/logs remain separately retained; their
public hashes bind the primary profiler evidence without exposing command-line
credentials. Integrated build/durability and product release gates remain the
integration owner's responsibility.
