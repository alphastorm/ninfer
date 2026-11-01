# P17 DFlash2 low-acceptance preregistration

Status: **registered design, no engine implementation or new GPU measurements**.
Refs [#58](https://github.com/alphastorm/ninfer/issues/58). Read the
[round-form design](../../../../docs/maintainer/dflash.md#round-forms-and-low-yield-governor)
for state, identity and file-level implementation scope. The existing
[P15 window study](../p15-native-dflash2/rtx4090-window-study.md) remains historical,
not a measurement of this candidate. S0 below is retained-evidence analysis.

## Binding and immutable gates

- A is the **shipped v0.14.0 MTP3 incumbent binary**, not MTP mode in the candidate
  engine. The retained gate binds RTX 4090 A SHA-256
  `5caa778029ba973ed06446f002d9da95faba0d70d66cb033abee7c4b88ad46e9` and RTX 3090 A
  `fedce425c09b3eca249d7ae3b4f8dcfe4a6839a82befa6168f77625ef6ec04c3`.
  A must be recovered from that shipped artifact with its DLL closure verified.
- N is v0.16.0 K7, source `26fa28d0e7834515508fc07ac503164a6effc820`:
  RTX 4090 binary `c6f59e9e7da2fafb8c2ded9e2cfaa63d8be5c9a91e4dbf615af42ffb4698be71`,
  RTX 3090 binary `72cd776be6caa99148757a88db8dca046f2c1fba67166975c8b4f2f84d94c624`.
  N is an identity/non-regression reference, never a substitute for gate A.
  Confirm publication and these bindings before S1. If publication changes
  bytes, freeze a disclosed amendment before measurements; do not relabel old data.
- Freeze candidate executable/DLL closure, model, tokenizer, effective profile,
  harness and request manifest by SHA-256 before each stage. The retained model
  SHA-256 is `81f924d440c27261d820c19a9f8d45794c5aee410f8a68bd358133fa8c0375da`.
- Requests are exactly the gate's 108 role cases: 84 depth0 plus 12 each at
  added depth32000 and depth64000. Compare each canonical request SHA-256 before
  pairing. The sorted `case_id SP request_sha256 LF` manifest has SHA-256
  `714102ec7c6a506993822731c85a3eae593e79151c4f88d7629a6ac4007f9252`.
  `log-024@0` request SHA-256:
  `cc7b5ef1f63a31d2292c1dc0d1201433d8ff4a9dbc7e73beb32cf747a225dcde`.
  Retained per-arm role receipts supply all manifest rows. No regeneration of
  allegedly equivalent prompts, changed sampling, omitted cases or warmup inclusion.
- Speed gates remain **108-role weighted >=1.08 and every case >=0.95 versus A**.
  Keep the gate's existing aggregation and completion_tokens/decode_seconds
  definition. Publish every case, both repeats, all exceptions and the minimum.
  The previous log-024 waiver is not inherited. Quality, determinism, agent,
  seven-supplemental, long-context and durable-state gates are unchanged.
- Native RTX 4090: 450 W, INT8 KV, C1, chunk1024, optimized proposal head,
  graphs on, capacity131072 and existing 640 MiB admission headroom. Native
  RTX 3090: 350 W, same controlled tuple, 768 MiB reserve. Preserve all other
  shipped profile settings. Isolate each lane; no overlapping measurement work.
- No engine implementation, GPU allocation, profiling or acceptance work before
  design acceptance and v0.16.0 publication. No production profile changes before
  all S4 gates. This document grants no operational window or release authority.

## S0 — completed offline, with limits

Sources are retained `final-4090/N16-d0-{0,1,2,3}.tar.gz`,
`final-3090/N16-d0-{0,1,2,3}.evidence.tar.gz`, RTX 4090 added-depth B1 archives,
and P16 W4090 `campaigns/4090-k{6,5}-log024-d0` plus `attribution-N/B`.
Request logs are joined uniquely to role receipts by decode duration, with
counter cross-checks; matching aggregate counters alone is not a valid join.
Private S0 results preserve archive hashes, exact JSONL lines and all reductions.

- **S0-a:** the only retained K6/K5 case is log-024@0. Both repeats differ from
  same-source K7 in normalized output hash, actual output-file bytes and token
  IDs. K7 hash is `bbe977b90f4a6fec611543b42449168b13ed6f3d6635486ed615f06ee8c4d76d`;
  K6 is `157e802d6bdbe9ac2eaa2e06c360631d18b2d842861a0e349325ad7973b95021`;
  K5 is `0f68b51e66c208fc60300a4dea5d25641f808f06d89a9d5e545fc6f643557812`.
  Other cases are unmeasured, not identity successes. Historical K3/K7
  normalized difference is documented in P15; the committed P14 K3 receipt
  contains no raw per-case hashes. H_W is not established for any Short form.
- **S0-b:** all 108 RTX 4090 B1 requests are greedy (temperature0), with zero
  presence/frequency penalties, min_p0, resolved top_k20/top_p approximately0.95.
  Six tool-grammar requests exist: two at each depth. All six low cases are
  unconstrained. Both cards' four depth0 arms have the same sampling controls.
- **S0-c:** log-024 has 291 rounds, attempted
  `[291,291,291,290,290,290,289]`, accepted `[197,155,121,87,68,56,48]` on both cards
  and repeats. Its reconstructed A=0..7 distribution is
  `[94,42,34,34,19,12,8,48]`, not the preparation's historical histogram.
  Other low-case B1 arrays are below; B2 equals B1 on its own card.
- **S0-d:** a non-binding text-copyability pre-screen of 24 recorded log prompts
  and outputs finds prefix-match proportions about0.20–0.49 (log-024:0.367).
  Retokenization is reversible Unicode whitespace/word/punctuation units,
  not model BPE. Search prior suffixes4/3/2 longest-first, most recent first,
  propose at most7 already-observed units; compare with subsequent recorded
  reasoning then content, without indexing future output. Missing model-token
  ledger/chat-template markers/round frontiers make this unsuitable for token
  acceptance or speed admission. S1 must capture exact IDs for S2 LookupSkip.

| Card | Case | Attempted positions0..6 | Accepted positions0..6 | A=0..7 distribution |
|---|---|---|---|---|
| Both | log-010@0 | [281,281,281,281,281,281,281] | [221,162,114,85,60,42,29] | [60,59,48,29,25,18,13,29] |
| 4090 | log-015@0 | [273,273,273,272,271,271,271] | [215,169,132,93,62,46,33] | [58,46,37,39,31,16,13,33] |
| 3090 | log-015@0 | [277,277,277,277,277,276,275] | [214,167,128,91,65,47,34] | [63,47,39,37,26,18,13,34] |
| 4090 | log-017@0 | [310,310,310,310,310,309,309] | [238,169,112,80,50,37,27] | [72,69,57,32,30,13,10,27] |
| 3090 | log-017@0 | [253,253,253,253,253,253,253] | [194,145,111,81,64,47,27] | [59,49,34,30,17,17,20,27] |
| Both | log-023@0 | [284,284,283,283,283,283,283] | [223,160,114,87,68,50,37] | [61,63,46,27,19,18,13,37] |
| Both | worklog-004@0 | [199,199,199,198,197,196,196] | [154,126,91,72,62,51,43] | [45,28,35,19,10,11,8,43] |

`SpeculativeStats::observe_round` records native verification, even if publication
truncates/discards outputs. Histogram reconstruction is `[R-c0,c0-c1,...,c6]`;
zero-extent fallbacks are excluded (worklog-004 has one). No ordered acceptance
trace exists here, so clustering is unknown. Card-specific log-015/log-017
outcomes differ. The 0.82 ms/column estimate is an extrapolated old K7/K3 secant,
not N16 live-column cost; it is excluded from all admission calculations.
Older recovered profiler unions suggest target verify dominates but retain an
INVALID capture-lifecycle receipt: explanatory only, never speed evidence.

## S1 — characterize after publication

Cluster-6 is fixed: log-024@0, worklog-004@0, log-010@0, log-023@0,
log-017@0, log-015@0. T is the default-off trace plus ExtentCap measurement
build from the design, not a physical-width refactor. ABBA means A,B,B,A;
identity comparisons use N or T-off, while speed admission always uses shipped A.

| Arm | Lane | Work and fixed decision |
|---|---|---|
| S1-BASE | 4090, then3090 | A versus N, isolated cluster-6 ABBA; bind current cost/yield and all cross-pair ratios |
| S1-TRACE | Both | N versus T-Long with trace, 108-case ABBA; require108/108 output hashes equal and each case's paired median absolute decode-time change <=1%; otherwise stop/fix trace before screening |
| S1-K | 4090 K3/4/5/6; 3090 K3/4 | Static width versus N K7, one ABBA per width on108; characterize cost/output effects only, not promote static windows |
| S1-PHASE | Both | K7/K4/K3 on log-024 plus fixed log-012@0 and repo-001@0, three profiled repetitions each; GPU phase unions for append/proposal/verify/accept/control and host gap; profiling never enters speed ratios |
| S1-CAP | Both | T forced-cap1 and3 versus T-off, one108-case ABBA each; fixed-W partition-identity screen and empirical live-work savings, not a linear per-column extrapolation |
| S1-CAL | Both | T alternates informative Long/cap3 after16 Long observations, one108-case ABBA versus T-off; held-out adaptive calibration control, never a production candidate |

Trace request SHA, round ordinal, frontier, K/P, previous append count, batch and
lane identity, graph class/envelope, A, licensed/committed counts, clipping,
terminal/discard state, arming/decision and exact prompt/output token IDs.
Preserve proposal IDs/q when required by an admitted replay analysis. Timings
must identify their domain; do not sum CPU exposure and GPU intervals as disjoint
components. No score D2H copy is needed for the history governor. Post-draft
confidence requires its own later instrumentation and accepted plan.
Report lag1..8 low-A transition rates, run lengths and base rates per request;
absence of clustering is a valid negative result, not grounds to alter the policy.
Trace cap: at most131072 round records and262144 token IDs per request; refuse
incomplete evidence on overflow. No silent truncation or extra default allocations.

## S2 — calibrated offline screen, separately per lane

Freeze these rules before S1. The governor uses the design's commit-only,
integer state machine. R in {8,16,32}; H=R*q/4 for q in {10,11,12,13};
ExtentCap cap in {1,2,3}; conditional Short K in {3,4,5}; conditional lookup
minimum continuation in {2,3,4}. Lookup alone is screened first, then combined
only with the top-ranked admitted non-lookup policy. Lookup uses exact token
suffixes4/3/2, most-recent earlier occurrence with available continuation,
verified token equality and no future access; exclude constrained requests.

Replay long cost uses measured lane/case/frontier costs, accounting for previous
append count. Approximate substitution yields are 1+A, 1+min(A,cap) or
1+min(A,Kshort); lookup uses exact token matches at the recorded frontier.
Extent costs come from S1-CAP; Short costs from S1-K; lookup savings may not
exceed measured proposer cost and must include host lookup/index overhead.
Do not use0.82ms/column. Normalize to fixed output budget, include prefill-first
output and fallback/terminal rules, then apply the existing weighted reducer.

This substitution is **not an exact counterfactual**: changed round boundaries
visit unobserved frontiers; Short proposals can differ even at a common frontier.
Report unsupported-frontier coverage and output divergence, not invented A values.
Calibration controls are S1-K, S1-CAP and held-out S1-CAL, not fitted policies.
Let epsilon be the maximum of (a) nearest-rank95th percentile absolute relative
error in per-case decode seconds and (b) maximum such error on cluster-6.
Compute it separately for each mechanism/lane; no pooling that hides a failure.
Require epsilon<=0.02, matching identity assumptions, full case coverage and
successful adaptive calibration. Exact replay/target identity is never inferred
from this error bound; epsilon is an empirical screening margin, not a CI.
Unmeasured widths cannot be admitted by interpolation. Cap2 is eligible only
in the direct fallback screen below, not replay admission; RTX3090 Short K5
is likewise excluded because its S1-K matrix measures only K3/K4.

Admission requires predicted log-024 ratio >=0.95+epsilon+0.01, every case
>=0.95+epsilon, and predicted weighted >= the session's N weighted ratio, all
against A. Rank admitted policies by identity class1,1-prime,2; then fewer new
graphs (ExtentCap, LookupSkip, Short); then higher minimum ratio, higher weighted
ratio, and lexicographic (action,cap/K,R,H,lookup-min). Freeze one finalist per
lane; 3090 stays disabled unless its own screen admits one. 5090 stays disabled.

If calibration fails, no replay ranking/admission is allowed. The only direct
fallback screen is six preregistered ExtentCap policies: R=16,H=44 or R=32,H=88,
each with cap1/2/3, cluster-6 ABBA versus A, at most one execution each. Reject
any cross-pair below0.95; rank survivors by the same order, then send one to
full S4. These are diagnostic admissions, never a substitute for the108 gate.
If nothing qualifies, retain the shipped configuration and report no admissible
mechanism. No repeat-to-pass, post-hoc grid expansion or automatic MTP fallback.
Post-draft verify-width exploration requires a separately accepted preregistration.

## S3 — only admitted conditional forms

Short plumbing: forced Short versus same binary's static width on108, full
request/output/token comparison, repeat determinism, Long-to-Short transition
and C>1/partial-terminal seams. Class1-prime additionally needs N K7 identity;
matching static Short alone is not H_W. No +/-2% yield substitute for broken
layout/identity. LookupSkip: measured proposals and acceptance must equal the
token-accurate prediction at each observed frontier; exclude grammar, verify
context catch-up, penalty counts and fixed-W target authority. Any unexplained
mismatch stops the stage. ExtentCap has no extra physical-form plumbing stage.

## S4 — frozen finalist and profile, no repeat-to-pass

1. Identity versus N:108 plus seven supplementals; all unarmed requests equal
   byte/token outputs (class0), all requests equal for class1. Class2 requires
   explicit output-change disposition and unchanged full quality/agent gates.
2. Isolated cluster-6 ABBA versus shipped A: every B/A cross-pair >=0.95.
3. Full108 gate versus A: weighted>=1.08, every case>=0.95; additionally paired
   candidate/N weighted>=0.995. Report raw request/output SHA, all case ratios,
   weighted reducer inputs, completion counts, fallbacks and minimum cases.
4. Existing powered EXP-0851120-prompt quality screen, agent probes, seven
   supplementals, determinism and long-context gates remain mandatory, with
   their existing decision rules. No quality subset chosen after speed results.
5. Existing131K INT8 true-cold durability proof; actual extra form graph/buffer
   memory<=64MiB AND unchanged lane admission reserve. Record capture time,
   post-capture availability, startup peak and OOM status. A fit failure rejects
   the form; do not lower capacity/headroom to make it pass.
6. Two planned repeats of armed cases must agree on output token IDs, normalized
   output hashes and governor decisions. Disabled3090/5090 profiles remain
   unchanged; use their normal unchanged-profile qualification, not an implicit
   rollout. No separate GPU window is authorized here for5090.

A genuine failure rejects that frozen finalist. Correctness fixes or invalid
infrastructure require a separately recorded disposition before any new run;
retain the original failure. No merge or lane-profile promotion is part of this
package; implementation and GPU execution are subsequent accepted work.

## Resource request and stop caps

Requested **after publication**, estimates rather than executed reservations:
S1 arms above: native RTX4090 exclusive up to8h, then native RTX3090 up to10h.
S2 is offline; direct fallback, only if needed, adds at most2h/3h respectively.
S3 conditional plumbing at most2h per admitted lane. S4 full qualification up
to24h per admitted lane under the existing release/quality scheduling authority.
Stop at each duration cap, retain incomplete status and seek a new bounded plan;
never drop cases, overlap another release window, lower gates or claim partial
completion. Additional device memory cap64MiB; no new graphs/buffers for ExtentCap.
No GPU request is made by this commit, and no GPU work has been run for P17 here.
