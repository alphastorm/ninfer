# Windows native managed-stop holds

Status: proposed design; no implementation, review verdict, or release qualification.
Issue: [#50](https://github.com/alphastorm/ninfer/issues/50).
Predecessor: [#66](https://github.com/alphastorm/ninfer/pull/66), held out of integration.
Source examined: `0628db0b0997c8bda4ac37c8e84da95986890014`.

Only this design document changes. The proposals below supersede #66's implementation
**direction**, not its historical evidence, and do not close that PR. Design review
is subsequent work. Current package behavior remains as cited in §2 until an
implementation changes the controller, installer, tests, and release notes together.

## 1. Decision, problem, and boundaries

**Retain evidence, not the live runtime slot.** Once a stop-event launch is proven
stopped, an incomplete final-save verdict becomes an immutable hold keyed by
`launch_id`. Publish the hold before removing that launch's `runtime.json`. A later
Start/Run may serve without acknowledging the hold. Only explicit Reconcile removes
holds; installer cleanup and Uninstall remain blocked until acknowledgement.

Separate three facts: process quiescence, final-save completeness, and GPU-owner
restoration. A held final save is not a still-running process and is not an exception
inside lifecycle composition. Internal Stop returns a structured result; standalone
`-Action Stop` fails on a newly held outcome unless explicitly given `-AllowHeld`.
Failures to prove quiescence or complete the required filesystem/owner operations
still fail the action. Never infer a successful stop from exception text.

[#50](https://github.com/alphastorm/ninfer/issues/50) identifies two related defects:

- **L1, fail-open verdict:** absent, unreadable, foreign-launch reports and forced
  stops can return success. Installer rollback can then delete the unfinished
  candidate's checkpoint/cache trees despite lacking a complete final-save proof.
- **L2, non-sticky refusal:** an explicit refusal retains the runtime on Stop, but
  the next Run removes the report and overwrites attribution. A later clean Stop
  can erase the reason destructive cleanup was refused.

The bounded asset is the interrupted candidate's retained evidence and isolated
cache, not a promise to recover sessions whose saves were refused. The incumbent's
availability and the GPU-owner safe-power claim are additional invariants. The
trusted local operator is not an adversary. Concurrent managed actions, task retries,
process death, malformed records, and partial filesystem/owner failures are in scope;
arbitrary privileged edits or manual launches during repair are not made safe by locks.
No server/checkpoint-format changes, cache migration, background reconciliation,
automatic acknowledgement, new packaged script, or generalized transaction service.
Work such as [#78](https://github.com/alphastorm/ninfer/issues/78) may reduce save
refusals but does not remove the need for fail-closed managed Stop.

### Why #66 was held

These are neutral identifiers for the lifecycle findings recorded in the
[#66 discussion](https://github.com/alphastorm/ninfer/pull/66), not new review results.
Round 2 confirmed the held-stop recovery class again; it was not another green pass.

| ID | Finding and consequence | Required successor direction |
|---|---|---|
| R1-A | Held Stop treated as fatal by Rollback, its recovery catch, and Restart; restoration or lease retirement could be skipped. | Structured stopped/held result; sweep every caller. |
| R1-B | Reconcile performed its mutation but status output required `state.json`, absent after first-install rollback. | Independent reconcile receipt; no status dependency. |
| R1-C | Stop interpreted runtime through the active release rather than `runtime.release_id`. | Resolve the launch's own canonical release root. |
| R1-D | Direct optional CIM `Name` access failed under StrictMode. | Property-safe census everywhere. |
| R1-E | Candidate census was gated on state activation. | Keep the pre-activation whole-root census already on main. |
| R1-F | Run replaced a previous launch's evidence without holding it. | Admission evaluates and retires the old launch first. |
| R1-G | Tolerating held outcomes could also tolerate `wrapper_cleanup_stalled`. | Unproven wrapper exit is never a held-success result. |
| R1-H | A restored pre-hold controller's Uninstall ignores holds. | Explicit mixed-version warning; no claim of retroactive protection. |
| R2-A | Retained candidate runtime named a release absent from restored state; real incumbent Start/Run refused before looking at holds. | Candidate runtime absent **before** incumbent Start, including pre-hold incumbents. |
| R2-B | Run and Reconcile rewrote one holds document under different locks and could lose an entry. | One common leaf lock for all hold/runtime mutations. |
| R2-C | Whole mutable runtime equality failed after PID/attestation updates and could strand a lease. | Stable launch key; owner retirement follows stop proof, not JSON equality. |

## 2. Verified baseline

All links below pin exact ranges at the examined commit. Function names identify
the affected boundaries; these observations are not claims that the proposals
already exist.

| Evidence | Current behavior |
|---|---|
| E1 — [`Write-JsonAtomic`, `Get-State`, `Get-Release`: 101–175][e-state] | JSON uses a temporary file and `Move-Item -Force`. Lifecycle state is schema 4; `Get-Release` refuses an id absent from `releases`. |
| E2 — [`Assert-CandidateLayout`: 238–274][e-layout] | Release root is `StateRoot\releases\<id>`; server, cache, receipts, and secret paths are bound to that identity. |
| E3 — [`Get-OwnedProcess` through `Get-RuntimeStopEvent`: 520–601][e-runtime] | Owned process requires executable path and creation ticks. Unreadable runtime currently becomes null. Controller census checks the exact server executable, not every image under its root. Terminate is the default stop capability; runtime schema 2 binds the event to a release. |
| E4 — [`Stop-ManagedProcess`: 900–1006][e-stop-control] | Receipt schema 1 has no launch id. Signal, fallback termination, wrapper wait, fresh owned-process checks, and census precede the report verdict. Initial release/plan is the active release. |
| E5 — [report verdict and retirement: 1008–1066][e-stop-verdict] | Missing/foreign report changes only `graceful` to `graceful_unreported`, without throwing. Explicit incomplete/refused report throws. Otherwise runtime is deleted; `finally` writes `last-stop.json`; Stop's caller restores the owner only if Stop returns. |
| E6 — [`Start-ManagedRelease`: 1068–1114][e-start] | Start validates, checks ownership, waits for wrapper exit, censuses, acquires the owner, starts the task, and waits ready. Its catch independently attempts Stop and owner restore and preserves the start failure as an inner exception. |
| E7 — [`Invoke-Run`: 1143–1178][e-run-entry], [1264–1351][e-run-publish] | Run holds `run.lock`, generates a launch id, deletes the previous report at 1273, acquires the lease, publishes null-PID intent at 1335, PID/ticks at 1339, and attestation at 1350. There is no old-launch retirement or common runtime-write lock. |
| E8 — [wrapper exit: 1352–1401][e-run-exit] | A launch-bound report suppresses task retry after observable nonzero exit. Finally kills its own child, retains runtime, releases `run.lock`, then conditionally restores ownership. The action probe occurs **after** releasing `run.lock`. |
| E9 — [wrapper/action coordination: 833–873][e-wait], [action lock and dispatcher: 1472–1555][e-dispatch] | `action.lock` serializes mutating actions with a 300-second acquisition deadline. Run deliberately does not acquire it. Start waits ready under it; Run must progress concurrently. Restart retains the lease across Stop/Start; Rollback stops, swaps pointers, starts, and has a recovery Stop/Start. |
| E10 — [outer lease: 387–489][e-outer], [inner lease: 222–270][e-inner], [inner restore: 371–387][e-inner-restore] | Outer schema 1 binds release/controller and prior pause. Restore checks the active release's liveness/census and exact owner identity. Inner schema 1 binds device identity and prior power; its restore consumes that record. Neither lease is launch-id keyed. |
| E11 — [pending transactions and cleanup inventory: 239–283][e-pending] | Only `in_progress` and `repair_required` are pending. Cleanup targets are transaction-owned release, secret, cache, and staging trees. |
| E12 — [handoff and candidate quiescence: 948–990][e-quiescence] | Incumbent uses its own controller's plain Stop. Candidate Stop is activation-gated, but whole-root census is not. No explicit candidate-lease or runtime-retirement assertion follows it. |
| E13 — [rollback/repair: 992–1090][e-rollback] | Quiescence precedes snapshot restoration; cleanup precedes incumbent Start. Any incomplete Stop blocks restoration. Status becomes rolled_back/repaired or repair_required. |
| E14 — [installer lock/repair: 1093–1153][e-install-lock], [activation: 1537–1584][e-activation], [task: 628–636][e-task] | Installer owns `install.lock`. Candidate controller is copied before activation. Task uses `powershell.exe -Action Run`, explicit start, and three failure restarts; controller replacement alone is not a launch fence. |
| E15 — [`Uninstall-ManagedRelease`: 1404–1470][e-uninstall], [release notes: 3–24,69–81][e-notes-power] [and][e-notes-stop] | Uninstall takes install then action lock; Stop precedes inner prior-power restore, task removal, and root deletion. Notes describe containment of explicit refusal, not an implemented Reconcile command. |
| E16 — [fixtures: 298–388][e-fixture], [installed layout: 423–485][e-installed-fixture], [refusal regression: 3042–3087][e-old-test] | Interrupted-upgrade fixture installs logging controllers and an incomplete lifecycle record. The refusal test substitutes real Stop definitions but not the real public dispatcher or real incumbent Start/Run. It asserts repair remains blocked. |
| E17 — [regression contracts: 1954–2007][e-static-stop], [2021–2093][e-static-owner], [pre-activation census: 3311–3401][e-census-tests] | Existing tests pin fresh reads, wrapper/owner coordination, report binding, inventory, and census behavior. Preserve those behaviors while replacing function-body string assumptions that change. |

Qualification's seven literal `-Action Stop` call sites were also read, not inferred:

| Caller | Pinned source | Successor decision |
|---|---|---|
| Preflight incumbent and scratch | [1151–1165][e-q-preflight] | Keep strict Stop; held final save aborts qualification admission. |
| Transfer/install scratch handoff | [1563–1575][e-q-install] | Keep strict Stop. |
| Reduced-Host-pressure setup | [1656–1671][e-q-pressure] | Keep strict Stop before separate scratch workload. |
| Bidirectional rollback `Stop-And-Report` | [1795–1828][e-q-rollback] | Keep strict Stop and the graceful-candidate assertion. |
| Security setup | [1833–1845][e-q-security] | Keep strict Stop. |
| Final restoration | [1993–2038][e-q-restore] | Must distinguish a held save from failed quiescence; see §10. |

The first row contains two call sites; the other rows contain one each. Restart validation
also reads `last_stop.mode/outcome/forced` at [2212–2216][e-q-restart]. It must remain
strict even though the controller's operational Restart will tolerate a held save.

## 3. Identity and durable records

### Launch identity and evidence

- Modern launch key is the generated 32-lowercase-hex `launch_id`, never reused.
  `release_id` and its canonical root are validated bindings, not mutable identity.
  Reject duplicate keys with inconsistent bindings. PID, creation ticks, readiness,
  and `gpu_uuid_verified` may change without changing the launch key.
- Resolve `runtime.release_id`, not `active_release`. Installed entries must have
  the canonical root. A removed candidate can still be resolved to
  `StateRoot\releases\<id>` without inventing an installed release. Validate ids
  against `^[A-Za-z0-9][A-Za-z0-9._-]*$` before path construction.
- A foreign **live** runtime is a refusal, not authority to signal/kill using the
  active release. A foreign exited runtime can be evaluated at its own root and
  retired there. All fresh Stop reads repeat this binding check.
- Every hold insertion, sticky-receipt lookup, and compare-and-retire uses the
  launch key. Never compare serialized runtime documents. Re-read runtime under
  the leaf lock; same-key PID/attestation updates are accepted, a different key
  invalidates the operation. Snapshot the latest record when first creating a hold.
- Report path must equal the launch root's `logs\shutdown.json`; its launch id must
  match. A report outside that path, even with the right id, is not evidence.
- First incomplete/unproven/forced receipt for a key wins. Keep it in `last-stop.json`
  while that launch remains unretired; retries must not overwrite it with a clean
  receipt before copying it into a hold. After retirement, later launches may
  advance last-stop normally; the immutable hold remains authoritative.
- Persist a forced-stop intention before termination when possible. A receipt-write
  failure must not suppress best-effort termination, but must refuse retirement and
  automatic owner handback. Do not turn an observed forced stop into clean evidence
  merely because a complete report appears later.
- Legacy terminate-only records without a launch id never acquire a fabricated hold
  key. Retire them only with wrapper exclusion, release/process attribution, and
  fresh idle census; PID/ticks are process guards, not modern hold identity. A
  stop-event record without a valid launch id is corrupt and is preserved/refused.

Missing runtime is distinct from unreadable/invalid runtime. Add a strict mutation
reader; do not inherit E3's catch-and-return-null behavior for admission, retirement,
reconciliation, or installer cleanup decisions. Observational Status may retain its
existing schema and best-effort presentation, but cannot authorize cleanup.

### Minimal hold schema

One file, `StateRoot\shutdown-holds.json`; no per-launch registry or index:

```json
{
  "artifact_type": "ninfer_windows_shutdown_holds",
  "schema_version": 1,
  "holds": [
    {
      "launch_id": "<32 lowercase hex characters>",
      "release_id": "<release instance id>",
      "release_root": "<StateRoot>\\releases\\<release instance id>",
      "runtime": { "<fields>": "verbatim runtime record at first retirement" },
      "stop_receipt": { "<fields>": "first non-clean launch-bound receipt" }
    }
  ]
}
```

The nested objects above describe copies, not literal placeholder fields in a
stored record. Validate envelope, array, unique ids, canonical roots, and agreement
of entry/runtime/receipt launch and release ids; mode must be stop-event. Inspect
optional fields through `PSObject.Properties`, including CIM `Name`. A missing file
means no holds; removing the last entry deletes the file. Invalid data fails closed.
Holds contain attribution and verdict, not secret values or checkpoint payloads.
They are immutable until acknowledgement, with no automatic expiry or size cap.

| Existing artifact | Change; schema policy |
|---|---|
| Lifecycle state/status 4; runtime 2 | No schema or field changes. Runtime mutations gain serialization. |
| Managed-stop receipt 1 | Add optional `launch_id` (null only if no attributable launch). Keep existing fields and schema version. Sticky retry semantics are documented. |
| Install journal 1 | Add status `held_cleanup`; use existing `phase` for lifecycle-restored and cleanup-held milestones. No new fields/version. |
| Outer lease 1; inner owner lease 1; serve shutdown report 1 | Unchanged. No launch id added to a lease acquired before Run generates one. |
| Install, failure, repair, and uninstall receipts | Unchanged schemas; held cleanup is a failed/incomplete operation, never a passed repair receipt. |

Reconcile emits and persists a new schema-1
`ninfer_windows_shutdown_reconcile_receipt` in the existing receipts directory:
unique receipt id, release id, UTC time, lifecycle-state-present boolean, census
result (`idle` or `active-new-launch`), the cleared hold entries verbatim, and
remaining hold count. This is acknowledgement evidence, not a second mutable hold
registry. Persist it before removing entries; on failure leave holds in place.

## 4. One mutation domain and lock order

`holds.lock` is the **launch-attribution leaf lock**, shared by controller and
installer. Every write/delete of holds **and runtime**, plus every read authorizing
those mutations, uses it. This includes intent, PID, attestation, failed-spawn
retirement, Stop, admission, crash-window repair, and Reconcile. Also serialize
sticky last-stop writes/reads and reconcile receipt publication in this domain.
Installer destructive-decision snapshots use the same lock, never an unlocked
read-modify-write. Status may read without it and gains no mutation authority.

Use the existing exclusive file-handle pattern (`OpenOrCreate`, `ReadWrite`,
`FileShare.None`), a bounded acquisition deadline (proposed 60 seconds/100 ms
poll), `finally` disposal, and a non-reentrant lock assertion in all mutation
helpers. The OS releases a dead process's handle; do not delete a live lock file
or add stale-lock takeover. Share identical small lock/read/compare-retire helper
bodies between the two scripts and test them; no new packaged module.

**Total nested acquisition order:**

```text
install.lock -> action.lock -> run.lock -> holds.lock
```

Skipping locks is permitted; reverse acquisition is not. A run probe is not a
retained exclusion interval. Final Stop/Start-admission/installer retirement uses
a retained run handle after wrapper drain when it needs to exclude a new wrapper.

| Actor | Lock scope |
|---|---|
| Run | `run` for wrapper lifetime; short `holds` sections; never `action` or `install`. |
| Start | `action`; drain prior wrapper, take `run` for old-launch admission/retirement; release `run` before task start and Wait-Ready. |
| Stop / Restart / Rollback | `action`; signal/wait without `run`; after drain take `run`, repeat census/binding checks, then `holds` to commit retirement. Release leaf before owner calls; release run fence before any Start. |
| Installer initial rollback/repair | `install`; child Stop owns/releases its own `action`. Then `install -> action -> run` for revalidation and snapshot restoration (§6). No child controller call while these inner locks are held. |
| Cleanup-only held repair | `install -> action -> holds` snapshot. Candidate is no longer launchable from restored state; do not take a serving incumbent's lifetime run lock. |
| Reconcile | `action -> holds`; if selected ids still occupy runtime, first drain/prove that launch stopped and acquire `run`, never while holding leaf. |
| Uninstall | `install -> action`; Stop, then run fence/recheck through task removal; release all file handles before deleting their containing root. |

**Under `holds.lock`:** bounded record reads/validation, JSON publication, and
compare-delete only. No CIM census, task API, process wait/spawn, event signal,
GPU-owner operation, controller invocation, network operation, or tree deletion.
Parse reports and prove quiescence outside it, with the run fence where required;
then re-read/compare the key under it. A changed key means retry/refuse, never apply
old evidence to a new launch. Run clears its stale report only after old-launch
retirement and before publishing/spawning the new launch, while it owns `run`.

`action.lock` cannot replace the leaf: Start holds it while awaiting readiness
that requires Run to publish. `run.lock` cannot replace it: a live incumbent holds
it while Reconcile must acknowledge an older inactive candidate. Two locks chosen
by writer cannot protect one envelope; per-hold files would not coordinate runtime
retirement. The existing wrapper action probe remains after run-lock release (E8).

The leaf serializes multiple file operations; it does **not** make them one atomic
filesystem transaction. Order hold publication before runtime deletion. Interrupted
publication may leave `hold(L) + runtime(L)`; retry must complete retirement before
restoration. Never describe that slot as harmless to an old incumbent. Atomic
replacement behavior on supported Windows remains a proof obligation (§12); this
is not a new sudden-power-loss/fsync guarantee.

## 5. Launch machine, verdict, and public actions

### Launch states and transitions

`L` is a launch key. Holds for other launches are orthogonal to the current slot.

| State | Representation | Transition |
|---|---|---|
| Empty | No runtime | Run admission validates holds and retires any predecessor; publishes fresh L intent. |
| Published | Runtime L, null PID | Spawn -> Running, then same-key attestation updates. Known pre-spawn failure -> compare-retire own intent, no hold. |
| Running | Runtime L and live owned process | Stop -> Stopping; natural/wrapper/host exit -> Exited. |
| Exited | Runtime L, not yet evaluated | Stop or Start/Run admission proves wrapper/root idle, evaluates report and sticky receipt. |
| Stopping | Runtime L retained | Signal, bounded wait, fallback termination; quiescence failure -> StopUnproven. |
| StopUnproven | Runtime and lease retained; first failure receipt if writable | No replacement, restoration, or deletion; retry after actual exit. Once proven stopped, prior non-clean evidence forces Held. |
| RetiredClean | Runtime L removed; no hold L | Clean stop-event verdict or proven terminate-mode stop; unrelated holds unchanged. |
| Held | Hold L durable, runtime L removed | Service may start a new key; explicit Reconcile -> Acknowledged. |
| RetirementInterrupted | Hold L plus runtime L | Fresh quiescence/exclusion, compare-retire L; never republish or clear it as another launch. |
| Acknowledged | Hold L removed after audit receipt | No recovery of refused sessions implied; eligible installer cleanup may proceed separately. |

A null PID is not proof that spawn never happened. Only the still-owning wrapper's
positive knowledge that Process.Start did not create a child permits the no-hold
pre-spawn transition. A dead wrapper's null-PID intent must go through census and
report evaluation. Failed attestation with a spawned/killed child is not exempt.

### Verdict rules

Quiescence is a prerequisite, not an outcome inferred from a shutdown report:
owned process gone, wrapper finished/excluded, and whole launch-root census idle.
Broaden the controller census to match the installer's whole-root policy, retaining
same-name unreadable-image refusal and optional-name handling (E3/E12).

For a **stop-event** launch, clean requires all of:

1. Expected report path, parsed schema-1 `ninfer_serve_shutdown_report`, matching L.
2. Boolean `complete=true`; integer (not string/boolean/fraction) `saved`, `skipped`,
   `refused` in `[0, int.MaxValue]`, with `refused=0`.
3. Outcome `graceful` or `already_stopped`, no observed force/nonzero/signal failure.
4. No earlier sticky non-clean receipt or existing hold for L.

| Evidence | Verdict after quiescence |
|---|---|
| Missing/unreadable/malformed report, wrong field types/id/path | Held; normal/already-exited outcome becomes `graceful_unreported`. |
| Bound report with `complete=false` or refusals | Held, `graceful_incomplete_shutdown`; preserve available session counts. |
| `graceful_wait_expired`, `graceful_nonzero_exit`, `signal_failed`, `terminated`, or forced | Held even if a complete report exists; keep causal outcome. |
| Sticky prior failure or already held L | Held; first receipt wins regardless of later report. |
| `still_running`, `wrapper_cleanup_stalled`, census failure | Unproven, not held-success; no runtime retirement or owner handback. |
| Terminate-mode launch, proven stopped | Ordinary clean retirement for lifecycle purposes; no shutdown-report requirement and no hold. An unproven terminate stop still fails. |

Internal `StopResult` contains `stopped=true`, retired launch bindings,
`held_launch_ids`, and the current receipt. It is the only pipeline output;
incidental helper output is suppressed. `Stop-ManagedRelease` restores the owner
when requested, then returns the result. A held result itself never throws.
Operational errors may occur **after** a hold or retirement was committed; callers
must not assume every exception means the old runtime still exists.

GPU-owner restoration is authorized by this stop result (or the wrapper's proof
of its own child's exit), plus fresh runtime-own-release and active/lease-root
census and existing hash/prior-pause checks. It never depends on matching a hold's
runtime snapshot. A restore failure leaves the lease for retry. Restart deliberately
keeps the lease for the next Start; if that Start fails, its cleanup must first
prove stop, then restore. A stalled wrapper is not overridden by an idle PID check.
No change to `Control-GpuOwner.ps1` or its power policy is proposed.

### Per-action behavior

| Action | Contract with holds |
|---|---|
| Status | Existing schema 4; last-stop passes through its optional launch id. No implicit reconcile or no-state status fabrication. |
| Start | Existing healthy active launch stays idempotent. Otherwise retire prior exited attribution before owner acquisition; warn about holds but do not block service merely because they exist. Invalid records/unproven processes still refuse. |
| Run | Independently enforce admission under run lock, including scheduled retry. Evaluate old report before deleting it; publish only into an empty slot with a fresh, unheld key. |
| Stop | Retire/prove stop and restore owner. Emit status and then fail if this invocation held a launch, unless `-AllowHeld`. Historical holds alone do not make an already-empty Stop fail again. |
| Restart | Accept held Stop, warn, retain/reuse lease, then real Start. Unproven Stop never reaches Start. Failed Start cleanup preserves original error and owner obligations. |
| Rollback | Pointer-only, non-destructive. Both current Stop and recovery Stop accept held results. Preserve original target-start failure; compound cleanup failure names both, with original as inner error. |
| Reconcile | Requires `-ReleaseId`; independent receipt (§7), no service start/stop or owner restore as acknowledgement. |
| Uninstall | Refuse any holds before Stop; check again after Stop and under final launch exclusion before prior-power restore/task removal. A new hold leaves the stopped service, task, and root for reconciliation. |

Add `Reconcile` to the dispatcher, `-ReleaseId` only for it, and `-AllowHeld` only
for Stop. Reject invalid combinations before effects. `-AllowHeld` accepts a stopped
but held outcome; it never ignores exceptions, clears evidence, or permits cleanup.

## 6. Installer machine and held rollback

| From | Condition | To / invariant |
|---|---|---|
| in_progress | Install succeeds | completed, unchanged. |
| in_progress / repair_required | Candidate unproven, owner restore fails, or snapshot restoration fails | repair_required; preserve candidate trees and remaining recovery evidence; no incumbent Start against candidate state. |
| in_progress / repair_required | Quiescent, attribution retired, snapshots restored, incumbent Start succeeds (or no incumbent), candidate held | held_cleanup; restoration finished, all transaction-owned candidate trees retained. |
| in_progress / repair_required | Same proof, no candidate hold, cleanup succeeds | rolled_back / repaired. |
| held_cleanup | Holds remain | held_cleanup; report acknowledgement/repair command; no snapshot restore, task registration, or incumbent Start. |
| held_cleanup | Holds acknowledged, fresh candidate census/lease/runtime checks pass, cleanup succeeds | repaired. |
| held_cleanup | Cleanup/quiescence fails | held_cleanup with diagnostics; restoration-complete invariant remains true. |

Pending enumeration includes `held_cleanup`; new install is blocked until repair
finishes. A held transaction never emits a passed install/repair receipt.

### Required ordering

1. Under `install.lock`, when state names the activated candidate, invoke the
   **candidate's packaged controller**, not an overwritten restored controller,
   with `-Action Stop -AllowHeld`. Use parameter metadata to pass AllowHeld only to
   controllers supporting it; legacy handoff remains plain Stop. Do not swallow
   any exception merely because a hold exists. Preserve incumbent-before-successor
   owner attestation order (E12/E14).
2. After child Stop returns, acquire `action`, then a retained `run` fence before
   changing an activated lifecycle. If a task retry acquired run first, refuse or
   retry Stop, not restore underneath it. Re-census the candidate root, including
   pre-activation candidates, and require its outer lease absent. Never delete a
   lease from the installer.
3. Under `holds`, strictly read runtime and holds. Require **no candidate runtime**
   before restoring snapshots. If a crash left runtime L and hold L, finish its
   compare-and-retire only after step 2's proof. Unheld candidate runtime must be
   evaluated by the candidate controller while attributable, not deleted by the
   installer. Malformed runtime is a refusal, not absence. Release the leaf.
4. Restore state, controller, owner controller/helpers, and task from the journal.
   Retain action/run fencing throughout this restoration. Publish the existing
   journal's phase as `${Mode}_lifecycle_restored` before releasing the fence;
   this milestone means all snapshot restoration succeeded, not that Start did.
   On first install, restored state/controller/task may all be absent.
5. Release action/run locks **before** calling the restored incumbent's public
   Start. It must reach real Run admission with no candidate runtime in the slot.
   A pre-hold incumbent needs no knowledge of holds. Never relabel candidate
   runtime as incumbent runtime. If Start fails, retain repair_required and the
   lifecycle-restored milestone; retry skips snapshot restoration and uses
   idempotent Start. A crash after successful Start but before the final journal
   update may repeat the Start call, not blindly restart a healthy service.
6. Re-snapshot candidate holds under the leaf before the cleanup decision. While
   held, skip **all** cleanup targets: release, cache/checkpoints, secrets, staging.
   After successful restoration/Start, record held_cleanup and fail with the
   packaged Reconcile command followed by `-RepairInterruptedInstall`. With no
   hold, use the existing validated cleanup inventory and terminal statuses.

The initial restoration fence is conditional on changing an activated lifecycle.
A pre-activation copied candidate is not launchable through installed state;
census and inspect it under install/action without waiting on an untouched serving
incumbent's run lock. Similarly, completed held_cleanup repair leaves incumbent
Run alone. Candidate absence from the restored `releases`, retired runtime, and
completed restoration are the guards making that cleanup-only path safe. A record
contradicting those guards refuses cleanup rather than re-entering restoration.

A snapshot is not, by itself, exclusion from a future launch. The retained run
fence in steps 2–4 closes the managed task-retry window that a snapshot-only
design leaves open. A late task cannot read candidate state and then spawn after restoration:
Run acquires run.lock **before** reading state (E7). External controller calls,
including Start, are never made while the installer owns their action/run locks.

## 7. Explicit Reconcile

`-Action Reconcile -ReleaseId R` acknowledges inspected evidence; it does not
recover unsaved sessions, stop a new launch, restore the owner, delete a tree, or
mark an installer repaired. Use the retained candidate's packaged controller when
the root controller was restored to an older version or removed on first rollback.

1. Under action lock, read/validate holds and capture the exact set of ids for R.
   No entries is an explicit error. Validate state if present; absence is supported.
2. For inactive R (installed or not), prove its canonical root idle, retaining
   unreadable same-name image refusal. A missing directory alone is not proof:
   still perform the census. For active R serving a **different, unheld key**, old
   holds may be acknowledged without stopping it (`active-new-launch`).
3. If selected ids still occupy runtime, require wrapper exclusion and fresh
   quiescence before completing that interrupted retirement. A hold is not a
   substitute for present liveness proof. Never delete a newer runtime key.
4. Under holds.lock, re-read the envelope and runtime; persist a unique receipt
   containing the selected entries still present, finish any proven same-key
   retirement, then remove **only those ids**. Entries added after selection survive,
   including new holds for R. No whole-document overwrite from an earlier snapshot.
5. Print the reconcile receipt directly, not `Get-StatusObject`. A write failure
   leaves the hold; a crash after receipt publication but before removal leaves a
   safe retry (possibly another audit receipt), not implicit acknowledgement.

The candidate installer is used for subsequent cleanup. Reconcile can run while
an incumbent serves and does not require a lease-free *incumbent*. Installer repair
separately requires the candidate's lease retired before deleting its trees.

## 8. Failure matrix

`H(L)` = immutable hold; `rt(L)` = live slot; tests below are planned, not executed.

| Trigger | Durable state after | Visible result | Recovery/retry | Cleanup | Decisive test |
|---|---|---|---|---|---|
| Missing/unreadable/malformed/foreign/path-mismatched report | H(L), runtime retired; owner restored if successful | Stop held failure, or success with AllowHeld | Inspect/Reconcile | Withheld | T1, T9 |
| Explicit incomplete/refused final save | H(L), first receipt and counts | Same | Start may serve; later clean Stop cannot clear H | Withheld | T1, T2 |
| Forced/nonzero/timeout/signal failure; process now gone | H(L), causal receipt | Same | Reconcile | Withheld | T1 |
| Terminate predecessor or clean bound stop-event | Runtime retired, unrelated H unchanged | Success | Normal lifecycle | Only if candidate has no H | T1, T2 |
| Still-running child, stalled wrapper, helper in root, uncertain CIM | Runtime/lease and first failure receipt retained | Hard failure even with AllowHeld | Prove exit, retry Stop; sticky failure then holds | None | T3, T8 |
| Leaf timeout / hold write fails | Runtime/lease retained; first failure receipt if writable | Hard failure | Retry after I/O/lock recovery; never infer absent from parse error | None | T8 |
| Hold published, runtime delete fails or process dies | H(L) + rt(L); lease may remain | Failure or interrupted action | Fresh proof and compare-retire before restore | None | T8, T9 |
| Owner restoration fails after retirement | H if applicable; runtime absent; lease retained | Hard failure, not a tolerated hold | Stop retry restores recorded owner; no hold duplication | None until retired lease | T3, T9 |
| First failed Stop followed by complete report | First receipt survives; eventual H(L) | Held, not newly clean | Reconcile | Withheld | T8 |
| Run sees prior exited/foreign launch | Old H or clean retirement precedes fresh intent | Replacement allowed only after retirement | Reconcile old H separately | Old candidate protected | T4 |
| Spawn known not to occur / wrapper dies around spawn | Own intent removed / uncertain intent retained | Run failure | Uncertain case censused and evaluated | No premature cleanup | T4, T8 |
| Run update vs Stop snapshot | One L; newest runtime copied at first hold | Normal held result and owner retirement | No equality workaround | Withheld | T5 |
| Run appends while Reconcile removes another hold | Both updates serialized; new H retained | Both actions complete | No recovery needed | Only acknowledged ids eligible | T6 |
| Held candidate rollback, including old incumbent | rt(C) absent before real Start; H(C); held_cleanup | Incumbent available, installer reports held cleanup | Reconcile then cleanup-only repair | All candidate trees retained | T9 |
| Retry of held_cleanup / post-Reconcile repair | Unchanged / repaired | Held diagnostic / passed repair | No repeated restore or Start | Deferred / candidate only | T9 |
| First-install held rollback | No state/root controller; packaged controller and H retained | Held diagnostic | Packaged Reconcile without state, then repair | Deferred | T7, T9 |
| Task Run arrives between Stop and restore | Fence excludes it, or rollback refuses before restoration | Safe restoration or repair_required | Retry with candidate quiescent | None on refusal | T10 |
| Reconcile receipt write fails / concurrent new same-release hold | Existing H remains / new H not selected remains | Error / receipt for captured ids only | Retry explicitly | No implicit deletion | T6, T7 |
| Uninstall sees old H or creates H during Stop | Root/task retained; new-H case already stopped | Refusal before inner prior-power restore | Reconcile each release, retry | None | T11 |
| Corrupt envelope/runtime or conflicting launch bindings | Original bytes retained | Mutation/admission fails closed | Operator restores valid evidence; no automated repair by deletion | None | T12 |
| Old controller/installer used after downgrade | Holds/journal may be ignored | No safety promise from old tool | Use retained updated packaged tools | Old Uninstall can destroy evidence | T13 |

## 9. Red-to-green regression plan

No tests run for this design-only change. Implementation must supply observable red
and green evidence through the public `-Action` parameter binder and dispatcher;
helper-only calls and static strings do not prove lifecycle composition.

Reuse `installed_release` and committed predecessor bytes (E16), not the incomplete
logging-only interrupted-upgrade lifecycle. Borrow #66's dispatcher injection
technique: insert test-only OS-boundary overrides immediately before the real
switch, leaving parameter binding, Start/Run/Stop/Reconcile, identity, locks, JSON,
verdict, and lease decisions intact. Define stubs in each child script's scope;
do not depend on installer dynamic-scope leakage. Stub GPU/task/CIM/network/process
boundaries, not `Start-ManagedRelease` or the retirement decision being tested.

| Test | Required observations |
|---|---|
| T1 — Stop outcome table | All report/forced cases in §5, including complete report plus force, already-exited missing report, invalid numeric types, and terminate mode. Assert ordinary Stop exit failure versus AllowHeld, exact first receipt/key, runtime retirement, lease behavior, and retained trees. |
| T2 — Sticky refusal across service replacement | Public Stop(refusal) -> Start -> separate Run -> clean Stop. Old hold byte-equivalent throughout; new key retired cleanly; repeat empty Stop does not clear the hold. |
| T3 — Composed callers | Real Restart, Rollback target/recovery starts, Start-failure cleanup, and owner-restore failure. Held allows non-destructive progress; stalled wrapper/live child never does. Original start failure remains identifiable; no lease stranded or prematurely handed back. |
| T4 — Admission | Start and Run independently cover same/foreign installed/removed-release runtime, missing/refused/complete reports, live census, null-PID intent, spawn failure, attestation kill, and hold-plus-runtime crash window. Old report is consumed before removal. |
| T5 — Stable-key mutation | Same L changes PID/ticks/attestation between snapshot and final read. Hold captures latest runtime, original receipt persists, owner retires, Rollback reaches real Start. A different L must refuse stale retirement. |
| T6 — Two-process Run/Reconcile race | Deterministic barrier schedule below; assert exact surviving keys and reconcile receipt, not just both exit codes. Include a new same-release hold after selection. |
| T7 — Reconcile | Public packaged path without state, installed inactive, removed candidate, active-new-launch, busy/unreadable root, optional/missing Name, selected-key runtime, no holds, receipt I/O failure. New runtime and incumbent lease remain unchanged. |
| T8 — Stop/crash persistence | Kill/fault at forced-intent receipt, hold publication, runtime removal, and lease restore; inject lock timeout. Retry with a later complete report must retain first non-clean evidence. Corrupt runtime never becomes empty. |
| T9 — Real held installer rollback | Full sequence below, all held outcomes, first install, restoration/Start failures, and repeated repair. Replaces `test_incomplete_shutdown_blocks_rollback_and_repeated_repair`, not its evidence-retention assertion. |
| T10 — Restoration exclusion | Separate Run tries before/while installer owns run fence; candidate cannot spawn across restored state. Crash after lifecycle-restored phase and after incumbent Start proves restart-safe repair. Keep E17's pre-activation census scenarios. |
| T11 — Uninstall | Old hold refuses before Stop; newly held Stop refuses before inner power restore and task removal; post-Reconcile retry succeeds. |
| T12 — Structural invariants | Strict envelope/array/root/key checks; optional Name under StrictMode; all runtime/hold writes centralized with lock assertions; identical shared helper bodies; no serialized-record identity comparisons. Retain script parse/inventory checks. |
| T13 — Mixed tools and qualification | Real pre-hold incumbent; metadata AllowHeld detection; old-tool warnings. Qualification gates remain strict; final restore attempts required incumbent Start after a proven held stop without marking final-save qualification passed. |

**T9 must run real controllers on both sides.** Candidate root and packaged
controller use successor source. Incumbent `snapshots/controller.bin` uses either
successor source or exact baseline source from `0628db0b0997c8bda4ac37c8e84da95986890014`.
Construct both full installed records and snapshot them before candidate activation.
The task stub launches a **second PowerShell process** with the restored controller's
`-Action Run`; it must cross actual runtime admission/publication to a controlled
process boundary. Merely logging `incumbent:Start`, calling a stub Start, or manually
replacing runtime after rollback cannot pass this test. Assert in order:

1. Candidate stopped/held, runtime absent before incumbent Start, lease retired.
2. Incumbent Start and Run succeed; incumbent attribution is new, not candidate.
3. held_cleanup; every candidate target and immutable hold retained.
4. Repeated held repair makes no restore/task-register/Start calls.
5. Packaged candidate Reconcile leaves incumbent runtime/lease untouched.
6. Repair deletes candidate-only trees and records repaired.

This directly closes the round-2 blind spot: the prior fixture's incumbent was a
logging stub (E16 and #66), so its green result never exercised runtime admission.

**T6 schedule:** seed holds A and a sentinel hold, plus exited unreported runtime B.
Process 1 dispatches Reconcile(A), snapshots selection, then pauses at its actual
hold-envelope write. Process 2 dispatches Run and attempts old-B hold publication.
The parent observes both barriers, verifies B has not committed while process 1
owns holds.lock, then releases process 1. Both finish; final envelope contains the
sentinel and H(B), not A, and the receipt contains only A. Repeat with Run paused
inside the leaf first and Reconcile queued second. Do not make process 1 wait for
B to finish: correct locking would deadlock that test. Use external release gates,
bounded waits, independent processes, and real file locks. The unlocked #66 version
must deterministically lose an update or violate the barrier assertion.

| Historical finding | Tests closing the class |
|---|---|
| R1-A callers/lease | T3, T9, T13 |
| R1-B no-state output | T7, T9 |
| R1-C release attribution | T4, T5, T7 |
| R1-D StrictMode Name | T7, T10, T12 |
| R1-E pre-activation census | Existing E17 tests plus T10 |
| R1-F overwritten evidence | T2, T4 |
| R1-G stalled wrapper tolerance | T3, T8 |
| R1-H old Uninstall | T13 and explicit release-note limitation |
| R2-A retained runtime blocks incumbent | T9, T10, both controller generations |
| R2-B lost hold update | T6, T12 |
| R2-C mutable-record equality | T5, T12 |
| #50 L1/L2 | T1 + T9 / T2 |

Run new regressions red against the baseline; demonstrate T5/T6/T9 detect the #66
classes against its held implementation, accounting for absent successor CLI options
rather than accepting a parameter-binding error as the substantive red. Green must
exercise the complete successor, then the Windows release-driver environment and
supported Windows PowerShell 5.1 task path as well as portable pwsh fixtures. These
are implementation/release obligations, not verification claimed by this document.

## 10. Operator behavior, qualification, and mixed versions

- Standalone Stop can now fail **after the server stopped and owner restoration
  succeeded**. Its diagnostic names launch/release and reconciliation; `-AllowHeld`
  is deliberate operational tolerance, not final-save success. Restart and pointer
  Rollback may restore service with a warning while evidence remains held.
- Reconcile is explicit acknowledgement of possible lost final saves. Installed
  active-new-launch service need not stop. Rolled-back/first-install recovery uses
  `releases\<id>\bin\lifecycle\Control-Release.ps1 -Action Reconcile -StateRoot
  <root> -ReleaseId <id>`, then the candidate/newer installer with
  `-RepairInterruptedInstall`. Never recommend deleting hold/runtime files.
- Reboots/crashes that leave unreported stop-event attribution can accumulate
  holds. No background expiry; Uninstall requires acknowledgement of every release.
  Whole-root census now also blocks on helper/benchmark images in the release root.
- Qualification admission and evidence phases in §2 keep strict Stop; do not add
  AllowHeld globally. Restart's existing graceful/unforced assertions stay intact.
  **Final `qualify_native.restore()` is a recovery caller**, however: currently its
  strict scratch Stop precedes incumbent Start ([1993–2038][e-q-restore]), so leaving it unchanged
  would reproduce R1-A. It must tolerate only a proven held Stop (capability-detected
  AllowHeld), complete owner/census checks and incumbent restoration, then retain a
  failing qualification outcome/held diagnostic. It must not auto-Reconcile or
  publish a passing final-save result. Its tests must cover the real dispatch
  outcome, not catch exception text. Any actual Stop/owner failure still blocks
  unsafe restart. This small caller change belongs in the implementation.
- A pre-hold incumbent restored by rollback can Start because candidate attribution
  is already gone. It cannot enforce hold-aware Uninstall or preserve new holds
  for its own launches. Explicit use of its old Uninstall may delete retained
  evidence with the root; use updated packaged tools for acknowledgement/cleanup.
- An older installer does not enumerate held_cleanup; it may ignore pending work.
  Downgrading does not remove holds or convert the journal. Do not use old repair
  tools on a held transaction. No silent compatibility shim or schema migration.

**Migration: none.** Main has no released hold envelope to migrate. Existing
stop-event runtime/report evidence is evaluated on first successor action; legacy
terminate behavior remains scoped as above. Package bytes will change only in the
implementation/release, requiring the normal rebuild and affected release
qualifications, not a retroactive patch to the earlier published package.

## 11. Design choices and rejected variants

These choices replace simpler variants that would reintroduce the held classes:

1. **Callers are enumerated, not assumed.** §2 lists every qualification Stop
   caller. The baseline writes runtime at 1335/1339/1350, and action-lock
   acquisition waits 300 seconds, longer than the leaf lock's 60 seconds.
2. **No unsafe “held runtime is harmless” exception.** Installer requires candidate
   runtime absent, including hold-plus-runtime crash windows, before *either*
   controller generation starts. Corrupt runtime never counts as absent.
3. **Actual exclusion, not a stale snapshot.** Retain run fencing across activated
   restoration and journal its completion before Start; do not leave task retry
   during restoration as an unanswered correctness assumption.
4. **Sticky receipt and crash ordering.** First non-clean evidence must survive
   retries even when hold publication fails. Same-key retirement, not a JSON
   snapshot comparison, is decisive; a leaf lock is not multi-file atomicity.
5. **Exceptions are not a liveness enum.** Receipt/retirement/owner I/O can fail after
   stopping; hard errors still refuse destructive progress but do not imply all
   earlier writes rolled back. Start cleanup cannot hand back a lease after an
   unproven wrapper stop.
6. **Reconcile is conservative for inactive roots.** Census installed-inactive as
   well as removed releases; only an active different launch gets the non-disruptive
   acknowledgement path. Never treat a missing root as a substitute for census.
7. **Real test scheduling.** Two processes and parent-controlled barriers, not a
   writer waiting for an operation blocked by its own lock; real Start/Run for the
   restored incumbent, not a sentinel substituted for lifecycle code.
8. **Qualification recovery is in scope.** Rather than leaving the qualification
   caller unchanged, final restoration must finish safe incumbent recovery on held
   scratch Stop while keeping qualification failed. All evidence/admission Stops remain strict.

## 12. Implementation order and open questions

One cohesive successor implementation; do not merge intermediate behavior where
controller held semantics and installer/caller handling disagree.

1. Add state-faithful dispatcher fixtures and red T1/T2/T5/T6/T9 proofs first.
2. Implement strict readers, stable-key helpers, common leaf lock, and centralized
   runtime/hold/sticky-receipt writes. Keep packaged-file inventory unchanged.
3. Implement verdict/retirement, admission, structured Stop, and every controller
   caller together, including owner-proof gating and Uninstall's second hold check.
4. Add Reconcile and persisted acknowledgement, then installer held_cleanup,
   restoration fencing/milestone, and pre-hold handoff behavior.
5. Update qualification recovery caller, focused regressions, and Windows release
   notes. Supersede the explicit-refusal-only containment paragraph (E15) with the
   new behavior, commands, and old-tool limitations; no claim to recover unsaved data.
6. Review the frozen implementation after focused red/green proof; then normal
   Windows/package and hardware qualification before release. This draft has none
   of those implementation receipts and does not authorize deployment.

Open questions for design review and implementation proof (defaults above apply):

- Is the proposed 60-second leaf-lock acquisition bound appropriate for the supported
  filesystem/antivirus behavior? Validate file replacement and lock handles on both
  Windows PowerShell 5.1 and pwsh; test one/many holds array round trips. Do not relax
  refusal or serialize external work under the leaf to accommodate a slow machine.
- Does actual Task Scheduler retry/task restoration obey the stated run-fence
  ordering on supported Windows? Rehearse the interrupted restoration schedule;
  a failure requires fixing that boundary, not accepting a census-only proof.
- What operator acknowledgement wording best distinguishes preserved checkpoints
  from unrecoverable refused sessions? Default is release-scoped explicit Reconcile
  with an immutable receipt and no automatic cleanup.
- Should final qualification restoration expose a separate “restored with held save”
  detail in its existing failure record? Default: restore safely, retain the held
  diagnostic, and never qualify the run; no new qualification schema is required
  merely to tolerate the operational stop result.

## Source links

[e-state]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-Release.ps1#L101-L175
[e-layout]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-Release.ps1#L238-L274
[e-runtime]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-Release.ps1#L520-L601
[e-stop-control]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-Release.ps1#L900-L1006
[e-stop-verdict]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-Release.ps1#L1008-L1066
[e-start]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-Release.ps1#L1068-L1114
[e-run-entry]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-Release.ps1#L1143-L1178
[e-run-publish]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-Release.ps1#L1264-L1351
[e-run-exit]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-Release.ps1#L1352-L1401
[e-wait]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-Release.ps1#L833-L873
[e-dispatch]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-Release.ps1#L1472-L1555
[e-outer]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-Release.ps1#L387-L489
[e-inner]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-GpuOwner.ps1#L222-L270
[e-inner-restore]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-GpuOwner.ps1#L371-L387
[e-pending]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Install-Release.ps1#L239-L283
[e-quiescence]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Install-Release.ps1#L948-L990
[e-rollback]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Install-Release.ps1#L992-L1090
[e-install-lock]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Install-Release.ps1#L1093-L1153
[e-activation]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Install-Release.ps1#L1537-L1584
[e-task]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Install-Release.ps1#L628-L636
[e-uninstall]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/Control-Release.ps1#L1404-L1470
[e-notes-power]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/RELEASE_NOTES.md#L3-L24
[e-notes-stop]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/RELEASE_NOTES.md#L69-L81
[e-fixture]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/test_windows_release.py#L298-L388
[e-installed-fixture]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/test_windows_release.py#L423-L485
[e-old-test]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/test_windows_release.py#L3042-L3087
[e-static-stop]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/test_windows_release.py#L1954-L2007
[e-static-owner]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/test_windows_release.py#L2021-L2093
[e-census-tests]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/test_windows_release.py#L3311-L3401
[e-q-preflight]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tools/qualification/qualify_native.py#L1151-L1165
[e-q-install]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tools/qualification/qualify_native.py#L1563-L1575
[e-q-pressure]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tools/qualification/qualify_native.py#L1656-L1671
[e-q-rollback]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tools/qualification/qualify_native.py#L1795-L1828
[e-q-security]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tools/qualification/qualify_native.py#L1833-L1845
[e-q-restore]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tools/qualification/qualify_native.py#L1993-L2038
[e-q-restart]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tools/qualification/qualify_native.py#L2212-L2216
