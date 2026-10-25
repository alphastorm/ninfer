# Contracts (K0)

- Branch: `rebase/contracts`; base: upstream `64d10fbf`. Scope: K1–K7 specification, three declaration headers, canonical/MAC fixtures and two host-only tests. No shared CMake/public API files edited.
- `docs/durable/CONTRACTS.md`: outer schema 3 + native set/1 + fingerprint 2; release-isolated roots; canonical authenticated envelopes; deduplicated native tables; VisionItem inventory; exact layout/build identity; staged export and atomic restore; CLI mapping; file-I/O and error/status/ack contracts.
- Headers: `src/runtime/contract/durable_checkpoint_set.h`, `durable_engine_api.h`, `runtime_fingerprint.h`. These are declarations/value types, not native/store/Engine implementations; implementation ownership remains Persistence/EngineCache/DurableStore/ServePort.
- Lead-approved decisions: private immutable session scope + shared principal scope; mandatory `publish_export` gate separate from Engine execution lock; Windows file flush + write-through replacement, directory no-op acknowledged as `ntfs-write-through`; status schema 2 null whitelist, no fabricated required identity/counters; strict-origin flag preserves configured-root and API-key ≥32-byte floor.
- Compatibility choice: assignment excludes host/cache admission capacities from fingerprint, overriding the planning note; effective-config digest hashes only the identity projection. Old fork-v2 generations are incompatible, never migrated or quarantined merely for age.
- Fixtures: two synthetic canonical/authentication envelopes and matching MACs; RFC 4231 HMAC cases; POSIX/Windows publication acknowledgements. They are not a native import/restart corpus and contain only public test-key material.
- Tests: `tests/test_durable_contracts.py` is the independent canonical/MAC oracle; `tests/test_durable_contract_headers.cpp` checks declaration traits and production nlohmann serialization against the same fixtures and integer/Unicode/control boundary vector.
- Verification: priority-2 K0 lease job `20261025T011523Z-K0-contracts-host-223570`, source commit `746b29d2`, exited 0 after 3 seconds; three standalone header compiles passed, C++ encoder reported five vectors passed, Python ran six tests in 0.002 seconds (`OK`). Compiler GCC 13.3.0; Python 3.12.3. No GPU use, local build/test, linter or formatter.
- Commands inside the approved build environment (`SRC` = exact branch checkout, `BUILD` = persistent K0 build directory): for each of the three headers, `printf '#include "%s"\n' "$header" | g++ -std=c++20 -Wall -Wextra -Werror -I"$SRC/src" -x c++ -fsyntax-only -`; then `g++ -std=c++20 -Wall -Wextra -Werror -I"$SRC/src" -I"$SRC/third_party" "$SRC/tests/test_durable_contract_headers.cpp" -o "$BUILD/ninfer_durable_contract_headers_test"`; `"$BUILD/ninfer_durable_contract_headers_test" "$SRC/tests/fixtures/durable"`; `python3 -B "$SRC/tests/test_durable_contracts.py"`.
- Lease invocation: `gpu-lease submit --holder K0 --label contracts-host --priority 2 --minutes 5 -- bash contracts-host.sh`; awaited with `gpu-lease wait 20261025T011523Z-K0-contracts-host-223570 10`. Temporary transport script only entered the common build environment and ran the commands above; it is not a repository artifact. Strict-origin and lead-approved K6 `io::remove_tree`/`io::replace_path` clarifications change prose only, not the tested headers/fixtures/tests.
- Registration requested from Integrator: standalone C++ test with `src` and `third_party` include roots, fixture-directory argument; Python CTest invocation `Python3_EXECUTABLE -B tests/test_durable_contracts.py`. Headers need no link source registration.
- Not claimed: production MAC/parser validation, native codec/import, Engine transaction implementation, crash/restart proof, Windows build/runtime, G1/G2 or full K4 failure matrix. Owning slices/integration supply those gates.
- Main-approved implementation-only follow-up: `durable_reason_name` centralizes frozen K7 spellings beside the enum, with compile-time assertions in the existing header test. This addition is not covered by the original K0 proof; Integrator owns verification in its next batched job, with no separate K0 lease.
- Provenance: derived from upstream/fork interfaces and approved RepoPrompt plan `prep-v0-13-0-upstream-re-DF08C8`; no community code copied. No publication/tag/release action performed.

# S1 — own-prefix n-gram copy drafting

## Decision: NO-GO for product inclusion

The implementation and bounded experiment are complete on `rebase/ngram-draft`; nothing
has been merged, published, enabled in a profile, or approved for a release candidate.
The measured edit-speed improvement does **not** satisfy the complete acceptance gate.

- **Source identity:** exact starting integration commit
  `9eb0a5fb9951527adaa2b262fac8ed1124eb83b4`; implementation commits `c925c710` and
  `60a95b0e`. The measured feature binary is from
  `60a95b0e91e4eee32b6982ccd9c6b27161659b5d`, SHA-256
  `91ce5b48a40a4a103e68e45a4e06bf25506de933393647de910ace211bb4f23c`.
  The separately built, unmodified-base binary is
  `d4c6317a67dc342c5ae3660c10fa3b64da50e35ba8380d8c22e9adc75246b5a3`.
  `37e3594b` adds the reference comparison and CPU receipt; subsequent report/schema
  documentation does not change the measured C++ implementation.
