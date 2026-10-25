# Council remediation — Lane A

- Council `CR-20261025T085950Z-04b603aa`; findings **1, 2, 6, 9** closed on branch `rebase/rem-serve`. Frozen predecessor: `04b603aae4e9ce3c26f4175cf808180c0f05b5fe`.
- Exercised A+B source: **`3c517249d7573db47fcdbd6deeaa7d844323ec1f`**. Approved secondary CPU lane, holder **`rem-serve`**, build **slot 1**, detached job **9840**; terminal marker **`REM_SERVE_HOST_EXIT=0`**. Content-safe receipt names: `job-r3.log`, `ctest-r3.xml` in holder scratch.
- Raw evidence SHA-256: `job-r3.log` = `fa9fabfde20f9cd1d8d450651933eedf6f7f525fe5c31acb15ef268a0b07c5ad`; `ctest-r3.xml` = `9e2b21c9980c34d462dd4631cc358d7ac8975597e6aa49f88527350798a43631`. Raw files remain outside the repository; this report is their content-safe result summary.
- Build completed all requested targets, including production **`ninfer_serve` static archive** and eight host executables; final incremental build **12 steps**. **CTest 8/8 passed, zero failures/skips, 3.31 seconds**. Tests used the real runtime image with an empty visible-device mask under the existing driver lock; no model inference, production-container operation, or GPU qualification was performed.
- `src/serve/automatic_checkpoint_queue.{h,cpp}` (`cpp:125–240`) now owns the production pressure coordinator. The Engine callback only consults bounded bookkeeping and enqueues; Responses/catalog inspection and save work run on its checkpoint thread. Every completed task notifies Engine progress, including retryable refusals. Existing `TransientRetryBudget` is wired to six retries, not an unconditional retry loop: race retries observe Engine/publication progress, Capacity observes Engine progress, I/O observes successful storage progress. Waiting for HTTP publication consumes no save-attempt budget and only publication progress rearms it. Exhausted transient pressure remains Pending; reported nonretryable refusal still settles.
- `src/serve/session_checkpoint_service.{h,cpp}` (`cpp:200,258,327–465`) wires the coordinator, the Engine's published-endpoint acknowledgement, and separate real-progress observations. Native K4 prepare/commit remains atomic under the existing Responses lock. `/v1/ninfer/status` fields and launch/profile values are unchanged.
- `src/serve/openai_responses_store.{h,cpp}` (`cpp:207–268,619–628`) adds durable single-record deletion and restore freshness checks. DELETE preallocates and publishes survivors before erasing RAM, retains undeleted disk-only records, and keeps prior authority on refusal. Native-unavailable/racing or deleted endpoints publish authenticated replay-only survivors rather than introducing a leaf-delete 409. Whole-session erase is used only when no RAM or persisted survivor remains. Restore refuses endpoint-incompatible live state or any live record absent from the loaded generation; it never compares cross-restart numeric orders.
- Shipped behavior was checked with `git show 1302d639:src/serve/responses_http.cpp`: authorized leaf deletion returned success without a special refusal. The proposed conservative leaf 409 was therefore removed under Main's instruction. Parent/ancestor and leaf success semantics remain intact; the authenticated replay-only generation is the approved native-unavailable representation.
- New cases in `tests/test_automatic_checkpoint_queue.cpp`: `pressure_callback_does_not_wait_for_restore_commit`, `automatic_retry_requires_matching_progress`, `automatic_retry_budget_and_terminal_refusal`, `publication_abandonment_completes_pressure`, `durable_record_delete_preserves_survivors`, `durable_delete_retains_disk_only_records`. They exercise the production coordinator, real threads, the real Store/filesystem, immediate reopen without final-save, and file-backed host reads; native export/import is a deterministic host fixture.
- `tests/test_openai_responses_store.cpp:280` adds `test_restore_guard_captured_after_newer_turn`: real LRU eviction, newer unsaved endpoint before guard capture, and an Engine callback willing to commit. Existing native-refusal fixtures in `tests/test_session_checkpoint_service.cpp` and the store suite were changed to use endpoint-compatible partial state, preserving their original native-refusal coverage rather than bypassing the new freshness guard.
- Superseded verification remains failure evidence: source `4e6fc26de4b8d826fabba55e4fd71aa09d4784c4` stopped at a new Store test helper's declaration order; no CTest ran. Source `f0a4a3304655665cc0650e1cbc1d066ef5885285`, slot 0/job 29108, built but passed only 6/8: the new deletion fixture omitted its required read queue, and the Store quota plan rejected below-hard-quota re-save after deferred cleanup. The corrected fixture and Lane B's bounded under-cap cleanup fix produced the final green source above; no unchanged-input rerun is claimed.

