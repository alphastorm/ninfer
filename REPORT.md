# B2 durable serving restore

## Qualification state

- Branch `rebase/serve-restore`; serving source through `687a31d06`, production runner `37a2824a`. This is the Main-approved **source union, not an integration-qualified checkpoint**: integration `ec18847bf`, Serve `141839f8`, Frontend `8c3102905`, Store `550fe006`, corrected Store `202b17064` plus legacy/root closure `148384bda`, and the approved shared declarations.
- B2 host result at `5f8b2ac8bda4bd9eb91c65b2e7c2e06eafea314c`: **2/2 passed, zero failures/skips**, CTest **0.09 s**. `B2-HOST.json` records the exact evidence scope.
- Static `ninfer_serve` archive compiled at `fd4a7e58f78094b480d79fd1a76d126aa015d3c0`: **637 Ninja edges, exit 0**. This precedes the rooted-queue signature change and is not a linked-server or GPU proof.
- Corrected A1 source `148384bda50c0eac475ee6c77b245244df4934c1` separately passed **6/6 host targets**, zero failures/skips, **2.53 s** CTest. Its immutable [report](https://github.com/alphastorm/ninfer/blob/a20fcf74e/REPORT.md) remains standalone provider evidence, not a combined integration verdict.
- **B2 acceptance remains open:** no production-path GPU restart receipt has yet been exercised. EngineCache2 owns the single combined native-policy/stale-epoch/C2 plus B2 restart GPU batch after the final source union links and its CPU tests pass. B2 has submitted no duplicate GPU job.
- Frozen contract changes were approved by Main: typed Reader failure/fresh-reader access, pinned-reader reclaim oracle, `Unavailable` prepare/import states, explicit configured root for read-queue construction, refusal-only legacy-schema identification, and native victim validation before destructive reclaim. Shared declarations/docs remain integration/Contracts-owned.
- RepoPrompt preservation baseline: `prep-v0-13-0-upstream-re-DF08C8`; final G4 review remains Main-owned. No release/tag/image publication or production service change was performed.
- Latest closure commits beyond the frozen `37a2824a` input are `9f9f83ab4` (state docs/help), `1bab66130` (early-stop missing-provider refusal), `0dddecc83` (native progress owns pressure retry cadence), and `687a31d06` (Capacity remains Pending). They are source-complete but not covered by the recorded final-union CPU/model evidence. No claim of completed B2 acceptance is made.

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
- `tests/test_serve_restore_real.py` exercises actual `ninfer-serve` processes and HTTP, not the tracer: final-save below the automatic threshold; first keyed/no-parent restart with at least 16K cached tokens and byte-identical continuation; restored-parent chaining; explicit K7 publication; scoped retrieve/delete; persisted ancestor deletion with a retained child; and a no-prefix-reuse refusal. It hashes the actual model/binary, reads clean binary source provenance, retains failed-run evidence privately and emits only content-safe counts/hashes on success. It has not yet been run.

## K4 failure matrix

| Row | Exercised evidence | Result / limit |
|---|---|---|
| Payload corruption | A1 `test_transaction_restart_compatibility_and_corruption`, `test_post_verification_replacement_fails_closed` | PASS, streamed digest failure; no import, corrupt generation quarantined and prior immutable generation retained. Fake native payloads, real store/Reader/filesystem. |
| Manifest corruption | A1 `test_v3_manifest_validation_and_publication_guard` | PASS, noncanonical/invalid v3 metadata rejected before import. |
| Wrong MAC versus missing MAC under strict policy | A1 `test_manifest_origin_authentication`, `test_manifest_origin_compatibility_window` | PASS, wrong MAC is Corrupt; strict missing MAC is reversible OriginRequired with generation intact. |
| Capacity refusal | B2 `native_refusal_matrix`, `store_refusal_matrix`; existing Responses atomic-store suite | PASS with deterministic native result fake and real Responses capacity checks; distinct CapacityRefused, old records/bytes intact, no unrelated DAG eviction. Does not prove native physical-pool admission. |
| Fingerprint/identity mismatch and foreign collision | A1 KV-layout/fingerprint tests; B2 store-refusal matrix | PASS, clean refusal/no compatibility quarantine; foreign ID collision never invokes native commit. |
| Legacy v2 root with strict policy and bad MAC | A1 `test_legacy_schema_bad_mac_is_refused_without_mutation`, qualified at `148384bda` | PASS, versions 0/1/2/-1, escaped schema key/whitespace and prior corruption mark: every root entry/content unchanged and no native payload import. Values below 3 are refusal-only before MAC; eligible generations remain MAC-gated. |
| Transient filesystem/queue failure and retry | A1 `test_manifest_origin_transient_tag_failure_preserves_current`, `test_reader_failure_reasons_and_fresh_retries`; B2 native-refusal matrix | PASS, current preserved, typed IoUnavailable, fresh single-pass Reader after cause clears, no partial Responses publication. No sleep-based automatic retry. |
| Partial stage/deferred write failure/crash-shaped restart | A1 transaction/restart and buffered deferred-failure tests; low-level commit-failure tests | PASS, interrupted staging is ignored and prior current remains usable. Deterministic crash-shaped filesystem state, not a production process-kill proof. |
| Stale export epoch | A1 publication-guard fake | PASS, exact Stale/StalePublication and no current replacement. The real Engine epoch race is assigned to the combined A3 GPU batch; not yet exercised. |
| Stale Responses revision/active publication | B2 store-refusal matrix and existing Responses suite | PASS, native commit not called; deletion/publication races preserve authoritative live state. |
| Native semantic corruption quarantine | A1 `test_native_corruption_quarantine_is_generation_bound` | PASS, older generation cannot remove newer current; active Readers retain payload paths. Native semantic validation itself is A2/A3-owned. |
| Warmup/cache-disabled import | B2 preserves injected PolicyDisabled; production binding occurs after warmup | Host result propagation PASS. Direct native pre-reservation policy proof and real no-prefix request proof remain in the combined A3/B2 GPU batch. |
| Successful production restart | Static archive only; real runner registered as `ninfer_serve_restore_real_test` | NOT RUN. No tracer or host-fake result is substituted for this acceptance gate. |
| Scoped retrieve/delete and fresh publication order | B2 success/namespace matrix and existing Responses suite | PASS host-only. Real HTTP/durable deletion proof is included in the production runner, not yet exercised. |
| Missing final-save provider on remembered stop | Extended `ninfer_http_server_stop_test` at `1bab66130` | Added regression is not yet exercised: persistence-disabled remembered stop stays usable; an enabled root cannot acknowledge an early stop before providers attach. Included in final CPU union request. |

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

## Deliberately unclaimed and external requirements

- No linked production A3 server, GPU/model execution, integrated failure-matrix verdict, C2/32-GB release gate, native Windows/MSVC/DirectStorage execution, full CTest, performance claim, formatter/linter, or final review is claimed by the evidence above.
- Main owns release-isolated **default** roots in product/container lifecycle tooling. NativeOps uses `session-checkpoints-v3`; the container host directory/mount must likewise be new rather than reusing the previous release's checkpoint root. B2 consumes only the explicitly configured root and performs no root auto-discovery or legacy migration.
- Windows stop-event ACL/controller behavior and selected-GPU attestation remain their owning lane's proof. The production runner uses Linux SIGTERM through the same synchronous serving final-save callback; that is not a claim of Windows event execution.
