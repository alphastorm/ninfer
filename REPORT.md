# EXP-097: upstream kernel-route handoff

## Continued G2 attribution

`profile-attribution.json` records the shipped/candidate Nsight Systems 2026.1.3
capture at 32,047 prompt tokens (log-009 plus the frozen depth fixture). The
candidate's acceptance is slightly higher, 1,198/2,353 versus 1,195/2,376; unequal
draft arithmetic exists even on the byte-identical 128-token warmup output.
Acceptance is therefore not assumed equal or blamed for this measured slowdown.

The dominant GPU deltas per round are Q5 residual projections **+0.924 ms**,
Q4 SwiGLU **+0.584 ms**, and Q5 input projections **+0.270 ms**. BF16 attention
including merge is **0.191 ms faster** upstream. Draft attention, Q8 projection,
selector, fused RoPE and dynamic convolution are essentially unchanged; shortlist
projection is 0.028 ms slower. Between-round idle is 0.358/0.371 ms, with only
0.019/0.021 ms overlapping host commit. The second upstream graph does not explain
the gap. No attention, Program-round or Engine change is justified by this capture.

The next measured candidate uses eight K warps for Q5 residual T=8/16 at K=6,144
and 17,408, retaining upstream's two-stage loading and the shipped linear
reduction order. sm_86/89 and neighboring extents retain their existing routes.
Expanded FP64/graph coverage includes both verify widths; GPU results are not yet
claimed for this candidate. Earlier results below remain the previous baseline.

The following isolated candidate reuses the existing padded pairwise Q4 schedules
for fused SwiGLU at T=8/16. All other widths and sm_86/89 remain unchanged. The
public numerical matrix already covers both widths; graph replay now does too.
Its separate bounded measurement and full fused-output fork comparison are
required before accepting the route.

## Disposition

- Tested runtime source: `9b391d513726b3ed7d60a80a95daa68cf9dacc97`, branch `rebase/kernel-routes`, upstream base `64d10fbf`.
- The guarded Q4/Q5 input-projection routes improve the role-corpus median by **10.55% over vanilla master**, but reach only **91.46% of shipped**. **G2 FAILS**: the required ratio is 98%, and two workload p95 ratios also exceed 1.10.
- Six scoped GPU Op executables passed. The source-to-source fork comparison found **zero differing BF16 output words across 2,949,120 values / 32 cases**. This is not a full-model quality proof.
- This handoff follows the request to finish the current iteration. No BF16 fixed-width candidate or Nsight attribution iteration was started. The original broader G2 assignment is not claimed complete.
- No production container, release tag, published image, or native dispatch schedule was changed. The branch is suitable for integration of the tested route changes, **not release acceptance**.

## End-to-end role results

One serial pass over the same 84 common EXP-095 cases per engine; all 252 role requests completed with HTTP 200. Low reasoning, temperature zero, no added reasoning headroom; reasoning-only completions remain included. Configuration: DFlash2 K=7, BF16 KV, 131,072 context and explicit global KV capacity, prefill chunk 1,024, two resident lanes.

| Engine | Median decode tok/s | Weighted decode tok/s | Decode ms/round |
|---|---:|---:|---:|
| Shipped `1302d639` | 338.573 | 291.848 | 15.524 |
| Vanilla `64d10fbf` | 280.102 | 248.003 | 18.428 |
| Q4/Q5 candidate | 309.651 | 274.994 | 16.645 |

Vanilla/shipped median ratio: **0.82730**. Candidate/vanilla: **1.10550**; candidate/shipped: **0.91458**. Candidate weighted decode improves 10.88% over vanilla and remains 5.77% below shipped. Candidate round cost falls 9.67% from vanilla and remains 7.23% above shipped. The median is not within two percentage points of the 0.98 boundary, so the agreed near-boundary repeat rule does not trigger.

Empirical nearest-rank p95 is computed over each workload's per-case decode milliseconds/token, not a replicated per-case latency distribution:

