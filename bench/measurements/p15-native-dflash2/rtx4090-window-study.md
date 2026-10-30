# RTX 4090 DFlash2 decode-floor investigation

Status: **no speed fix is qualified**. The smallest remaining candidates are runtime-only
DFlash2 K6 and K5 on the RTX4090 lane, in that order. Existing K3 evidence rejects K3.
No runtime/kernel/loader/default/lane-profile bytes change in this investigation.
The RTX3090 K7 and RTX5090 paths stay unchanged. This is a bounded experiment;
GPU acceptance and production promotion belong to the release lead.

## Recorded cost model

Source: P14 `7a25cd72f490183e4113f1e84e68e36d653ddd28`, INT8 KV, C1,
chunk1024, graphs on, optimized proposal, 450W RTX4090 / 350W RTX3090.
The full redacted inputs and calculations are in [rtx4090-round-cost.json](rtx4090-round-cost.json).
These are **not** measurements of installed N (`2c6f423b`) or frozen P15 F.

The assignment's 171262 drafted / 94758 accepted / 24526 rounds and the MTP
104700 / 83864 / 34930 are the **108-case aggregate**, confirmed by the gate's
`acceptance` block. They are not log-024's counters. `accepted_tokens / rounds`
counts accepted drafts, not all emitted tokens: a speculative round also emits a
correction/bonus token. Treating 3.86/2.40 as total tokens per round and combining
that aggregate ratio with one-case throughput incorrectly produces ~1.7x cost.

For log-024, every measured request emits1024 tokens; the first comes from prefill.
Define `R=speculative.rounds+fallback_steps`, amortized `ms/round=1000*decode_seconds/R`,
and `emitted/round=1023/R`. MTP has314 speculative rounds plus one fallback;
DFlash2 K7 has291 speculative rounds and no fallback. Accepted drafts are708/732.

| Card/series | Backend | Drafted | Accepted drafts/round | Emitted/round | ms/round (two-arm mean) |
|---|---|---:|---:|---:|---:|
| 4090 original | MTP3 | 942 | 2.254777 | 3.247619 | 24.074258 |
| 4090 original | DFlash2 K7 | 2032 | 2.515464 | 3.515464 | 27.550563 |
| 4090 isolated | MTP3 | 942 | 2.254777 | 3.247619 | 24.006219 |
| 4090 isolated | DFlash2 K7 | 2032 | 2.515464 | 3.515464 | 27.495910 |
| 3090 original | MTP3 | 942 | 2.254777 | 3.247619 | 42.574534 |
| 3090 original | DFlash2 K7 | 2032 | 2.515464 | 3.515464 | 47.032190 |

Thus the case's yield improves only1.082474x, but DFlash2 rounds cost1.144399x
on4090 versus1.104702x on3090. Their quotients explain the observed speed ratios
0.945889 and0.979873. Isolated4090 is0.945090, not noise inferred from a single
sample. With unchanged K7 yield, only about0.52% faster decoding would cross0.95;
changing the window also changes yield, so that is not the K6/K5 required gain.

Whole108-case totals, both ABBA repeats (216 requests per backend):

| Card | Backend | Accepted drafts | Spec rounds | Fallbacks | Decode seconds | Accepted/spec round | Emitted/decode round* | ms/decode round |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 4090 | MTP3 | 83864 | 34930 | 26 | 859.188277 | 2.400916 | 3.394839 | 24.579136 |
| 4090 | K7 | 94758 | 24526 | 28 | 683.055839 | 3.863573 | 4.855176 | 27.818516 |
| 3090 | MTP3 | 84010 | 35026 | 32 | 1509.125719 | 2.398504 | 3.392036 | 43.046543 |
| 3090 | K7 | 94772 | 24476 | 20 | 1163.625815 | 3.872038 | 4.864958 | 47.502687 |

*`(sum(completion_tokens)-request_count)/(spec_rounds+fallbacks)`; terminal
publication can truncate licensed tokens. The registered weighted gate uses
completion_tokens/decode_seconds, not this explanatory amortization. Decode
weighted gains remain1.263615x/1.299682x; a strong aggregate does not erase the floor.

### What the timings actually locate

Original4090 B1 logs7.988993s decode device-wait exposure and0.033987s host
exposure over291 rounds. A0 logs7.554860s device wait and0.035243s host over315.
3090 B1 is13.644178s wait/0.049893s host; A0 is13.399639s wait/0.053279s host.
This supports a predominantly device-side difference, not a large host-commit
regression. Timing domains are distinct: `engine_timing.decode` includes engine
boundaries beyond the model's `timings_seconds.decode`; do not sum them as if exact
subcomponents of the same interval.

Draft vs target verify vs state commit **GPU times were not recorded**. Whole-request
`engine_commit_output` and `program_post` host counters are not GPU state-commit
measurements. No per-width verify cost or hardware-counter proof identifies a
specific bottleneck. The conclusion is a measured cost/yield imbalance on4090,
not proven FP8 saturation, bad acceptance numerics, or an sm89-only kernel bug.

## Configuration candidates and output effects

