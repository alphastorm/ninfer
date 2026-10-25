# B2 durable serving restore

## Qualification state

- **B2 acceptance PASS at exercised A3+B2 union `d693c6c7ca805cdb5a6ab4d7688ce600aa58f6b4`.** Authored branch `rebase/serve-restore` through `b44efcf5a9914c014fd6d5884d4f30c66ffff926` is the historical Main-approved source union, not the exact built proof input. Integrator3 requested this REPORT-only closure: no additional provider merge/rebase, no claim that the authored branch itself was GPU-built.
- B2 host result at `5f8b2ac8bda4bd9eb91c65b2e7c2e06eafea314c`: **2/2 passed, zero failures/skips**, CTest **0.09 s**. `B2-HOST.json` records the exact evidence scope.
- Shared A3+B2 CPU source `6a6bfa3db3ebfa9a63a4475dbc4f033e8e97f896`: production server linked, **12/12 scoped CTests passed, zero skips, 0.59 s**, including final B2 early-stop regression. This is EngineCache2-owned evidence, not a B2 rerun.
- Corrected A1 source `148384bda50c0eac475ee6c77b245244df4934c1` separately passed **6/6 host targets**, zero failures/skips, **2.53 s** CTest. Its immutable [report](https://github.com/alphastorm/ninfer/blob/a20fcf74e/REPORT.md) remains standalone provider evidence, not a combined integration verdict.
- EngineCache4's changed-input lease `582334` passed **E2 plus all four production boots**, exit **0 / 201 s**, no leftover/unowned containers; four-boot phase **130 s**. Original content-safe receipt: `8e4d03d59f625eac87b37854c1e02e4b3361467c:receipts/engine-cache/serve-restore-582334.json`. B2 read that receipt; B2 submitted no duplicate GPU job.
- Host runner source `1510dfdc392da6880fb1d5c3e2ba08cf4d403c36`: `python3 -B tests/test_serve_restore_runner.py` → **5 tests, OK, exit 0**. Integrator3 owns shared-CMake registration and integrated RC verification.
- Frozen contract changes were approved by Main: typed Reader failure/fresh-reader access, pinned-reader reclaim oracle, `Unavailable` prepare/import states, explicit configured root for read-queue construction, refusal-only legacy-schema identification, and native victim validation before destructive reclaim. Shared declarations/docs remain integration/Contracts-owned.
- RepoPrompt preservation baseline: `prep-v0-13-0-upstream-re-DF08C8`; full integrated RC and G4 remain Integrator3/Main-owned. No release/tag/image publication or production service change was performed by B2.

### Qualification boundaries and resolved failures

- Shared CPU receipt remains at `f0a54eb99f6d6c090a90d6a51eb84a26611524b3:receipts/engine-cache/a3-host-6a6bfa3d.json`. Its source predates the direct-span and runner closures; final GPU evidence is attributed only to exact union `d693c6c7…`, never retroactively to this CPU input.
- PortableCore2 reports the combined MSVC source `2a4266e30` fails because `generation_service.h` uses `std::span` without its direct header. B2 adds `<span>` explicitly; no compile rerun is claimed. Its reported incomplete `std::array` in public `types.h` and POSIX-only request-log test are outside this slice and remain integration-owned.
- EngineCache3's real batch at `9e2261f92b9cbca8b44933e5064cbdd2978a9400` failed **before HTTP/save**, with `incomplete or invalid checkpoint runtime fingerprint v2`: its actual CMake cache and binary version declared `NINFER_UPSTREAM_BASE_SHA=unknown`. K2 correctly refused unknown provenance. The old driver's cleanup masked that first startup failure as a stop-acknowledgement failure; no shutdown report or restart receipt existed.
- Corrected build input is `-DNINFER_UPSTREAM_BASE_SHA=64d10fbf9f8e6bfc79ab0570b94674c5b9997929`, not a fingerprint fallback or validator relaxation. The runner now rejects incomplete upstream-base provenance before starting a GPU child and preserves the first exception, chaining any cleanup failure. Actual argv attestation remains per boot; durable effective-config identity already excludes shutdown-report paths and the prefix-reuse policy switch.
- Main's binding K5 ruling preserves root/global-`--no-prefix-reuse` CLI refusal and its parser regression. Corrected Boot 3 uses normal startup warmup (`CacheParticipation::Disabled`) and keyed `store:false`; it now passes with explicit zero cached tokens, unretained probe 404, unchanged current through shutdown. No HTTP cache-disable extension or parser relaxation was invented.
- Native immutable-identity fix `12ae6ee3c157462f4dde009f59eacc47f88c1595` is compiled and GPU-qualified in `d693c6c7…`. Runner provenance/error closure `33ea3f13` is consumed as `fff4f17c`; Boot 3 fix `6e4cca2e` as `13e5172a7`; obsolete override removal `1510dfdc` as `d693c6c7c`. Final receipt commit is `8e4d03d59f625eac87b37854c1e02e4b3361467c`.
- Earlier prefix/preemption/direct-adapter passes retain their exact-source evidence; unchanged E1/startup/prefix/preemption inputs were not repeated. Failed/partial receipts remain failure evidence, superseded only for the changed E2/four-boot gates.

### Production receipt: PASS, actual serving path

Both original receipts were read from the EngineCache lane; receipt ownership is not reassigned to B2.

| Evidence | Observed result |
|---|---|
| Exercised source | `d693c6c7ca805cdb5a6ab4d7688ce600aa58f6b4` |
| Binary SHA-256 | `1563599f6dfa599578bf53048aa0b2f2100cfab0316d57cfe5a38254ebf74e0f` |
| Model artifact SHA-256 | `81f924d440c27261d820c19a9f8d45794c5aee410f8a68bd358133fa8c0375da` |
| Short-session graceful stop | `saved=1`, `skipped=0`, `refused=0`, `complete=true` |
| First keyed/no-parent restart | **20,926 cached tokens**; **27-byte identical** continuation, SHA-256 `5a2528e22c6a6603d5eeafd214223e57812a9da7174bc1a4445c8f9308f8344c` |
| Explicit K7 save | `exported`, `reason=none`, `committed=true`, authenticated, `fsync`; **2,164,056,150 B**, **4 points / 4 states / 329 pages**, frontier/restored **20,993** |
| Warmup/unretained request | Cached tokens **0**, unretained retrieve **404**, current unchanged |
| Scoped and persisted deletion | Foreign/missing capability **404**; own retrieve/delete **200**; after restart deleted ancestor **404**, retained descendant **200** |
| E2 overlap | **1/1 passed, zero failures/skips, 46.6706 s**; Pending preserves victim, publication permits reclaim, import returns while sibling decodes; restored C2 output equals fresh deterministic tokens |

Original evidence at commit `8e4d03d59f625eac87b37854c1e02e4b3361467c`:
`receipts/engine-cache/serve-restore-582334.json` and
`receipts/engine-cache/a3-e2-582334.json`. Receipts contain counts, timings, hashes and relative names, not prompts, keys, raw session identifiers, UUIDs or private paths.

## Provider bindings

| Boundary | Serving binding | Authority and acknowledgement |
|---|---|---|
| Native response tag and submission epoch | `src/serve/generation_service.cpp:345`, `bind_checkpoints` | Installs only `CheckpointEngineAccess::set_checkpoint_tag` and `publication_epoch`; B1 captures the epoch immediately after native submission. No request-counter durable fallback. |
| Once-resolved identity | `src/serve/session_checkpoint_service.cpp`, `startup_fingerprint` | Actual loaded native layout/frontend getters, resolved `MemorySummary` KV/prefill values, clean build provenance and actual configured artifact/binary identities. The principal scope is passed from GenerationService's once-computed authenticated scope. |
| Rooted native Reader backend | `store_options` | Calls `make_read_queue(engine, configured_root)`; real platform factory, no invented path/rootless shim. The adapter owns selected-device construction, including DirectStorage's LUID binding. |
| Staged export | `src/serve/session_checkpoint_service.cpp:216`, `save_session` | A1 Writer/finish surrounds real `export_session`; Responses snapshot and exact newest tag are fixed for the attempt. Bulk writes/hashing are not under the Engine execution lock. |
| Final current replacement | `save_session` publish callback | Passes the export's captured epoch to real `publish_export`. A1 releases its catalog mutex before entering the gate; only the final replace/sync closure runs under it. Stale never publishes. |
| K4 restore | `src/serve/session_checkpoint_service.cpp:163`, `restore`; `src/serve/openai_responses_store.cpp:587` | Responses guard and native session epoch precede disk I/O. A1 verifies the generation; the trusted active fingerprint reaches `prepare_import`; complete replacement preparation precedes the no-throw native commit and store swaps. Responses never calls the convenience `import_session` path. |
| Native corruption handoff | `restore` | Generation-specific A1 quarantine only after `Corrupt`; capacity, identity, policy and unavailable outcomes do not quarantine. A missing provider transaction is unavailable, not disk-corruption evidence. |
| Reclaimability | `src/serve/session_checkpoint_service.cpp:317`, `pin_recoverable` | Fresh `load(active_fingerprint, tag)`, exact newest-tag equality and retained engine Reader. A3 validates the victim's bytes/native shape and rechecks current/tag before destructive reclaim; `covers()` is only a save-skip predicate. |
| Pressure checkpoints | `save_before_eviction` | Worker callback only enqueues; publishing replies remain Pending without exporting an older snapshot. Matching publication completion/abandonment notifies off the Responses mutex. The A3 progress coordinator owns retry cadence; B2 retains tagged Pending/Settled outcomes rather than a second, incomplete progress clock. Busy/stale/I/O/capacity refusal remains Pending. Reported nonretryable policy and superseded/absent replies may settle under the frozen handler contract. |
| Graceful final save | `src/serve/http_server.cpp:678`; `src/serve/session_checkpoint_service.cpp:429` | HTTP drain precedes synchronous queue drain and final saves, including short sessions. Counts reflect actual saved/covered/refused outcomes. Log-only allocation failure cannot revoke an acknowledgement. A1's actual durability mode appears in successful K7 receipts. |
| Managed shutdown report | `src/serve/session_checkpoint_service.cpp:406` | Flushed exclusive staging file, atomic replacement and platform directory-sync policy. Schema-1 report carries managed launch ID and saved/skipped/refused counts; a report-write failure fails the stop result. The existing early Windows stop watcher is preserved. |
| Public wiring and fail-closed startup | `src/serve/http_server.cpp:632,656` | Default attach creates the real providers; enabled persistence requires the complete callback set and native hooks. Explicit callbacks are a deterministic test seam, never installed by the production app. |

The serving adapter owns no native cache implementation. Native Program reservations, ResourceManager publication with a fresh order, bounded victim reclamation and backend construction remain A2/A3 authorities.

## Restore and isolation behavior

- First stored authenticated `prompt_cache_key` request can restore without a parent ID; a missing scoped parent/retrieve/input-items lookup restores only its authenticated lineage. Anonymous or foreign capabilities cannot read/delete private records.
- `commit_openai_response_restore` preserves exact native results, including `Unavailable/IoUnavailable`; store-local capacity, revision, active-publication and foreign-collision failures never invoke native commit.
- Response deletion serializes disk invalidation plus RAM deletion with same-session saves from **before** snapshot capture through publication. The weak mutex table is retired on subsequent admission; no unbounded session tombstone table or global Engine lock is introduced. Store erase Conflict leaves RAM unchanged.
- Explicit checkpoint POST returns the frozen `schema: "ninfer-durable-operation/1"` save receipt (`exported`, `reason=none`, `committed=true`, real counts, origin and `dir_sync`), not the old `state=available` save alias. Checkpoint status retains its schema-3 vocabulary. NativeOps migrated its consumer; Harness/ProductOps reported no affected save-receipt consumer in their assigned files.
- `tests/test_serve_restore_real.py` exercises actual `ninfer-serve` processes and HTTP, not the tracer: final-save below the automatic threshold; first keyed/no-parent restart with at least 16K cached tokens and byte-identical continuation; restored-parent chaining; explicit K7 publication; scoped retrieve/delete; persisted ancestor deletion with a retained child; and warmup/unretained-request opt-out. It hashes the actual model/binary, reads clean binary source provenance, retains failed-run evidence privately and emits only content-safe counts/hashes on success. All four boots passed at exact union `d693c6c7…`.

## K4 failure matrix

| Row | Exercised evidence | Result / limit |
|---|---|---|
| Payload corruption | A1 `test_transaction_restart_compatibility_and_corruption`, `test_post_verification_replacement_fails_closed` | PASS, streamed digest failure; no import, corrupt generation quarantined and prior immutable generation retained. Fake native payloads, real store/Reader/filesystem. |
| Manifest corruption | A1 `test_v3_manifest_validation_and_publication_guard` | PASS, noncanonical/invalid v3 metadata rejected before import. |
| Wrong MAC versus missing MAC under strict policy | A1 `test_manifest_origin_authentication`, `test_manifest_origin_compatibility_window` | PASS, wrong MAC is Corrupt; strict missing MAC is reversible OriginRequired with generation intact. |
| Capacity refusal | B2 `native_refusal_matrix`, `store_refusal_matrix`; real E2 at `d693c6c7…` | PASS: distinct CapacityRefused and unchanged Responses state in host tests; real 758,120,448 B Host pool preserves the victim while Pending, then admits import after verified save publication. No capacity quarantine. |
| Fingerprint/identity mismatch and foreign collision | A1 KV-layout/fingerprint tests; B2 store-refusal matrix | PASS, clean refusal/no compatibility quarantine; foreign ID collision never invokes native commit. |
| Legacy v2 root with strict policy and bad MAC | A1 `test_legacy_schema_bad_mac_is_refused_without_mutation`, qualified at `148384bda` | PASS, versions 0/1/2/-1, escaped schema key/whitespace and prior corruption mark: every root entry/content unchanged and no native payload import. Values below 3 are refusal-only before MAC; eligible generations remain MAC-gated. |
| Transient filesystem/queue failure and retry | A1 `test_manifest_origin_transient_tag_failure_preserves_current`, `test_reader_failure_reasons_and_fresh_retries`; B2 native-refusal matrix | PASS, current preserved, typed IoUnavailable, fresh single-pass Reader after cause clears, no partial Responses publication. No sleep-based automatic retry. |
| Partial stage/deferred write failure/crash-shaped restart | A1 transaction/restart and buffered deferred-failure tests; low-level commit-failure tests | PASS, interrupted staging is ignored and prior current remains usable. Deterministic crash-shaped filesystem state, not a production process-kill proof. |
| Stale export epoch | A1 publication-guard fake; direct real adapter checks at `fff4f17c79db65e253384a310fa8515e580180cb` | PASS, exact Stale/StalePublication and no current replacement. `a3-e2-580588.json` explicitly records stale epoch rejection before callback; no unchanged rerun is claimed. |
| Stale Responses revision/active publication | B2 store-refusal matrix and existing Responses suite | PASS, native commit not called; deletion/publication races preserve authoritative live state. |
| Native semantic corruption quarantine | A1 `test_native_corruption_quarantine_is_generation_bound` | PASS, older generation cannot remove newer current; active Readers retain payload paths. Native semantic validation itself is A2/A3-owned. |
| Warmup/cache-disabled import | Host propagation; direct adapter at `fff4f17c…`; production Boot 3 at `d693c6c7…` | PASS: direct check refuses before Reader/reservation; real warmup/unretained request reports zero cache, unretained 404, unchanged current through stop. Global root plus `--no-prefix-reuse` remains a shipped CLI refusal. |
| Successful production restart | Original `serve-restore-582334.json`, `production_path=true`, `tracer=false` | PASS, four boots: first keyed/no-parent restore hits 20,926 tokens, exact 27-byte continuation, committed authenticated/fsync save and successful managed final-save. |
| Scoped retrieve/delete and fresh publication order | B2 host namespace/fresh-order matrix; original four-boot receipt | PASS host fresh-order checks and real scoped HTTP behavior; foreign/missing 404, own 200, deleted ancestor still 404 and retained descendant 200 after restart. |
| Missing final-save provider on remembered stop | Extended `ninfer_http_server_stop_test` at `1bab66130`, included in shared 12/12 source `6a6bfa3d…` | PASS: persistence-disabled remembered stop remains usable; enabled root cannot acknowledge before providers attach. No Windows stop-event execution is claimed. |

## Exact exercised commands

`$SOURCE` is a genuine detached Git checkout of the stated source; `$BUILD` is the persistent holder build directory; `$COMMON` is the approved helper directory. Private machine names/absolute private paths are intentionally not copied here.

```sh
export NINFER_BUILD_HOLDER=B2
export NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2-cpu-local
# Source 5f8b2ac8bda4bd9eb91c65b2e7c2e06eafea314c
"$COMMON/build-wsl.sh" "$SOURCE" "$BUILD" \
  -DNINFER_BUILD_PROFILE=rtx5090 \
  -DNINFER_UPSTREAM_BASE_SHA=64d10fbf9f8e6bfc79ab0570b94674c5b9997929 \
  -- ninfer_session_checkpoint_service_test ninfer_openai_responses_store_test
"$COMMON/run-in-env-wsl.sh" --name b2-k4-host --mount "$SOURCE" --mount "$BUILD" -- \
  ctest --test-dir "$BUILD" --output-on-failure \
  -R '^ninfer_(session_checkpoint_service|openai_responses_store)_test$' --timeout 30
# Terminal marker: B2_HOST_BATCH_EXIT=0

# Source fd4a7e58f78094b480d79fd1a76d126aa015d3c0; static archive, not app linkage
"$COMMON/build-wsl.sh" "$SOURCE" "$BUILD" \
  -DNINFER_BUILD_PROFILE=rtx5090 \
  -DNINFER_UPSTREAM_BASE_SHA=64d10fbf9f8e6bfc79ab0570b94674c5b9997929 \
  -- ninfer_serve
# Terminal marker: B2_SERVE_COMPILE_EXIT=0
```

Both batches used the approved detached launcher and supervised two-slot CPU helpers, observed slot 0. No GPU option was used. The wait wrapper's exit code alone was not treated as successful verification.

The first source-union attempt at `99a6b0991` stopped during CMake configure: `ninfer_sha256` was declared twice after the provider union. No test ran. Integrator authorized removal of only the duplicate target (`3cd887d2c`); the changed-source host batch above then passed. The existing target and consumer links were preserved.

### Final GPU batch, executed by EngineCache4

Source is exactly `d693c6c7ca805cdb5a6ab4d7688ce600aa58f6b4`.
`$ROOT` is the holder workspace, `$MODEL_DIR` / `$MODEL` the read-only model,
`$PRIVATE` native-ext4 TMPDIR, and `$RECEIPTS` per-source evidence. The existing
`engine-cache-e2.sh` runner was restricted to the two changed-input phases.
No B2 GPU submission or repeat of unchanged passing scenarios occurred.

```sh
gpu-lease submit --holder E1 --label engine-cache-final-overlap-restart \
  --priority 2 --minutes 15 -- bash engine-cache-e2.sh \
  d693c6c7ca805cdb5a6ab4d7688ce600aa58f6b4
gpu-lease wait 20261025T065709Z-E1-engine-cache-final-overlap-restart-582334 20

export NINFER_BUILD_HOLDER=E1
export NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2
"$COMMON/build.sh" "$SOURCE" "$BUILD" \
  -DNINFER_UPSTREAM_BASE_SHA=64d10fbf9f8e6bfc79ab0570b94674c5b9997929 \
  -- ninfer-serve ninfer_qwen3_5_durable_pressure_real_test
"$COMMON/run-in-env.sh" --gpu --mount "$ROOT" --read-only-mount "$MODEL_DIR" \
  --env "NINFER_TEST_ARTIFACT=$MODEL" --env NINFER_TEST_BACKEND=dflash2 \
  --seccomp "$COMMON/ninfer_io_uring_seccomp.json" -- \
  ctest --test-dir "$BUILD" --output-on-failure --timeout 600 \
  --output-junit "$RECEIPTS/dflash2.xml" -R '^ninfer_qwen3_5_durable_pressure_real_test$'
"$COMMON/run-in-env.sh" --gpu --mount "$ROOT" --read-only-mount "$MODEL_DIR" \
  --env "TMPDIR=$PRIVATE" --seccomp "$COMMON/ninfer_io_uring_seccomp.json" -- \
  python3 -B "$SOURCE/tests/test_serve_restore_real.py" \
  --server "$BUILD/apps/ninfer-serve" --artifact "$MODEL" \
  --artifact-sha256 81f924d440c27261d820c19a9f8d45794c5aee410f8a68bd358133fa8c0375da \
  --port 18151 --receipt "$RECEIPTS/serve-restore.json"
```

## Deliberately unclaimed and external requirements

- Historical B2 CPU commands do not establish an A3 server link or GPU proof; the separately attributed receipts do. This report closes the scoped B2 matrix and production restart, not the full integrated RC, C2/32-GB release gate, native Windows/MSVC/DirectStorage execution, full CTest, performance qualification, formatter/linter, or G4 review.
- Main owns release-isolated **default** roots in product/container lifecycle tooling. NativeOps uses `session-checkpoints-v3`; the container host directory/mount must likewise be new rather than reusing the previous release's checkpoint root. B2 consumes only the explicitly configured root and performs no root auto-discovery or legacy migration.
- Windows stop-event ACL/controller behavior and selected-GPU attestation remain their owning lane's proof. The production runner uses Linux SIGTERM through the same synchronous serving final-save callback; that is not a claim of Windows event execution.