| Workload | Cases | Vanilla/shipped p95 | Candidate/shipped p95 | Candidate tail gate |
|---|---:|---:|---:|---|
| log-triage | 24 | 1.15434 | 1.04761 | pass |
| regression-contract | 8 | 1.17225 | 1.07303 | pass |
| repository-evidence | 30 | 1.18953 | 1.07392 | pass |
| structured-output | 2 | 1.41870 | 1.27769 | **fail** |
| work-log-extraction | 20 | 1.25413 | 1.11822 | **fail** |

Every individual case ratio, including regressions, is retained in `bench/measurements/exp097/{baseline-comparison,q4-comparison}.json`.

## Depth sweep and two-client pairs

Baseline order was shipped/upstream/upstream/shipped, with fresh isolated servers between arms. Baseline cells are arithmetic means of two observations; the candidate has one observation. Singles requested 2,048 completion tokens; each pair member requested 1,024. All 60 context requests completed with HTTP 200.

| Approximate context | Shipped single tok/s | Vanilla single tok/s | Candidate single tok/s | Shipped pair wall tok/s | Vanilla pair wall tok/s | Candidate pair wall tok/s |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 354.89 | 301.87 | 326.09 | 442.97 | 456.42 | 496.42 |
| 32K | 342.52 | 273.66 | 297.68 | 75.06 | 76.68 | 77.32 |
| 64K | 335.38 | 214.40 | 271.36 | 37.23 | 38.26 | 39.00 |
| 120K | 274.07 | 225.77 | 253.25 | 17.56 | 15.02 | 15.06 |

Pair wall throughput includes prefill, queueing, decode and delivery; it is **not** constant-B2 decode throughput. At long contexts, the explicit global capacity can serialize or preempt requests. Shape-based matching makes individual member attribution ambiguous when both members have identical token counts; pair aggregates, not member identity assignments, are authoritative. Full timing and round-cost values are in `context-comparison.json` and the five arm receipts.

## Route decisions

All Op microtimings use the public dispatch, cold CUDA Graph execution, 10 warmups and 61 samples. Each arm contains 56 measured Op rows, including neighboring widths. The following are median microseconds for the changed extents:

| Route / public Op | Physical extent | Vanilla | Candidate | Latency change | Disposition |
|---|---|---:|---:|---:|---|
| Attention Q4/Q5 split | aggregate T=8 | 48.416 | 38.112 | -21.28% | keep |
| Attention Q4/Q5 split | aggregate T=16 | 85.632 | 48.352 | -43.54% | keep |
| GDN Q4/Q5 split | aggregate T=8 | 62.752 | 46.304 | -26.21% | keep |
| GDN Q4/Q5 split | aggregate T=16 | 105.696 | 60.640 | -42.63% | keep |
| GDN snapshot | B=1, W=8 | 68.448 | 50.400 | -26.37% | keep |
| GDN snapshot | B=2, W=8 | 109.760 | 64.736 | -41.02% | keep |
| GDN record | B=1, W=8 | 66.848 | 50.432 | -24.56% | keep |
| GDN record | B=2, W=8 | 109.120 | 64.736 | -40.67% | keep |
| h24 BF16 verify | W=7/8 | measured in 32-row attention matrix | unchanged implementation | not attributed | retain upstream Grouped64; no fork kernel copied |
| Verify-round host-commit idle | end-to-end ms/round measured above | no phase attribution | no phase attribution | unproven | no scheduler/Program edits |

The Q4 route uses the existing sliced-K kernel with 16-byte shared code-row padding, pairwise K reduction, two staging buffers at T=8 and one at T=16. Q5 companions reuse upstream's existing two-stage sliced-K implementation. Both choices remain behind the existing shape dispatchers and `!NINFER_SM86 && !NINFER_SM89`; all other token extents retain their selectors. The materialized GDN snapshot/record path required its own dispatch change because it bypasses `linear_split`.

