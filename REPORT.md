# EXP-098: native DFlash2 measurement

**Complete, receipt-only. No runtime, profile or package change.** Both cards completed matched MTP3 versus DFlash2 K=7 measurements on published v3 `81f924d4`, INT8-group64 KV, C1, low reasoning, temperature zero and no added reasoning-token headroom. The common KernelRoutes corpus contains 84 cases; each arm also ran all 504 EXP-085 redaction-subset prompts and 14 fresh-process determinism repeats. This is not the full multi-metric EXP-085 quality screen.

## Measured result

Each comparison uses the same binary and largest measured DFlash2 context on its card. Both role arms returned HTTP 200 for all 84 cases. Rates are server-reported decode measurements; weighted rate is total completion tokens divided by summed decode seconds, and median is over the 84 cases.

| Measurement | RTX 4090 MTP3 → DFlash2 | RTX 3090 MTP3 → DFlash2 |
|---|---:|---:|
| Matched context / KV capacity | 94,144 | 92,672 |
| Median decode tok/s | 144.10 → **187.80 (+30.32%)** | 80.81 → **112.08 (+38.70%)** |
| Weighted decode tok/s | 137.43 → **164.81 (+19.92%)** | 77.35 → **97.82 (+26.46%)** |
| Accepted / drafted tokens | 42,536 / 55,228 → 47,977 / 90,561 | 42,823 / 55,921 → 48,225 / 91,853 |
| Acceptance rate | 77.02% → 52.98% | 76.58% → 52.50% |
| Speculative rounds | 18,416 → 12,957 | 18,650 → 13,141 |
| Completion tokens / round | 3.310 → 4.706 | 3.297 → 4.673 |
| Decode milliseconds / round | 24.085 → 28.555 | 42.629 → 47.775 |
| Sampled peak used VRAM, MiB | 21,496 → 22,918 | 21,340 → 22,794 |
| Greedy output pairs byte-identical | **28 / 84** | **34 / 84** |

Greedy equality hashes include content, reasoning and tool names/arguments, excluding transport IDs. They are output equality checks, not proof of token-level arithmetic equivalence. Full per-case, per-workload and p95 measurements are in [rtx4090-comparison.json](bench/measurements/exp098/rtx4090-comparison.json), [rtx3090-comparison.json](bench/measurements/exp098/rtx3090-comparison.json) and their bound arm receipts.

## Fit and quality limits

- **4090:** 94,144 tokens leaves **673,185,792 CUDA-free bytes**, just **2 MiB above the 640 MiB floor**. The adjacent 94,208-token configuration starts and decodes but falls **2 MiB below** that floor; 98,304 is rejected by the startup budget. The assignment's known-unfit 131,072 case was not rerun. [11-attempt receipt](bench/measurements/exp098/rtx4090-fit.json).
- **3090:** 92,672 tokens leaves **806,354,944 CUDA-free bytes**, just **1 MiB above the 768 MiB floor**. The adjacent 92,736-token configuration starts and decodes but falls **1 MiB below** the floor; 92,800 and 131,072 are rejected by the startup budget. [12-attempt receipt](bench/measurements/exp098/rtx3090-fit.json).
- Boundaries use 64-token pages, retain the lane's 2/4 extra device slots, 2048/1024 prefill and exact host budgets, and require a successful 64-token decode. These are conditional fit boundaries, **not full-context retrieval or durable/C2 qualification**. CUDA free memory determines the floor; NVML residency is separately reported and is not interchangeable with it.
- **Both leak screens are invalid, not passes or established regressions.** All 504 prompts were scored in every arm. Errors exceed the five allowed: 4090 **10 → 16**, 3090 **13 → 15**. Fresh-process output mismatches also fail determinism: 4090 **8 → 7 of 14**, 3090 **8 → 6 of 14**. Raw leak totals are **526 → 532** and **523 → 524**, respectively; these do not establish non-inferiority. No failed sample was suppressed or rerun. The registered rule, validity failures and unqualified observations are preserved.

## Recommendation

**Keep MTP3 at 131,072 tokens for the next native default profile.** DFlash2 delivers useful measured throughput gains, especially on repository and work-log workloads, but sacrifices roughly 28–29% of context capacity, leaves only 1–2 MiB beyond the required headroom at its measured limit, and does not preserve most greedy outputs. More importantly, neither leak screen yields a valid quality verdict. These results justify a separately qualified, lower-context opt-in DFlash2 candidate, not replacing the native default or lowering its headroom; this experiment changes neither.

## Provenance and restoration

- Measured source: **`eecb437f99d21cbc7f48740694de3cd6e51d4421`**, with independently copied 27-file native sets. Server SHA256: 4090 **`13980848de039c065a30e663d3f78d2576b03afb03c91dbbb2ffb6066d93baf9`**; 3090 **`31ab5abeafa139a021134a6d8ff01143bc65dfd9e582905a86f5b1ee4d949d23`**. Model: **`81f924d440c27261d820c19a9f8d45794c5aee410f8a68bd358133fa8c0375da`**, **20,437,521,664 bytes**, hashed for each invocation.
- Published source is `7a25cd72f490183e4113f1e84e68e36d653ddd28`. **The release lead attests that eecb437f and 7a25cd72 runtime binaries differ only by embedded provenance.** These receipts nevertheless bind the actually measured eecb437f binaries, not the published binary hashes.
- The initial I6 grant was preempted after the complete 4090 MTP arm; subsequent runs used Main's post-acceptance grants. All four windows stayed below 150 minutes. Own protected native leases were released after each window; final observations show **450/370 W, zero owned servers, no own leases, both canonical tasks Ready**. Canonical installations, tasks and model roots were not modified. [Grant, invocation and restoration ledger](bench/measurements/exp098/operations.json).
- Executed verification was the requested six fit/arm runs and two receipt reductions only: **no build, test suite, lint or formatter**. Harness: `tools/bench/run_exp098_native.py`; reducer: `tools/bench/compare_exp098_native.py`; [frozen protocol](bench/measurements/exp098/protocol.json). Public receipts contain counts, measurements and hashes, never prompt/output text or credentials. Exact private-path commands remain in the operator command log.
