# P20 copy drafting: rebase retained, revival declined

## Decision

This draft retains **7a only**: the four S1 commits rebased onto
`e6ac7b709a1a3f94fbbcd6529ce87e813d20cd09`, with compatibility corrections.
Copy drafting stays default-off, DFlash2-only and disabled at three or more active
lanes. It is **not in the P20 release candidate**. No new GPU experiment or quality
screen was authorized. The first step of any revival is the deferred exact-base
output-identity proof below; historical S1 receipts are not evidence for this rebase.

The lead's B1 offline replay sampled 3,000 real cloud-agent turns from 649 sessions,
containing 1,764,539 Qwen-tokenized output tokens. There were **no local Qwen turns**;
this is a structural proxy and cost projection, not measured NInfer throughput.

| B1 metric | Fixed minimum 12 | Backoff 12→24→36 |
|---|---:|---:|
| Accepted copied output | 20.5413% | 13.2531% |
| Pooled projected decode ratio | 1.06664–1.09314× | 1.05484–1.06928× |
| Median sampled-session ratio, cost 1.41 | 1.03952× | 1.02016× |
| Edit/hashline output coverage | 39.76% | 31.52% |

Both pooled projections miss the 15% revival bar. Backoff raises precision but
reduces useful coverage. Gains concentrate in edit/hashline and write-shaped turns;
missing local outputs, hidden reasoning and reconstructed/truncated prompts limit
any release inference. The complete B1 decision/evidence is retained in the P20
`B1-COPYABLE` lane, not published transcript text.

## Rebase and RepoPrompt reconciliation

- Original S1 commits: `c925c710a`, `60a95b0e9`, `37e3594b7`, `a05d6e779`.
  Rebased commits: `e16c2a102`, `88219b90c`, `463a3958e`, `2c4427ed0`.
- Kept the base's fresh-admission/reclaim, durable schema 3, origin authentication,
  checkpoint identities and persistence behavior unchanged. Shared serving/header
  conflicts were resolved additively; historical S1 numbers remain historical.
- The base moved speculative accounting to commit and added attempted-position
  histograms of size K. Retaining S1's decode-time increments would double-count;
  feeding a 15-copy extent to the K7 neural histogram would overflow it. Copy
  verification now uses `observe_copy_round` at the existing commit decision;
  neural histograms retain K entries and exclude copied-token work.
- Copy request-log counters are emitted only when enabled. CPU goldens cover the
  unchanged disabled speculative JSON block and omitted versus explicit-zero
  options for ordinary, MTP, DFlash and DFlash2 configurations.
- RepoPrompt preparation `prep-p20-engine-package--A95667` adopted the 7a/7b split.
  U10 is resolved: copy verification has **16 columns**, neural K7 has **8**. The
  shipped profile's prefill chunk is **1024**, not the plan's 4096. The actual S1
  proposer uses 16/8/4-token hash windows, minimum match 12 and two-token source
  lookahead; the plan's inferred replacement algorithm was not adopted.
- The proposed 7b module, prefill-side index, miss backoff and exact PR #277 C2
  partner ledger were removed from the working tree when the lead stopped revival.
  They are not present in this draft or represented as qualified behavior.

## Zero-copy identity finding

The S1 receipt does **not** support the initial claim that all divergent depth/log
requests proposed zero copies. Depth probes proposed 120/105/180/75 copied tokens;
log controls proposed 90/255. Sequential requests with no copy rounds retained
identical enabled/disabled bytes in S1. The divergent rows with no *own* copies were
C2 partners in a widened copy round. This is not an enabled-only neural-numerics
root-cause proof. S1's exact-base comparison itself retained a concurrent constrained
partner exception; its disabled versus explicit-zero comparison matched 23/23.

Source inspection does show additional enabled-only work: S1 lazily allocates and
indexes the committed prompt in its first decode call. It also reserves wider
persistent/frame/replay/mask capacity even though a no-copy round selects the
neural graph family and K7 view. These are concrete investigation targets, not a
measured attribution of every slowdown or evidence that wider capacity changed
zero-copy arithmetic. No new GPU/kernel trace was collected. A future revival must
prove identical no-copy kernels/workspace/graph behavior and byte-identical output,
not merely aggregate throughput.

## Exercised CPU proof and deferred GPU golden

- Base `SpeculativeStats::observe_round(15,15)` with K7 histograms fails under
  AddressSanitizer with a heap-buffer-overflow. The rebased proposer test exercises
  `observe_copy_round(15,15)`, exact totals and untouched neural positions; it passes
  under Apple Clang C++20 AddressSanitizer. This is an accounting integration defect,
  not a claim that unmodified base ever generated copy rounds.
- Exact-source CPU-only sm_120a compilation of `451c9ac908d883ec83dc2ba80515e0ffbd456f0a`
  completed all 660 steps on sf-pc: GCC 13.3, CUDA 13.4.92, clean source, Release.
  `ninfer-serve` SHA-256: `8cd83158a01fa30d68411b50bfbe9957ba72b6bb1f5651c1685646a0bc06d626`.
  No server was started and no GPU inference was performed. Later changes add only
  tests, the prepared probe, harness compatibility and this record.
- PR CI is the authority for the final-head host-test and native-compile results;
  build and host-contract workflow links are retained in the draft PR.

`tools/bench/check_ngram_identity.py` is the prepared, **unexecuted on a GPU** gate.
Run isolated fresh servers for exact base, candidate with the flag omitted, and
candidate with `--ngram-draft-tokens 0`, all otherwise identical, greedy, BF16,
DFlash2 K7, C2, chunk 1024, context/KV 131072, host context 0 and no prefix reuse.
For each arm, bind the actual binary and exact commit, then run:

```sh
python3 tools/bench/check_ngram_identity.py capture \
  --arm "$ARM" --endpoint "$URL" --key "$KEY_FILE" --log "$REQUEST_LOG" \
  --binary "$BINARY" --source-commit "$COMMIT" --out "$EVIDENCE/$ARM"
python3 tools/bench/check_ngram_identity.py compare \
  --root "$EVIDENCE" --out "$EVIDENCE/identity.json"
```

Default arm names are `base omitted zero`. The probe compares actual output
projection bytes **and generated token IDs**, excludes only transport-generated
IDs, and requires zero copy rounds. A separately authorized enabled zero-copy arm
uses `capture --enabled` (requiring enabled counters) and an explicit `compare
--arms base omitted zero enabled` list. Private bytes stay outside the repository.
This four-short-request probe complements, not replaces, the complete S1 golden
corpus and the broader no-pressure greedy invariants.

## If revival is authorized later

First execute the deferred exact-base disabled golden; then implement 7b in
`speculative/copy_draft.{h,cpp}` with CPU tests before any GPU allocation. Preserve
the exact 30/470/1470 thousandths C2 ledger semantics from ashalliants PR #277,
calibrated only for K7; build the own-prefix index beside prefill and keep request
ownership through cancellation/resume. The named iamwavecut short SHAs were not
resolvable; the matching inspected commits are `51917102ac37d64cc7333bc95f67efc489c79c67`
(prefill-side index) and `f76b614c9d275b9d44fd94afb8fbcc4f5b956bd1` (index page handling).

Only after CPU and no-copy identity gates pass should the lead allocate a new
matched base/off/on/on/off performance run, followed by the powered 1,120-case
EXP-085 screen and 80-case determinism candidate captures against the sealed
production arm. That is five performance server arms plus two quality captures,
not reuse of the historical S1 timings as fresh evidence. Neither these jobs nor
the quality-screen arm specification are included in this stopped revival.