Upstream h24 Grouped64 already fits W=7 and W=8 in one query tile (42 and 48 packed rows). Vanilla B=1/context=128 measured 13.536 microseconds at both widths. A proposed constant-width specialization was **not** implemented or measured; this report does not claim that it would recover the remaining gap. The old fork BF16 implementation and unshipped `3a2fadbd` were not carried. No NVFP4 KV or scheduler transplant was introduced.

## Correctness evidence

| Executable / selector | Result | Wall seconds |
|---|---|---:|
| `ninfer_linear_split_multi_output_test` | pass | 126.937 |
| `ninfer_linear_split_two_output_test` | pass | 46.688 |
| `ninfer_linear_split_test` | pass | 14.198 |
| `ninfer_gdn_input_proj_conv_snapshot_test` | pass | 34.150 |
| `ninfer_gdn_input_proj_conv_record_test` | pass | 8.588 |
| `ninfer_softmax_attention_test --causal-only --kv-dtype bf16` | pass | 67.749 |

The tests now use complete decoded-input FP64 projection references at T=8/16, graph boundaries around T=16, full GDN T=16 references, dense/masked B2/W8 record cases, and B2/W7,W8 BF16 graph replay. No tolerance was relaxed. A 600-second per-executable limit was used; the multi-output test exceeds the earlier infrastructure sweep's 90-second limit.

`python3 -m unittest discover -s tests -p test_exp097.py` passed four accounting tests locally before route edits and returned success again inside the completed candidate iteration. No project-wide suite, formatter, or linter was run.

The temporary bit-comparison program compiled unmodified Apache-2.0 fork headers archived from `1302d639` beside the current kernel, under the same CUDA toolchain. It compared all BF16 output bits for Q4 row counts 4,096/7,168 and Q5 7,168/12,288, K=5,120, T=8/16, four seeds. Q5 T=8/16 arithmetic matches the fork on those vectors despite using upstream's staging implementation. `fork-bit-identity.json` preserves the complete replay source, reference paths/archive hash, command and all 32 results; no private reference implementation was added to the production source tree. This establishes the tested source schedules, not cross-toolchain identity of every possible input or full-model output identity.

## Artifact and environment identity

- Shipped runtime image: `sha256:6a02feba4163d992cc6a46baf28e0ece2ffe6a2ead91939e566c1b3080f5bc02`.
- GPU-ready build/runtime environment: `sha256:e7f34ca2f41ecea14da5cce0b2a30cae6b0877e3b3eb371bd92d103bca1121f1` (`ov-build-env:master-64d10fbf-r2`).
- Shipped v2 artifact: `0634abb07024221de141456cf04a42ab74b18bc38e1b781c6eb2e062a467eec3`; master-upgraded v3: `facba3bd318139c50899d4a0d74576837f73f0f498a461961467d56df10f4944`.
- Direct proof: all 1,184 tensor payloads / 20,424,296,864 tensor bytes identical; the complete original 20,437,148,160-byte payload range is preserved, SHA256 `ac0db89fef9efbadf38cc6bfd97b763714ed55b1ab6d31e28b59325f85e87ffd`. Proof took 44.439 seconds. Payload equality does not assert identical frontend rendering or all generated answers.
- The first vanilla Op attempt used the prior environment and failed with `cudaErrorStubLibrary`. It is environment-invalid, not a product correctness failure. Only that invalid GPU step was repeated after the r2 fix.
- Nsight Systems and Compute 2025.4.1.0 were located with CPU-only metadata commands. **No GPU profiler capture was executed.**

## Reproduction commands

Paths below are symbolic roots for the leased source, persistent build, external frozen corpus, model and helper directory; private host paths and credentials are deliberately excluded. All builds/GPU executions occurred under holder D2. The validated source commit is the one named above, not an evolving branch tip.