## Red-at-F assertions

These are **source-derived red proofs, not executions against an F binary**. The assignment explicitly permits identifying the assertion F violates. The new native/schema representation itself is not claimed to have existed at F.

| Finding | Deterministic invariant and F counterexample |
|---|---|
| 1 | `pressure_callback_does_not_wait_for_restore_commit` requires the production handler to return Pending while HTTP owns R and awaits a queued worker commit, then both commit and save completion finish. At F, `session_checkpoint_service.cpp:374–378` calls `responses.await_publication`, whose first operation (`openai_responses_store.cpp:374`) blocks on R. Under the test's controlled schedule the worker cannot return to commit. |
| 2 | `automatic_retry_requires_matching_progress` requires one completion notification after a transient/I/O/Capacity refusal, no retry on unchanged/unrelated progress, and a second attempt after matching progress. At F, `session_checkpoint_service.cpp:342–365` neither wires the budget nor re-enqueues and notifies only when settled; the first completion and later retry assertions fail. The bounded-budget and publication-abandonment cases additionally preserve Pending versus reported terminal-refusal semantics. |
| 6 | `durable_record_delete_preserves_survivors` immediately reopens the real Store after ancestor/leaf acknowledgement, requiring the undeleted record Available and the deleted record absent, without shutdown/automatic repair. At F, `session_checkpoint_service.cpp:299–313` erases the whole session and only then removes one RAM record: the reopened survivor is Missing. The disk-only case prevents incorrectly treating the last RAM record as the last durable record. |
| 9 | `test_restore_guard_captured_after_newer_turn` requires refusal before calling its successful Engine callback and preservation of `resp_new_unsaved`. At F, `openai_responses_store.cpp:539–578` accepts the unchanged captured revision, removes all target-session live entries, and installs the older snapshot: the assertion fails despite no post-capture race. |

## Exercised host targets and commands

All eight passed: `ninfer_engine_checkpoint_control_test`, `ninfer_engine_checkpoint_adapter_test`, `ninfer_resource_manager_test`, `ninfer_openai_responses_store_test`, `ninfer_session_checkpoint_service_test`, `ninfer_session_checkpoint_store_test`, `ninfer_automatic_checkpoint_queue_test`, `ninfer_checkpoint_policy_test`.

The variables below name the approved shared helpers, isolated holder checkout/build, and holder-owned cache/receipt directory; private absolute paths are deliberately omitted.

```sh
export NINFER_BUILD_HOLDER=rem-serve NINFER_BUILD_JOBS=12
export NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2-cpu-local
export NINFER_CCACHE_DIR="$HOLDER/ccache"
"$COMMON/build-wsl.sh" "$SOURCE" "$BUILD" \
  -DNINFER_BUILD_PROFILE=rtx5090 \
  -DNINFER_UPSTREAM_BASE_SHA=64d10fbf9f8e6bfc79ab0570b94674c5b9997929 \
  -- ninfer_serve ninfer_openai_responses_store_test ninfer_session_checkpoint_service_test \
  ninfer_automatic_checkpoint_queue_test ninfer_checkpoint_policy_test \
  ninfer_session_checkpoint_store_test ninfer_engine_checkpoint_control_test \
  ninfer_engine_checkpoint_adapter_test ninfer_resource_manager_test
export NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2
timeout 300 flock -x -F "$DRIVER_LOCK" \
  "$COMMON/run-in-env-wsl.sh" --name remediation-host-r3 --gpu --env CUDA_VISIBLE_DEVICES= \
  --mount "$HOLDER" --volume ov-rem-serve-native-root /nativeio \
  --env TMPDIR=/nativeio --env NINFER_CHECKPOINT_TEST_ROOT=/nativeio \
  --seccomp "$COMMON/ninfer_io_uring_seccomp.json" -- \
  ctest --test-dir "$BUILD" --output-on-failure --output-junit "$HOLDER/ctest-r3.xml" \
  -R '^ninfer_(openai_responses_store|session_checkpoint_service|automatic_checkpoint_queue|checkpoint_policy|session_checkpoint_store|engine_checkpoint_control|engine_checkpoint_adapter|resource_manager)_test$' --timeout 180
```

## Main-approved K1/K4 clarification