- **Approved interface:** `SpeculativeOptions::ngram_draft_tokens` is `uint32`, default
  zero, accepts only `0|15`, and requires DFlash2 when enabled. CLI and server expose
  `--ngram-draft-tokens`. Three default-zero `uint64` counters—`ngram_rounds`,
  `ngram_drafted_tokens`, `ngram_accepted_tokens`—are verified-draft subsets and appear
  in the request-log speculative object. The positional histogram remains neural-only.
  Main approved this shape; no frozen durable contract or profile changed.
- **Implementation map:** `program/speculative/ngram_proposer.h:25` indexes only the
  request's committed prefix, verifies matches, uses 16/8/4-token hashes with a minimum
  12-token match and two-token source lookahead, and allocates no per-proposal token
  corpus. `src/ops/sequence/copy_proposals.cu:27` overlays deterministic one-hot sparse
  proposals. The existing verifier and RNG domains are unchanged. C1 copy rounds skip
  neural drafting; C2 can combine copied and neural rows. Three or more active lanes
  never copy. `program/graphs.cpp:116,546` separates neural/copy topology families;
  round buffers, replay views, pending widths, feature appends and grammar row strides
  follow the selected width. `program/grammar_masks.cpp:12,21` contains the two
  Persistence-approved wide-row offsets. Decode timing includes host lookup/index work.
- **Community provenance:** studied `ashalliants/master` at
  `0312a1d76cd701270ab71caca116e6f75520cad9`, especially `fdcc3494c`, `afe2f253c`,
  `5551e0921`, `5b32c6f09`, `e5acb18fe`, and `scripts/run.bat`'s DFlash2/one-or-two-lane
  launcher gating. The proposer is reimplemented from the attributed Infernix/remesis
  design, not copied source. Community-reported +42% agent replay, +53–76% write-back
  and −0.2..−0.4% off-policy results are references, not results of this branch.
  Community partner-balance/backoff refinements were not transplanted.
- **CPU proof:** approved two-slot CPU service, Release/Ninja, `sm_120a`, 12 workers,
  CUDA 13.1.2. All seven requested targets built; the final two changed translation
  units rebuilt successfully. The separate exact-base server build also passed.
  `ninfer_qwen3_5_ngram_proposer_test` passed under CTest, total 0.06 seconds, on
  `c925c710`; its header/test are unchanged in the measured implementation.
- **GPU proof:** one priority-5, 60-minute lease,
  `20261025T041541Z-S1-ngram-copy-abba-495728`, completed in 908 seconds with exit zero,
  zero leftover containers stopped and zero unowned containers left running. RTX 5090,
  driver 621.01; exact runtime image and artifact digests are in `execution.json`.
  CLI options, serve options, request-log serialization and CUDA overlay tests passed;
  the overlay test includes eager/graph replay, mixed rows, domains/guards and alias
  rejection. Default-off `ninfer_qwen3_5_native_transactions_test dflash2` passed in
  17.462 seconds. No formatter, linter or project-wide suite ran.
- **Workload provenance:** Main explicitly approved six frozen role controls
  (`log-001`, `log-002`, `repo-001`, `repo-005`, `struct-002`, `struct-003`) plus three
  separately labelled deterministic synthetic Read→Edit→explain chains. The synthetic
  chains use Read tool-history messages and request full-file output after one line
  changes; they are not an existing frozen write-back corpus or actual Edit-tool calls.
  Also measured concurrent copy/control and constrained-JSON/control pairs, and the
  EXP-095 single-request depth probe at 0/32K/64K/120K.
- **Protocol:** one pristine-base reference followed by same-binary off/on/on/off
  arms, 23 requests per arm. Temperature zero; DFlash2 neural K=7, copy K=15,
  BF16 KV, 131072 context/KV capacity, configured concurrency two, two device state
  slots, prefill chunk 1024, no host context and no prefix reuse. The first feature-off
  arm omits the flag; the last explicitly sets zero. Rates are token-weighted within
  each workload group, comparing the median of the two on rates with the median of the
  two off rates; individual depth points also gate acceptance. This is one bounded
  experiment, not a powered statistical qualification.
- **Identity/quality failures retained:** omitted-versus-explicit-zero outputs match
  for **23/23** requests, but pristine-base versus feature-off matches only **22/23**
  (the mixed constrained partner differs). Enabled-versus-disabled greedy output
  matches only **15/23**: both free partners, both log controls and all four depth probes
  differ. Exact full-file edits pass only **1/3** chains in *every* arm, including the
  pristine base. The successful chain gains +66.526%; the reported aggregate edit
  speed is therefore not a qualified three-chain editing result. Mixed constrained
  JSON content is exact. No causal explanation for the identity failures is asserted.
- **Copy activity:** each enabled arm records 225 copy rounds, 3374 verified copied
  proposals and 2856 accepted copied drafts; both disabled arms record zero. These
  counters prove copies were exercised, not that every concurrent pair round used B2.