```bash
export NINFER_BUILD_HOLDER=D2
export NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2
"$COMMON/build.sh" "$SOURCE" "$BUILD" -DNINFER_BUILD_BENCHMARKS=ON -- \
 ninfer-serve ninfer_linear_split_multi_output_bench ninfer_linear_split_two_output_bench \
 ninfer_causal_softmax_attention_bench ninfer_gdn_input_proj_bench \
 ninfer_linear_split_multi_output_test ninfer_linear_split_two_output_test ninfer_linear_split_test \
 ninfer_gdn_input_proj_conv_snapshot_test ninfer_gdn_input_proj_conv_record_test \
 ninfer_softmax_attention_test ninfer_qwen3_5_dflash_round_bench ninfer_bench
```

Each test in the correctness table was run through `run-in-env.sh --gpu --mount "$ROOT" -- "$BUILD/tests/<executable>"` with its displayed selectors. The bit-comparison compile command is embedded in its receipt. The DFlash round benchmark's new `--spec dflash2` selector was built but no standalone round benchmark measurement was executed.

```bash
"$BUILD/bench/ninfer_linear_split_multi_output_bench" --format q4q5 \
 --tokens 7,8,9,15,16,17 --execution graph --cache cold --warmup 10 --repeat 61 --csv-out attn-input.csv
"$BUILD/bench/ninfer_linear_split_two_output_bench" --format q4q5 --n 16384 --k 5120 \
 --tokens 7,8,9,15,16,17 --execution graph --cache cold --warmup 10 --repeat 61 --csv-out gdn-split.csv
# Each combination of OP=snapshot,record and B=1,2:
"$BUILD/bench/ninfer_gdn_input_proj_bench" --op "$OP" --format q4q5 --n 16384 --k 5120 \
 --tokens 7,8,9 --batch "$B" --execution graph --cache cold --warmup 10 --repeat 61 --csv-out gdn.csv
"$BUILD/bench/ninfer_causal_softmax_attention_bench" --entry append --geometry d256-h24-kv4 \
 --kv-dtype bf16 --batch 1,2 --tokens 6,7,8,9 --context 128,32000,64000,120000 \
 --mapping fragmented --execution graph --cache cold --warmup 10 --repeat 61 --csv-out attention.csv
```

Server common arguments were `--host 0.0.0.0 --port 8080 --model-id q38-ninfer --request-log-jsonl /logs/requests.jsonl --max-context 131072 --kv-capacity 131072 --prefill-chunk 1024 --kv-dtype bf16 --max-concurrency 2 --pending-timeout-ms 180000 --spec dflash2 --draft-tokens 7 --lm-head-draft --vision --preserve-thinking`, with a separately supplied private key. Docker bridge publishing was loopback-only `127.0.0.1:18140:8080`, with the io_uring seccomp policy. Shipped extras: `--device-state-slots 2 --host-state-slots 24 --host-kv-mib 16384 --gpu-keep-warm-ms 60000`; master extras: `--device-state-slots 4 --host-context-mib 16384`. These slot flags have different historical semantics; both represent the intended two resident lanes plus spare state.

```bash
python3 tools/bench/run_exp097.py --arm "$ARM" --base-url http://127.0.0.1:18140 \
 --api-key-file "$KEY_FILE" --request-log "$REQUEST_LOG" --qualification "$QUALIFICATION" \
 --probe "$PROBE" --case-ids "$CASE_IDS" --long-fixture "$LONG_FIXTURE" \
 --identity "$IDENTITY" --receipt "$RECEIPT"
# B2 and A2 additionally used --skip-roles.
```

The arm receipts bind runner/probe/corpus-selection/fixture hashes and binary/image/model identities. The shipped binary hash is explicitly labelled expected, not independently measured. Receipts contain only hashes, case identifiers, counts and timings; prompts, generated text, keys and private host paths are excluded.

## Unclosed acceptance boundaries

G2 remains failed; no release/fallback decision is made by this slice. The remaining gap has not been attributed to host commit idle, attention, shortlist projection, or other schedules. Full-model quality/leak screening and native Windows numerical qualification remain required by the program. BF16 fixed-width experimentation, GPU profiler attribution and any additional kernel family changes require continued work beyond this requested handoff; none are silently represented as completed here.
