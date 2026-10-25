# Engine/cache slice

## Scope and preserved contracts

- Branch `rebase/engine-cache`, upstream base `64d10fbf9`; upstream scheduler, preemption, native context ownership, DFlash2 and grammar remain authoritative. No fork scheduler, old resource ledger, NVFP4 KV implementation, standby-pinning retries or unshipped Q5 route was imported.
- `GpuKeepWarm` owns one nonblocking stream and one memory-free warp. Busy-stream queries skip rather than enqueue; `cudaErrorNotReady` is cleared before the next model launch check. Runtime-selected pacing is 50 ms / 100 ms for sm_89 and 3.5 ms / 10 ms otherwise. The device-bound Engine worker creates/destroys it, arms the idle window after execution/context progress, wakes immediately for pending work/stop, and disables it with a warning after a CUDA failure. Default is off; the accepted CLI spelling is `--gpu-keep-warm-ms`.
- C2 regressions retain upstream's queue bound (`max_concurrency + max_pending_requests`), the 30 s pending default, cancellation and compact row/control membership. A shortened explicit queued deadline exercises expiry without changing that default. Real-model tests cover two readers of a primed checkpoint, live short-sibling continuation while a long sibling still runs, fresh-versus-reused deterministic tokens, independent output budgets, abandonment and settled membership. Existing upstream pressure/preemption/replay tests remain intact.
- Private `reuse_scope` is immutable at adoption and distinct from the mutable newest-session hint. Shared stable/tool prefixes use `shared_reuse_scope` at bearer-principal granularity and remain reusable across that principal's different sessions. Four-way fanout, warm arrival, foreign-private denial, cross-principal shared denial, anonymous isolation and `store:false` ownership protections have executable ResourceManager regressions. No old retention enum or blanket shared-cache disable was introduced.
- Selection telemetry is captured from the actual checkpoint role before Native may consume its handle. The four precise routes are `private_endpoint`, `private_input_replay`, `private_long_anchor`, `shared_stable_prefix`; Root/Checkpoint remain available for genuinely unclassified native paths. No TurnClosure subtype is fabricated. Catalog occupancy counts actual valid records, including active owners before capture, without adding a second policy ledger.
- Native unsupported KV modes are rejected in `normalize_engine_options`, before DeviceContext/artifact allocation: sm_86/sm_89 reject NVFP4 and mixed FP8-key/NVFP4-value KV; sm_86 also rejects FP8 KV. A16 weight codecs are unaffected. Portable two-limb arithmetic replaces Engine `__int128` cost/reclaim calculations and preserves exact Q32 rounding/saturation.
- GPU UUID ownership stays with the shipped launcher/controller contract: child-only PCI bus ordering, absent `CUDA_VISIBLE_DEVICES`, current numeric `--device`, and request-log UUID attestation. Main explicitly rejected adding an engine/server `--gpu-uuid` flag. Tooling UUID selectors remain supported. Managed Windows stop-event orchestration is PortableCore/ServePort-owned, not a second Engine stop mechanism.

## Verification

Observed host-free command:

```sh
python3 -B tests/test_engine_cache_gate.py
```

Result: five predicate tests passed. They cover exact startup shape, graph-reservation growth, context refusal, GPU/budget attestation, one-startup-record enforcement and output allowlisting. No local CUDA build was attempted.

GPU/build results and measured idle TTFT are recorded only after the approved bounded lease job completes. This branch's job is slice proof, not integrated hardware qualification. Windows compilation/qualification belongs to the native owners.

## Startup gate

`tools/smoke/engine_cache_gate.py` starts and stops two independent servers, with keep-warm off and 60,000 ms. It requires Infra's measured startup baseline rather than guessing a CUDA-graph ceiling. It attests C2, 131,072 logical context, automatic KV, BF16, DFlash2 K=7, prefill 1,024, Vision, LM-head draft (`proposal_head=optimized`) and two extra Device StateImages. The host-pool argument is explicit because upstream replaces separate Host state/KV pools with one physical pool.

Each arm primes real work, takes three first-nonempty-text-or-reasoning SSE latency samples after 12 s idle, then exercises HTTP C2 sibling fanout/continuation and requires real multirow decode counters. Startup refusal, logical-context reduction, graph reservation above the measured baseline, wrong UUID, memory-budget overflow or an unavailable Engine fails the gate. Private server logs stay in caller-selected scratch; the public receipt contains only allowlisted measurements, hashes and identifiers, not paths, prompts, outputs or credentials.

Reproduction shape (paths supplied by the lease holder):

```sh
NINFER_BUILD_HOLDER=E1 "$COMMON/build.sh" "$SRC" "$BUILD"
python3 "$SRC/tools/smoke/engine_cache_gate.py" \
  --binary "$BUILD/apps/ninfer-serve" --artifact "$MODEL" \
  --baseline "$BASELINE" --source-commit "$SOURCE_COMMIT" --gpu-uuid "$GPU_UUID" \
  --host-context-mib 20867.734375 --port 18150 --samples 3 --idle-seconds 12 \
  --work-dir "$PRIVATE_LOGS" --receipt "$RECEIPT"
```