- **Limits:** no positive-temperature empirical distribution screen, copy-enabled D1
  persistence matrix, other architecture, or later integration-checkpoint qualification
  is claimed. The unchanged sparse verifier consumes a delta proposal distribution;
  this is not evidence of trajectory equality. The measured failures block product
  inclusion independently of those additional integration gates.
- **Evidence/privacy:** `docs/performance/ngram-draft/{cpu,execution,baseline,0-off,1-on,2-on,3-off,summary}.json`
  contains source/binary/input/output hashes, counts, timings and explicit failures,
  not prompt/output text, checkpoint bytes, credentials, private paths or host names.
  All imported measurement receipts passed the private-marker check before writing.
  The first waiter expired while the job was still queued; it was not a failed GPU
  measurement. A partial archive transfer was retried with a longer transport deadline.

### Measured same-binary decode changes

| Workload | Copy-on versus copy-off |
|---|---:|
| Synthetic full-file edit group | +53.434% |
| Short Read acknowledgements | **−4.205%** |
| Explanations | +0.016% |
| Six frozen role controls, aggregate | +0.211% |
| Depth sweep, aggregate | +0.266% |
| Concurrent copy request | +48.784% |
| Concurrent free partner | **−0.682%** |
| Constrained copy request | +41.455% |
| Constrained free partner | +2.668% |

Individual 0/32K/64K/120K depth changes are **−1.061% / −0.584% / +3.340% /
−0.619%**. The short-read, free-partner and three depth losses exceed the allowed
0.5%; greedy equality and exact editing also fail. Recommendation: retain this as an
isolated experiment, **do not merge or enable it in the product**. Do not replace the
failed cases with a favorable subset.

Main accepted this NO-GO and authorized publishing the isolated branch as the experiment
record. Two conditions would make reconsideration worthwhile: **(1)** gate copy proposals
to edit/write-back turns only, leaving short Read turns untouched; **(2)** isolate and
resolve the cause of enabled/off greedy divergence, currently equal on only **15/23**
requests. Neither change is implemented or claimed by this experiment.

### Commands exercised

Variables below denote the owned source/build/staging roots and ephemeral key file;
no private absolute paths are published.

```sh
NINFER_BUILD_HOLDER=S1 \
NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2-cpu-local \
bash "$COMMON/build-wsl.sh" "$SRC" "$BUILD" -- \
  ninfer-serve ninfer_qwen3_5_ngram_proposer_test ninfer_copy_proposals_test \
  ninfer_cli_options_test ninfer_serve_options_test ninfer_request_log_test \
  ninfer_qwen3_5_native_transactions_test

# Header-only proposer, once; unchanged by the subsequent C++ correction.
ctest --test-dir "$BUILD" -R '^ninfer_qwen3_5_ngram_proposer_test$' --output-on-failure

# Separate detached exact-base source/build, same environment and configuration.
bash "$COMMON/build-wsl.sh" "$BASE_SRC" "$BASE_BUILD" -- ninfer-serve

gpu-lease submit --holder S1 --label ngram-copy-abba --priority 5 --minutes 60 \
  -- bash "$STAGE/measure.sh"
gpu-lease wait 20261025T041541Z-S1-ngram-copy-abba-495728 65
# The first waiter expired in the queue; continued waiting without resubmission.
gpu-lease wait 20261025T041541Z-S1-ngram-copy-abba-495728 180

ninfer-serve "$MODEL" --host 0.0.0.0 --port 18180 --api-key-file "$KEY" \
  --model-id q38-ninfer --max-context 131072 --kv-capacity 131072 \
  --max-concurrency 2 --device-state-slots 2 --host-context-mib 0 \
  --prefill-chunk 1024 --spec dflash2 --draft-tokens 7 --lm-head-draft \
  --kv-dtype bf16 --no-prefix-reuse --request-log-jsonl "$REQUEST_LOG" \
  --log-stats-interval-ms 0
# Baseline and 0-off omit the flag; 1-on/2-on add --ngram-draft-tokens 15;
# 3-off adds --ngram-draft-tokens 0.

python3 run_ngram_draft.py run --arm "$ARM" --copy-window "$WINDOW" \
  --base-url http://127.0.0.1:18180 --api-key-file "$KEY" \
  --request-log "$REQUEST_LOG" --probe "$PROBE" --qualification "$QUALIFICATION" \
  --long-fixture "$FIXTURE" --binary "$BINARY" --source-commit "$SOURCE_COMMIT" \
  --artifact-sha256 81f924d440c27261d820c19a9f8d45794c5aee410f8a68bd358133fa8c0375da \
  --receipt "$RECEIPT"
# The pristine-base invocation additionally supplies --reference.
python3 run_ngram_draft.py compare 0-off.json 1-on.json 2-on.json 3-off.json \
  --baseline baseline.json --receipt summary.json

ninfer_cli_options_test
ninfer_serve_options_test
ninfer_request_log_test
ninfer_copy_proposals_test
NINFER_TEST_ARTIFACT="$MODEL" ninfer_qwen3_5_native_transactions_test dflash2
```