An authenticated responses-only generation may declare manifest `mode: "responses_only"` and snapshot `replay_only: true`; absent mode/flag retains the existing native format and false default. This mode contains exactly the response payload and checksum metadata, zero native frontier/restored/payload statistics, and no native index, payload, or Engine Reader. Origin authentication is mandatory even when generic native import policy is non-strict; load cross-checks the authenticated manifest mode against the snapshot flag. Such a generation preserves undeleted addressable Responses after deleting their native endpoint or when that endpoint is unavailable, but is never `covers()`/pin evidence for native reclamation. Its restore applies the normal revision/publication/freshness/capacity/namespace guards and atomically publishes only the Responses lineage without native import; a subsequent parent request replays its retained transcript. A new completed stored turn establishes a normal native endpoint again. Main requested this paragraph for Contracts to incorporate after the cut; `docs/durable/CONTRACTS.md` was intentionally not edited by Lane A.

---

# Integration checkpoint state (inherited)

- Branch `rebase/upstream-64d10fbf`; owner Integrator4. Green tag `rebase/ckpt-1` points to tested `732bc1452e6515b644a91e3d149e37d7c2a8640a`, not subsequent source edits. Exact content-safe proof: [integration-ckpt-1.json](integration-ckpt-1.json); registry/history: [INTEGRATION.md](INTEGRATION.md).
- Persistence `d4e24475` integrated with newer frozen contract/backend/header/CMake corrections retained and duplicate SHA target removed. Lease `20261025T044408Z-int-persistence-732bc145-520685`: 483 build steps/201 s, host 60/60 in 9.77 s, dense plus KV 11/11 in 252.71 s, explicit Vision/DFlash2/optimized loader passed; exit 0/489 s, zero leftovers. Grammar passes unchanged in 21.9744 s.
- Canonical published `81f924d4` shared MTP/fp8 passed on secondary GPU job `10880`: 12 clients, 1,862 reused tokens, 201 decode rounds/387 rows, six partial-tail clients, available=1. Rebuild 237 steps/653 s; terminal exit 0. Earlier `5476dfd6` result is supplementary only. Superseded queued lease `522084` exited 125/0 s without a test or leftovers.
- Harness and `/checkpoints-v013` lifecycle are green at exact `ec927e574`: 321-step/2323 s secondary build, host60/60 including sorted whole-tool byte pins, lifecycle15/15, canonical MTP/fp8 and loader pass. Dense aggregate11/11 after prefix-only closeout at600 s passed495.35 s; initial239.294 s timeout remains in [integration-harness.json](integration-harness.json). No assertion was relaxed.
- A3 `8e4d03d59` + B2 `2fa762cc` are green at `686732de3`/`rebase/ckpt-durable`: lease `587569`,274-step/139 s build, host65/65, dense11/11, loader/canonical MTP/fp8, E2 overlap37.0491 s, four production boots with20,926 cached tokens and27 byte-identical continuation bytes; exit0/613 s, zero leftovers. [Receipt](integration-durable.json). Root/global-no-reuse refusal unchanged. Runtime-base `35720754d` precedes RC; Main permits stage-only build+package inventory/static equivalence, default build deferred to cut workflows.
- Final routes are integrated and green at `528b3da24673a7dca0a8af30d304bd1a487ef244`: lease `585010`, 196-step/156 s build, host 60/60, dense 11/11, loader/canonical MTP pass; eight operators plus causal BF16 attention pass, exit 0/811 s with no leftovers. [Receipt](integration-routes.json). Main separately accepts G2 at 342.150 versus 338.448 tok/s (101.09%, founder threshold 96%); provider depth/tail caveats remain.
- Initial RC `12ff96e0` passes host65/dense11/loader/MTP/E2/four-boots, but is not frozen. Main's #385 gate amendment passes8 unit tests/saved-metric reevaluation. Harness repaired the cost-invalid18-token private fixture without changing runtime policy or isolation assertions:16 host tests pass; changed-fixture lease594061 reuses1614/1623 tokens through private_input_replay. [Initial receipt](integration-rc.json). Full PRODUCT foreign checks and rebuilt exact amended-source RC inventory remain required before componentv0.7.0 declaration; no publication claimed.
- PortableCore `9fdb98826` (code `6661b7d03`) reports combined `47ebc3824` Windows sm89/sm86 links, 52 native host passes, Linux host52/GPU-I/O3 green. ArchEnable3 owns remaining native execution; NativeOps `9f3efe53` owns package/controller proof and DS root fix `87ae8085a`. These are not integrated native checkpoint or package acceptance.