Existing API `SpeculativeOptions` makes K startup-fixed (MTP1..5, DFlash2 1..15).
`program/round_buffers.cpp` sizes actual proposal/verify/selector storage from K;
`execution/draft.cpp:270-384` runs the draft at K+1 and selector at K;
`program/decode.cpp:593-790` submits the configured-width graphs. A server-config
window change genuinely changes physical work without a new engine knob.

| Priority | Candidate | Source change? | Byte effect / rationale |
|---|---|---|---|
| 1 | 4090 DFlash2 K6, width7 | No; existing config | May change output bytes. Bidirectional masked draft, dynamic convolution and selector see a different block; target numerical route/shape and sampled proposal decisions can change. Opt-in4090 only; quality and determinism gates required. |
| 2 | 4090 DFlash2 K5, width6 | No; existing config | Same caveat, more work removed but more late accepted tokens lost. No3090/5090 configuration change. |
| Rejected | K3, width4 | No | Already measured failure:0.811526 floor,13/84 below0.95, weighted0.969806. Recorded K3 normalized output hash differs from K7. No unchanged repeat-to-pass. |
| Follow-up | Acceptance-adaptive physical width | Yes | Potential output changes and a new graph/continuation-history policy. Changing only `proposal_extents` leaves full K+1 tensors/graphs and much of the GPU cost intact; that is not a valid cheaper-round fix. |
| Follow-up | sm89 width8 target route | Yes | Kernel reduction/rounding may change bytes unless exact oracle/T-invariance proves otherwise. Needs L6 and measured phase attribution first; never silently enable on3090/5090. |

### Quantitative window sensitivities

Original4090, isolated4090 and3090 K7 per-position acceptance is
`[197,157,121,87,68,55,47]`, summing732 across291 rounds. Match each raw
`request_done` uniquely to its case receipt by decode duration: counters alone
are nonunique across cases and can incorrectly select another request's histogram.

Counterfactual truncation at K predicts `Y(K)=1+sum(position[0:K])/291` **if**
all earlier proposals/decisions stayed unchanged. Relative to the isolated K7
ratio `r=0.945090`, floor0.95 requires round-cost reduction
`1-r*(Y(K)/Y(7))/0.95`:

| K | Width | Counterfactual yield | Required round-cost saving | Isolated-based max ms/round |
|---|---:|---:|---:|---:|
| 6 | 7 | 3.353952 | 5.09% | 26.097079 |
| 5 | 6 | 3.164948 | 10.44% | 24.626445 |
| 4 | 5 | 2.931271 | 17.05% | not selected |
| 3 | 4 | 2.632302 | 25.51% | measured failed |

These are sensitivities, **not measured predictions or confidence intervals**.
K3 provides real counterevidence: actual yield2.643411, actual24.145198ms/round;
only12.36% cheaper than K7, but24.81% fewer tokens/round. Reducing width need not
improve end-to-end speed.

Native Q5 residual-add dispatch at both `n5120_k6144.cu` and
`n5120_k17408.cu` uses direct SIMT for widths1..4 and the same sliced-K
`SlicedR16T8W4S2` for widths5..8. The target Q4 SwiGLU native small-width path
also shares an eight-column tile. Hence K6/K5 cannot assume proportional width
savings in all expensive projections. Their draft selector/output/head work and
valid-column operations may save work; that saving remains unmeasured.

## Preregistered admission and rejected source shortcuts

The private release job plan registers K6 thenK5 isolated four-arm ABBA against
MTP3 on N, with every B/A cross-pair>=0.95, identical case/request bytes and
no repeat-to-pass. It then repeats the defined selection on final F/profile
bytes; among passing windows choose the highest minimum cross-pair ratio,
exact ties chooseK6. Only the final-profile winner proceeds to the unchanged
108-role>=1.08 weighted/every-case>=0.95 and seven-supplemental gate. The powered
EXP-0851120-prompt screen, deterministic outputs, agent probes and mandatory
131K INT8/640MiB-headroom true-cold durability proof remain required. Failure
keeps4090 MTP3 on F; no kernel experiment delays3090 freeze.

The preparation's sm86-binary-on4090 bisection is blocked *before launch* by
`program/planning/startup.cpp`'s exact compute-capability check. ISA forward
compatibility alone does not permit that run. A diagnostic guard change would
be a new source/build, excluded from F; do not remove the shipping architecture
check just to make the experiment run. No new code fix is justified by the
currently missing GPU phase measurements, and no source-fix performance claim
is made.

## Host-side proof

`ninfer_qwen3_5_dflash_window_test` executes the real existing CLI validation,
round-layout planning and Host-backed frame binding for K1/3/5/6/7/15 and
batch1/8. It checks K+1 proposal/verify/append/RoPE shapes, K-position selector
and probability buffers, target/logit/continuation/output dimensions, compact
row strides, increasing real allocations and invalid window/batch rejection.
It neither instantiates a DeviceContext nor launches CUDA. CI builds it with
`ninfer_core` plus `round_buffers.cpp` and includes it in the GPU-free CTest list.
This proves host shape handling, not GPU numerical correctness or speed.