Infra derived the pooled Host-byte equivalent as 16 GiB KV + 24 × 195,897,344-byte StateImages = 21,881,405,440 bytes, or 20,867.734375 MiB. This preserves the aggregate byte budget, not independent Host-state slot reservations.

## K3/A3 design only — no persistence implementation claimed

The frozen authority is `runtime/contract/durable_engine_api.h` / `docs/durable/CONTRACTS.md` from Contracts. Program checkpoint-set export/import remains Persistence-owned. G1 tracer hooks are not a production durability adapter. The dependent A3 implementation is intentionally not in this slice.

1. **Control-thread ownership.** External `export_session` / `prepare_import` calls enqueue a control operation and wait for a safe worker boundary. All Program inspection/mutation stays under worker/execution serialization. A worker pressure handler executes inline at a safe boundary; it must never enqueue then wait on itself. Work, stop and checkpoint-publication progress wake the same bounded control/admission loop.
2. **Exact owner and epoch.** Resolve authenticated session scope, expected tag, full retained checkpoint set and the current publication epoch together. Submission/finish/import ordering, not handler completion, determines newest lineage. Capture the epoch before disk loading; import rechecks it before publication and creates fresh runtime ordering. Disk metadata never authorizes either private or principal scope.
3. **Nonblocking export.** Stage an immutable set under worker serialization: checkpoint record leases, StateImage read leases and page/Host extent readers, with DFlash2 settled to the committed frontier. Deduplicate reachable StateImages/pages and reserve only bounded staging chunks. Release execution serialization before hash, encode, payload writes and manifest work. Disk threads own copied chunks only; pins/readers survive every accepted CUDA copy through completion, cancellation and error drain. Do not demote a whole continuation or allocate a second full checkpoint just to export. This removes the full-payload lock hold behind the reported 14.4 s fanout queueing defect; that latency reduction is not yet measured here.
4. **Final disk publication.** `DurableExportResult` carries `captured_epoch`. Mandatory `publish_export(engine,digest,tag,epoch,commit_callback)` rechecks under the same per-session publication gate used by submission/finish/import, then invokes only current-pointer replacement + sync. It holds no Engine execution lock and never waits on the worker while holding that gate. Payload hashing/writing is already complete. Stale never calls the callback; false/exception cannot acknowledge a save. Store catalog locks must not surround callbacks that re-enter the store.
5. **Transactional import.** Verify fingerprint/index/consumed payload bytes before publication. Program's move-only ImportReservation owns Host-only destinations, histories, immutable state, checkpoint slots and fences. Prepare all ResourceManager/Responses replacement allocations before native commit; epoch/policy checks precede commit; the post-native publication path must be no-throw. Aborts drain writers before releasing reservations. Capacity refusal is not corruption, identity mismatch or policy disablement.
6. **Pending protection is whole-plan protection.** `PressureCheckpointHandler` Settled/Pending must cover every destructive path, not just a chosen endpoint: `commit_release`, `commit_host_victims`, `erase`, `remove_point`/anchor replacement, `recycle_input`, private-owner abandonment/retirement, source consumption/retired points at binding, paused-snapshot revocation and adjacent demotion batches with Host victims. If any affected session is Pending, abandon the complete plan and replan after progress. Redundant replica cleanup is safe only when it preserves every durable/reachable checkpoint; physical Native authority remains unchanged. Shutdown/fail-all cleanup is a separate terminal path, not ordinary eviction.
7. **No deadlocks or speculative acknowledgements.** Worker pressure callbacks try-lock checkpoint-service coordination; Publishing returns Pending instead of waiting for a streamed response. Publication completion notifies after releasing the Responses mutex. External saves may perform the bounded publication wait. Settled distinguishes saved, transiently blocked and final logged policy refusal; it is never synonymous with saved.
8. **Bounded restore/reclaim.** Pin the compatible disk generation proving recoverability across one restore. Snapshot initial private-owner count N, obtain a new single-pass reader on every attempt, and allow at most 2N+1 attempts, retrying only after proven resident reclaim or newly proven recoverability. New arrivals do not enlarge that bound. Retain six transient save retries per completed turn; no admission rescan on every decode tick without queue/capacity/publication/checkpoint progress.

## Boundaries and provenance

- Shared CLI/header/CMake changes were supplied/authorized by Integrator; scope carriers, semantic enum and catalog declaration prerequisites are carried as their commits. Log compatibility changes were supplied by ServePort. `core/uint128.h` and its tests are PortableCore-owned prerequisites.
- The keep-warm primitive is re-expressed from the shipped Apache-2.0 fork source. No community-fork code was copied.
- Status schema 2 may report null with a reason for absent legacy catalog capacities, independent Host subpool capacities, legacy eviction counts, shared active-reference counts and the old whole-materialization prediction. Required scheduler/prefill/reuse counters and selected path are real, never fabricated zeros.
- No publication, product tag, image push, production-container operation or native package replacement is part of this slice.
