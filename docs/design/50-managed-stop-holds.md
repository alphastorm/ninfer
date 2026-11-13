# Windows native managed-stop holds

Status: revised proposed design; closed design-council dispositions incorporated;
no implementation or release qualification.
Issue: [#50](https://github.com/alphastorm/ninfer/issues/50).
Predecessor: [#66](https://github.com/alphastorm/ninfer/pull/66), held out of integration.
Source examined: `0628db0b0997c8bda4ac37c8e84da95986890014`.

Only this design document changes. The proposals below supersede #66's implementation
**direction**, not its historical evidence, and do not close that PR. §13 maps the
closed design council's dispositions; implementation proof and review remain
required. Current package behavior remains as cited in §2 until an implementation
changes the controller, installer, tests, and release notes together.

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
| Final restoration | [1993–2038][e-q-restore] | Recover on proven held Stop, durably retain held ids across cleanup re-entry, and keep qualification failed independently of safe host restoration (§10). |

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
  merely because a complete report appears later in that invocation or a persisted
  first receipt survives.
- **Persistence limit:** if the first non-clean receipt cannot be persisted at all,
  a fresh invocation has no durable discriminator for that observation. It evaluates
  the retained launch on its bound report and any other surviving evidence; a later
  complete report can therefore permit clean retirement. The failing invocation
  still refuses retirement and owner handback. Sticky-verdict guarantees require
  persisted evidence; this design does not claim to recover an unwritten fact.
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
with no interrupted-publication evidence means no holds before first creation.
**Never delete the envelope once created:** acknowledgement of the last entry
publishes `holds: []`. Invalid data or a missing destination with a leftover
publication temporary fails closed (§4).
Holds contain attribution and verdict, not secret values or checkpoint payloads.
They are immutable until acknowledgement, with no automatic expiry or size cap.

| Existing artifact | Change; schema policy |
|---|---|
| Lifecycle state/status 4; runtime 2 | No schema or field changes. Runtime mutations gain serialization. |
| Managed-stop receipt 1 | Add optional `launch_id` (null only if no attributable launch). Keep existing fields and schema version. Sticky retry semantics are documented. |
| Install journal 1 | Add status `held_cleanup`; existing `phase` records a monotonic lifecycle-restored milestone until a terminal status (§6). Cleanup errors change status/diagnostics, not that milestone. No new fields/version. |
| Outer lease 1 | No schema/field additions. Successor Acquire refreshes `release_id` on reuse; unproven refresh means unknown attribution, not proof that candidate cleanup is safe (§5). |
| Inner owner lease 1; serve shutdown report 1 | Unchanged. Neither owner lease gains a launch id. |
| Install, failure, repair, and uninstall receipts | Unchanged schemas; held cleanup is a failed/incomplete operation, never a passed repair receipt. |

Reconcile emits and persists a new schema-1
`ninfer_windows_shutdown_reconcile_receipt` in **`StateRoot\receipts`**, never the
candidate release's receipts directory, so it survives candidate-tree deletion:
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
| Installer forward activation | `install`; incumbent/retirement Stop calls own/release their own `action`. Then `install -> action -> run` before any installed lifecycle replacement, including `-NoStart` and exited/retry-pending incumbents (§6). |
| Installer lifecycle rollback/repair before the restored milestone | `install`; child Stop owns/releases its own `action`. Then bounded `install -> action -> run` before every snapshot application/re-application, even if state already names the incumbent (§6). No child controller call while inner locks are held. |
| Cleanup-only repair after the restored milestone, including held cleanup | `install`, short `holds` snapshots; **no `action`** and no serving incumbent's run lock. Only an interrupted same-key retirement may take `run`, after wrapper-drain proof. |
| Untouched pre-activation repair | `install`, short `holds` snapshots; **no `action`**. No installed lifecycle replacement. Only interrupted same-key retirement after wrapper-drain proof may take `run` (§6). |
| Reconcile | Short `holds` selection/commit sections; **no `action`**. Only if selected ids still occupy runtime, prove their wrapper drained, then acquire `run` outside the leaf to finish that interrupted same-key retirement. |
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
retirement.

The existing wrapper action probe remains after run-lock release (E8), including
for a restored pre-hold incumbent. **Only actions owning owner restoration may
hold `action.lock`.** Reconcile and owner-neutral installer paths never hold it,
so a concurrently exiting wrapper does not defer handback to a caller that will
not perform it. They serialize record decisions through `holds.lock`; they do not
signal/stop a serving incumbent or acquire its lifetime run lock. No dedicated
owner lock or changed wrapper probe is needed for this design.

The leaf serializes multiple file operations; it does **not** make them one atomic
filesystem transaction. Order hold publication before runtime deletion. Interrupted
publication may leave `hold(L) + runtime(L)`; retry must complete retirement before
restoration. Never describe that slot as harmless to an old incumbent.

For an existing `shutdown-holds.json`, `runtime.json`, or `last-stop.json`, replace
using an old-or-new-file primitive such as `[IO.File]::Replace`/`ReplaceFile`, with
a completed temporary in the same directory. Never delete the destination and
then move a replacement onto it; `Move-Item -Force` alone is not this contract.
First publication uses a same-directory, non-overwriting move. Strict readers
refuse a missing destination with a leftover publication temporary rather than
interpreting it as an empty slot/envelope or no prior receipt. Deliberate runtime
retirement remains a separately guarded deletion; hold-envelope emptying is a
replacement, not deletion. Apply these publication rules to both scripts'
shared helpers. Old-or-new visibility and handle contention on supported Windows
remain required implementation proofs (§12), not a sudden-power-loss/fsync claim.

## 5. Launch machine, verdict, and public actions

### Launch states and transitions

`L` is a launch key. Holds for other launches are orthogonal to the current slot.

| State | Representation | Transition |
|---|---|---|
| Empty | No runtime | Run admission validates holds and retires any predecessor; publishes fresh L intent. |
| Published | Runtime L, null PID | Spawn -> Running, then same-key attestation updates. Known pre-spawn failure -> compare-retire own intent, no hold. |
| Running | Runtime L and live owned process | Stop -> Stopping; natural/wrapper/host exit -> Exited. |
| Exited | Runtime L, not yet evaluated | Stop, Start/Run admission, or installer retirement before activation/restored Start proves wrapper/root idle and evaluates report and sticky receipt, whichever release L names. |
| Stopping | Runtime L retained | Signal, bounded wait, fallback termination; quiescence failure -> StopUnproven. |
| StopUnproven | Runtime and lease retained; first failure receipt if writable | No replacement, restoration, or deletion; retry after actual exit. Once proven stopped, persisted prior non-clean evidence forces Held; an unwritten first receipt has the §3 persistence limit. |
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
same-name unreadable-image refusal and property-safe optional-name handling
(E3/E12). A CIM row with a blank/unavailable executable path **and missing or blank
`Name` is unattributable and refuses stop proof, owner handback, and destructive
installer cleanup**, not an unrelated process to skip. A readable image outside
the root, or a blank image with a present, nonmatching name under the existing
same-name rule, remains distinct. A missing directory never substitutes for census.

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

Successor `Acquire-GpuOwnerLease` must refresh the outer lease's `release_id` to
the acquiring release **even when reusing** a matching lease and its already
satisfied pause; preserve controller identity, prior pause, and inner lease.
Do not use an inherited `release_id` as proof of present release attribution.
A lease not demonstrably refreshed by a successor controller has **unknown**
attribution and blocks destructive candidate cleanup until a managed Stop retires
it. With no new provenance field in schema 1, matching `release_id` or owner hash
alone does not prove refresh: only a refresh witnessed by the current successor
operation can establish that exemption for a serving incumbent. After a restart,
an old-controller acquisition, or lost refresh evidence, conservatively require
managed Stop before cleanup. Never delete a lease to manufacture attribution.

### Per-action behavior

| Action | Contract with holds |
|---|---|
| Status | Existing schema 4; last-stop passes through its optional launch id. No implicit reconcile or no-state status fabrication. |
| Start | Existing healthy active launch stays idempotent. Otherwise retire prior exited attribution before owner acquisition; warn about holds but do not block service merely because they exist. Invalid records/unproven processes still refuse. |
| Run | Independently enforce admission under run lock, including scheduled retry. Evaluate old report before deleting it; publish only into an empty slot with a fresh, unheld key. |
| Stop | Retire/prove stop and restore owner. Emit status and then fail if this invocation held a launch, unless `-AllowHeld`. Historical holds alone do not make an already-empty Stop fail again. |
| Restart | Accept held Stop, warn, retain/reuse lease, then real Start; successor Acquire refreshes the reused lease's release binding. Unproven Stop never reaches Start. Failed Start cleanup preserves original error and owner obligations. |
| Rollback | Pointer-only, non-destructive. Both current Stop and recovery Stop accept held results. Preserve original target-start failure; compound cleanup failure names both, with original as inner error. |
| Reconcile | Requires `-ReleaseId`; independent receipt (§7), no `action.lock`, service start/stop, or owner restore as acknowledgement. `active-new-launch` requires owned-process and wrapper liveness, not just an active pointer or different key. |
| Uninstall | Refuse any holds before Stop; check again after Stop and under final launch exclusion before prior-power restore/task removal. A new hold leaves the stopped service, task, and root for reconciliation. |

Add `Reconcile` to the dispatcher, `-ReleaseId` only for it, and `-AllowHeld` only
for Stop. Reject invalid combinations before effects. `-AllowHeld` accepts a stopped
but held outcome; it never ignores exceptions, clears evidence, or permits cleanup.

## 6. Installer machine, handoff, and held rollback

| From | Condition | To / invariant |
|---|---|---|
| in_progress | Fenced forward activation succeeds, with any exited slot already retired | completed, including `-NoStart`; no unevaluated predecessor runtime carried across activation. |
| in_progress / repair_required, no restored milestone | Candidate/slot unproven, owner restore fails, or snapshot restoration fails | repair_required; preserve candidate trees and recovery evidence; no incumbent Start against candidate state. |
| in_progress / repair_required, no restored milestone | Snapshots partly/fully restored but milestone publication failed or process died | Remain pending without assuming restoration complete; bounded action/run fence before any re-application. A serving restored incumbent requires explicit Stop then retry. |
| in_progress / repair_required, restored milestone durable | Incumbent Start fails | repair_required with milestone retained; retry skips snapshots and attempts safe, idempotent Start, not restoration under a live wrapper. |
| in_progress / repair_required, restored milestone durable | Incumbent available (or none), candidate held | held_cleanup; all transaction-owned candidate trees retained. |
| in_progress / repair_required, restored milestone durable | No candidate hold, cleanup succeeds | rolled_back / repaired. |
| in_progress / repair_required, restored milestone durable | No candidate hold, cleanup fails, or process dies after incumbent Start | repair_required / still pending, milestone unchanged; post-milestone cleanup retry does not acquire the serving incumbent's run lock. |
| held_cleanup | Holds remain | held_cleanup; report acknowledgement/repair command; no snapshot restore, task registration, or incumbent Start. |
| held_cleanup | Holds acknowledged, fresh candidate census/lease/runtime checks pass, cleanup succeeds | repaired. |
| held_cleanup | Cleanup/quiescence fails | held_cleanup with diagnostics and milestone retained; restoration-complete invariant remains true. |

Pending enumeration includes `held_cleanup`; new install is blocked until repair
finishes. A held transaction never emits a passed install/repair receipt. Detect
the restored milestone from **either** `rollback_lifecycle_restored` or
`repair_lifecycle_restored`, regardless of the current invocation's mode or pending
status. Once durable, keep that phase unchanged through Start/cleanup failures and
held diagnostics; do not overwrite it with `${Mode}_incomplete` or a cleanup-held
phase. Only a terminal rolled_back/repaired status may replace it with a terminal
phase. Thus one existing field suffices without losing restoration history.

### Upgrade handoff and forward activation

1. Prepare candidate-only files under `install.lock` without replacing installed
   lifecycle files. Stop a running incumbent through **its own controller and
   hash-bound owner policy**, before successor owner attestation. Capability-detect
   `-AllowHeld`: a successor incumbent uses `Stop -AllowHeld`, may leave its hold,
   and may proceed with the upgrade once stop/owner proof succeeds. An incumbent's
   release/cache trees are never candidate cleanup targets. A pre-hold incumbent
   receives plain Stop; a failure remains an install failure, not a held-success
   inferred from exception text.
2. Independently of task `State` and lease absence, successor code must evaluate
   and retire **every exited runtime in the slot, whichever release it names**,
   before activation. This includes a legacy incumbent's refused Stop and an
   incumbent exited between failure-restart intervals. Use the successor packaged
   controller's verdict/retirement path while the original lifecycle and owner
   identity are still available; child calls run with only `install` held. Persist
   any resulting hold before runtime removal. Unproven exit or owner restoration
   still refuses progress.
3. Before the first installed controller/helper/state/task mutation, take bounded
   `install -> action -> run`, then revalidate census, leases, runtime and holds
   (record reads under `holds`). Require the old slot retired even with `-NoStart`.
   If a scheduled retry owns run or a new/unproven key appeared, refuse or return
   to managed Stop **outside** the inner locks; do not replace files underneath it.
   Retain the fence through installed-file replacement and activation. Release it
   before candidate Start. A task not currently Running is not a launch fence.

### Rollback and repair ordering

1. Under `install.lock`, when state names the activated candidate, invoke the
   **candidate's packaged controller**, not an overwritten restored controller,
   with `-Action Stop -AllowHeld`. Before any restored incumbent Start, also use
   successor verdict rules to retire any exited slot left by the upgrade handoff,
   including an incumbent's own refused launch. A legacy Stop failure must not
   reach legacy Start with its refused report/runtime still in place. Do not
   swallow other operational errors because a hold exists.
2. Without a durable lifecycle-restored milestone, acquire bounded `action`, then
   a retained `run` fence **before any snapshot application or re-application**.
   This applies if state already points to the incumbent, not just when
   `state_activated` is true. Re-census candidate and relevant runtime/lease roots;
   require the candidate lease retired and refuse unknown lease attribution (§5).
   If a restored incumbent is serving or a task retry holds run, fail with an
   explicit **Stop the serving launch, then retry repair** diagnostic; never
   restore under its live lock. Snapshot re-application is idempotent and fenced.
3. Under `holds`, strictly read runtime and holds. Require an empty slot before
   snapshot restoration and before a fresh incumbent Start, not merely absence of
   candidate runtime. If a crash left runtime L and hold L, finish same-key
   compare-retirement only after wrapper-drain and step 2's fresh proof. Unheld
   exited runtime requires successor verdict evaluation, not installer deletion;
   release inner locks before any necessary child controller call and revalidate
   after reacquiring. Malformed runtime refuses. Never relabel one release's
   runtime as another's.
4. Restore state, controller, owner controller/helpers, and task from the journal,
   retaining action/run fencing throughout. Publish
   `${Mode}_lifecycle_restored` **before releasing the fence**; all snapshots must
   have succeeded. The milestone does not assert incumbent Start success. On first
   install, restored state/controller/task may all be absent. If publication fails,
   the next attempt follows step 2 even if every snapshot had actually completed.
5. Release action/run locks **before** the restored incumbent's public Start.
   It must reach real Run admission with no exited predecessor attribution in the
   slot; successor code enforces this even for a pre-hold incumbent. A healthy
   already-serving incumbent may remain idempotent. On Start failure retain
   repair_required and the milestone; subsequent recovery skips all snapshots.
   A crash after Start but before status publication may repeat an idempotent
   Start, not blindly restart a healthy service. Once availability is established,
   cleanup-only retries never repeat Start/task registration.
6. Re-snapshot candidate holds, runtime, restored state and lease attribution
   under the appropriate record locks before cleanup. While held, skip **all**
   targets: release, cache/checkpoints, secrets, staging. Record held_cleanup and
   fail with the packaged Reconcile command then `-RepairInterruptedInstall`.
   With no hold and fresh quiescence/lease proof, use the validated inventory in
   this order: **secret, cache, stage, candidate release root last**. On an earlier
   target failure stop cleanup and retain the release root containing the packaged
   repair tools, especially after first-install rollback. Cleanup failure without
   a hold is repair_required, not success, and preserves the restored milestone.

**Owner-neutral branches:** an untouched pre-activation candidate is not launchable
through installed state; its repair uses `install` and short `holds` sections,
**not action.lock**, with census outside the leaf. This branch requires no installed
lifecycle files/task to have been touched. A partially attempted forward activation
that touched them is lifecycle restoration, even if state never named the candidate;
it follows the fenced, pre-milestone path above, not this cleanup exemption.

After a durable restored milestone, **every** pending status (`in_progress`,
`repair_required`, or `held_cleanup`) skips snapshot restoration. Cleanup itself is
owner-neutral: no action lock and no wait on a serving incumbent's run lock. Only
completion of an interrupted same-key retirement may take run, after proving that
wrapper drained. Any separate required Stop/Start owns its own action lock, outside
the installer's neutral cleanup scope. Candidate absence from restored `releases`,
retired attribution, and restored milestone (or proof no installed files were ever
touched) authorize this branch. Contradictory records refuse cleanup, not silently
re-enter restoration or delete an unknown lease.

A snapshot is not, by itself, exclusion from a future launch. The retained run
fence for forward activation and restoration closes the managed task-retry window.
A late task cannot read candidate state and then spawn after restoration: Run
acquires run.lock **before** reading state (E7). External controller calls, including
Stop/Start, are never made while the installer owns their action/run locks.

## 7. Explicit Reconcile

`-Action Reconcile -ReleaseId R` acknowledges inspected evidence; it does not
recover unsaved sessions, stop a new launch, restore the owner, delete a tree, or
mark an installer repaired. Use the retained candidate's packaged controller when
the root controller was restored to an older version or removed on first rollback.

1. Under `holds.lock`, read/validate holds and strict runtime, and capture the
   exact set of ids for R. No entries is an explicit error. Release the leaf before
   state validation or liveness/census work; state absence is supported.
   **Do not take action.lock:** Reconcile is owner-neutral, including during a
   serving incumbent's natural exit.
2. `active-new-launch` requires active R with a **different, unheld key**, a live
   attributable owned process (image and creation ticks), **and its live managed
   wrapper**. A different key, active pointer, or non-Running task alone proves
   none of this. Otherwise require R's canonical root idle: inactive/removed R,
   stopped active R with no runtime, and active R with an exited unheld newer
   launch all take the idle-census branch. Keep §5's unreadable/unattributable-row
   refusals; a missing directory alone is not proof. An exited unselected newer
   runtime is not acknowledged or deleted by this operation.
3. If selected ids still occupy runtime, first prove their wrapper drained, then
   acquire a retained `run` fence outside the leaf and repeat fresh quiescence
   before completing that interrupted same-key retirement. No other Reconcile
   path takes run. A hold is not a substitute for present liveness proof. Never
   delete a newer runtime key.
4. Under holds.lock, re-read the envelope and runtime; revalidate the key/bindings
   used by the proof. A changed key requires fresh proof or refusal, not reuse of
   a stale liveness decision. Persist a unique receipt in **`StateRoot\receipts`**
   containing the selected entries still present, finish any proven same-key
   retirement, then remove **only those ids**, replacing the envelope with an
   empty array when appropriate. Entries added after selection survive, including
   new holds for R. No whole-document overwrite from an earlier snapshot.
5. Print the reconcile receipt directly, not `Get-StatusObject`. A write failure
   leaves the hold; a crash after receipt publication but before removal leaves a
   safe retry (possibly another audit receipt), not implicit acknowledgement.

The candidate installer is used for subsequent cleanup. Reconcile can run while
an incumbent serves and does not require a lease-free *incumbent*. Installer repair
separately requires the candidate's lease retired, and any other surviving lease
demonstrably attributed rather than unknown (§5), before deleting candidate trees.
The StateRoot receipt remains after that deletion.

## 8. Failure matrix

`H(L)` = immutable hold; `rt(L)` = live slot; tests below are planned, not executed.

| Trigger | Durable state after | Visible result | Recovery/retry | Cleanup | Decisive test |
|---|---|---|---|---|---|
| Missing/unreadable/malformed/foreign/path-mismatched report | H(L), runtime retired; owner restored if successful | Stop held failure, or success with AllowHeld | Inspect/Reconcile | Withheld | T1, T9 |
| Explicit incomplete/refused final save | H(L), first receipt and counts | Same | Start may serve; later clean Stop cannot clear H | Withheld | T1, T2 |
| Forced/nonzero/timeout/signal failure; process now gone | H(L), causal receipt | Same | Reconcile | Withheld | T1 |
| Terminate predecessor or clean bound stop-event | Runtime retired, unrelated H unchanged | Success | Normal lifecycle | Only if candidate has no H | T1, T2 |
| Still-running child, stalled wrapper, helper in root, uncertain CIM, including blank image plus missing/blank Name | Runtime/lease and first failure receipt retained | Hard failure even with AllowHeld; installer also refuses | Prove exit, retry Stop; persisted sticky failure then holds | None | T3, T8, T10, T12 |
| Leaf timeout / hold write fails | Runtime/lease retained; first failure receipt if writable | Hard failure | Retry after I/O/lock recovery; never infer absent from parse error | None | T8 |
| Death/fault during replacement of holds, runtime, or last-stop | Either old or new valid destination; missing destination plus publication temporary is corrupt, never empty | Recoverable old/new state or fail-closed mutation error | Strict read and ordered retry; last acknowledgement keeps an empty envelope | None on uncertainty | T8, T12 |
| Hold published, runtime delete fails or process dies | H(L) + rt(L); lease may remain | Failure or interrupted action | Fresh proof and compare-retire before restore | None | T8, T9 |
| Owner restoration fails after retirement | H if applicable; runtime absent; lease retained | Hard failure, not a tolerated hold | Stop retry restores recorded owner; no hold duplication | None until retired lease | T3, T9 |
| First failed Stop persisted, followed by complete report | First receipt survives; eventual H(L) | Held, not newly clean | Reconcile | Withheld | T8 |
| First non-clean receipt cannot be persisted at all, then controller exits | Runtime/lease retained by failing invocation, but no durable first verdict | Current invocation fails; a fresh invocation may evaluate a later complete bound report as clean (§3 limit) | Restore I/O, evaluate surviving evidence; no guarantee of an unwritten hold | Refused now; later governed by surviving evidence | T8 |
| Serving child exits during Reconcile or owner-neutral installer repair | Wrapper retires its own lease after proof; neutral caller never claims handback | No stranded lease and no additional Stop required, including pre-hold wrapper | Normal wrapper cleanup; neutral record mutations remain serialized | Only independently proven candidate cleanup | T3, T6 |
| Successor reuses lease / inherited lease has unproven refresh | Refreshed release binding / unchanged unknown lease | Normal acquire / cleanup refusal, not inferred candidate absence | Managed Stop retires unknown lease; do not delete it | Blocked while candidate-owned or unknown lease remains | T3, T13 |
| Run sees prior exited/foreign launch | Old H or clean retirement precedes fresh intent | Replacement allowed only after retirement | Reconcile old H separately | Old candidate protected | T4 |
| Spawn known not to occur / wrapper dies around spawn | Own intent removed / uncertain intent retained | Run failure | Uncertain case censused and evaluated | No premature cleanup | T4, T8 |
| Run update vs Stop snapshot | One L; newest runtime copied at first hold | Normal held result and owner retirement | No equality workaround | Withheld | T5 |
| Run appends while Reconcile removes another hold | Both updates serialized; new H retained | Both actions complete | No recovery needed | Only acknowledged ids eligible | T6 |
| Held candidate rollback, including old incumbent | rt(C) absent before real Start; H(C); held_cleanup | Incumbent available, installer reports held cleanup | Reconcile then cleanup-only repair | All candidate trees retained | T9 |
| Legacy incumbent handoff refuses before candidate activation | Incumbent report/runtime retained until successor retirement publishes its H; incumbent trees never targeted | Original upgrade failure retained; recovery may Start only after retirement | Successor retires any exited slot before legacy Start | Candidate-only and subject to its own holds | T9, T10, T13 |
| Exited/retry-pending incumbent at forward activation, including NoStart | Any exited slot retired before fenced replacement; held evidence retained | Install may proceed only under action/run fence; live retry causes refusal | Managed Stop outside inner locks, then revalidate | No bypass based on task State or absent lease | T10 |
| Retry of held_cleanup / post-Reconcile repair | Unchanged / repaired; StateRoot reconcile receipt survives | Held diagnostic / passed repair | No repeated restore or Start; no action lock or serving run-lock wait | Deferred / candidate only | T7, T9 |
| First-install held rollback | No state/root controller; packaged controller and H retained | Held diagnostic | Packaged Reconcile without state, then repair | Deferred | T7, T9 |
| Task Run arrives between Stop and restore | Fence excludes it, or rollback refuses before restoration | Safe restoration or repair_required | Retry with candidate quiescent | None on refusal | T10 |
| Crash after state/all snapshots restored but before restored milestone persists | Pending without milestone; snapshots may already name incumbent | Re-application requires bounded action/run fence; serving incumbent gets Stop-then-retry diagnostic | Idempotent fenced re-application only after managed Stop drains it | None until restored proof | T10 |
| Crash after restored milestone or Start, or no-hold cleanup I/O failure | Monotonic restored phase plus pending/repair_required; incumbent may serve | Cleanup-only retry, not run-lock timeout or snapshot replay | Skip snapshots in every post-milestone pending status; preserve diagnostics | Resume validated candidate-only inventory | T9, T10 |
| Secret/cache/stage cleanup fails before release-root deletion | Candidate release root and packaged repair entrypoint retained; milestone unchanged | Pending repair failure | Packaged repair retries; release root is last and skipped after earlier failure | No further targets deleted in failed attempt | T9, T10 |
| Reconcile active R stopped, or newer unheld key exited | Old selected H acknowledged only after idle-root census; newer runtime unchanged | `idle`, not `active-new-launch` | Live-new path requires owned process and bound wrapper; otherwise census/refuse | Separate installer proof still required | T7 |
| Reconcile receipt write fails / concurrent new same-release hold | Existing H remains / new H not selected remains | Error / receipt for captured ids only | Retry explicitly | No implicit deletion | T6, T7 |
| Uninstall sees old H or creates H during Stop | Root/task retained; new-H case already stopped | Refusal before inner prior-power restore | Reconcile each release, retry | None | T11 |
| Corrupt envelope/runtime or conflicting launch bindings | Original bytes retained | Mutation/admission fails closed | Operator restores valid evidence; no automated repair by deletion | None | T12 |
| Phase-15 qualification restore observes held scratch Stop, then automatic cleanup re-runs | Durable union of held launch ids in restoration record; safe host restoration may be passed | Qualification/summary remain failed with no qualified timestamp or passing lane publication | Empty scratch retry preserves marker; never auto-Reconcile | Qualification does not authorize held-tree deletion | T13 |
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
| T3 — Composed callers | Real Restart, Rollback target/recovery starts, Start-failure cleanup, and owner-restore failure. Held allows non-destructive progress; stalled wrapper/live child never does. Original start failure remains identifiable; no lease stranded or prematurely handed back. Reused lease gains the acquiring release id without changing prior pause/inner claim. Exit a serving child during each owner-neutral action (Reconcile, untouched pre-activation repair, post-milestone cleanup); verify no action-lock suppression and exact owner handback without a later Stop, for both controller generations. |
| T4 — Admission | Start and Run independently cover same/foreign installed/removed-release runtime, missing/refused/complete reports, live census, null-PID intent, spawn failure, attestation kill, and hold-plus-runtime crash window. Old report is consumed before removal. |
| T5 — Stable-key mutation | Same L changes PID/ticks/attestation between snapshot and final read. Hold captures latest runtime, original receipt persists, owner retires, Rollback reaches real Start. A different L must refuse stale retirement. |
| T6 — Two-process Run/Reconcile races | Deterministic barrier schedules below; assert exact surviving keys and StateRoot reconcile receipt, not just exit codes. Include a new same-release hold after selection. Also pause owner-neutral Reconcile while a serving child exits; neither generation's wrapper may strand its lease or require an additional Stop. |
| T7 — Reconcile | Public packaged path without state, installed inactive, removed candidate, stopped active root, exited unheld newer runtime, and genuinely live active-new-launch (owned process plus bound wrapper); absent/foreign wrapper cannot qualify as live-new. Busy/unreadable/unattributable root, selected-key interrupted retirement, no holds, and receipt I/O failure. Acknowledgement itself leaves newer runtime/incumbent lease unchanged; the independent wrapper-exit case is T6. Receipt bytes survive later candidate cleanup; last acknowledgement leaves a valid empty envelope. |
| T8 — Stop/crash persistence | Kill/fault at forced-intent receipt, hold publication/replacement, runtime replacement/removal, last-stop replacement, and lease restore; inject lock timeout. A persisted first non-clean receipt must survive a later complete report. Separately fault every first-receipt publication: current invocation refuses retirement/handback, while a fresh invocation has the documented surviving-evidence limit, not a fabricated durable verdict. Corrupt runtime or missing destination with publication temporary never becomes empty. |
| T9 — Real held installer rollback | Full sequence below, all held outcomes, first install, restoration/Start failures, and repeated repair. Also fail secret/cache/stage deletion after acknowledgement and assert packaged repair tools remain because release-root deletion has not begun; resume and retain StateRoot receipt bytes. Replaces `test_incomplete_shutdown_blocks_rollback_and_repeated_repair`, not its evidence-retention assertion. |
| T10 — Forward/restoration exclusion and retry | Separate Run before/while either activation or restoration owns run; no candidate spawn across replaced state, including `-NoStart` with an exited or retry-pending incumbent. Both incumbent generations: crash after state restoration but before controller/task restoration, after all snapshots but before milestone publication, after durable milestone, and after incumbent Start. Before milestone, bounded fencing or explicit Stop-then-retry refusal; after it, no snapshot replay, phase overwrite, or serving run-lock wait, even for no-hold cleanup failure. Keep E17's census scenarios except change blank-image plus missing/blank Name from allowed cleanup to refusal. |
| T11 — Uninstall | Old hold refuses before Stop; newly held Stop refuses before inner power restore and task removal; post-Reconcile retry succeeds. |
| T12 — Structural and publication invariants | Strict envelope/array/root/key checks; property-safe Name under StrictMode in both scripts. Blank-image plus missing/blank Name refuses Stop proof, owner restore and installer cleanup, retaining runtime/leases/trees; named unrelated or readable foreign images remain distinct. All runtime/hold/sticky-receipt writes use asserted leaf ownership and identical shared helpers; no serialized-record identity comparisons. Last hold removal replaces with an empty array. Fault/kill existing-file replacement for each artifact: old or new valid bytes, never silently absent; missing-with-temporary refuses. Retain script parse/inventory checks and require §12's Windows stress proof. |
| T13 — Mixed tools and qualification | Real pre-hold incumbent refuses final save during upgrade handoff; successor retires that incumbent launch before legacy restart, preserving its hold/report evidence and trees. Capability-detected AllowHeld on successor handoff permits progress but retains incumbent holds. Unrefreshed/stale outer lease attribution blocks candidate cleanup until managed Stop, including after losing in-memory refresh proof. Old-tool warnings remain explicit. Qualification's admission/evidence gates stay strict; phase-15 held Stop after phases 1–14 pass persists held ids, restores incumbent, then survives automatic cleanup/restore re-entry against an empty slot. State, summary and receipt hashes stay consistent and failed, safe restoration is not falsely failed, and no passing lane artifact is published. |

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
6. If an incumbent lease remains without current successor-refresh proof, repair
   refuses without stopping the incumbent or taking its run lock; an explicit
   managed Stop must retire that unknown lease before cleanup. Then repair deletes
   candidate-only trees in root-last order and records repaired without replaying
   snapshots/Start; the StateRoot reconciliation receipt remains byte-equivalent.

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

**T6 owner-handoff schedule:** seed an older hold and a serving unheld launch.
Process 1 enters public Reconcile (repeat for inactive-candidate and active-new-launch
selection), proves its branch, and pauses in a bounded leaf section. Assert it
does **not** own action.lock. Let the serving child exit and its real wrapper pass
the post-run action probe while Reconcile is still in progress. Release the leaf
barrier without waiting on wrapper completion under that lock. Both actions must
finish with exact prior owner state restored, outer/inner claims retired as
applicable, selected hold acknowledged, and no extra Stop. Repeat with the exact
pre-hold wrapper and the successor wrapper; T3 applies the same schedule to each
owner-neutral installer branch. This proves mixed-version handback, not merely
that acknowledgement leaves an unrelated live lease untouched.

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
  active-new-launch service need not stop when owned-process and wrapper liveness
  are proven; stopped active roots and exited newer launches require idle census.
  Rolled-back/first-install recovery uses
  `releases\<id>\bin\lifecycle\Control-Release.ps1 -Action Reconcile -StateRoot
  <root> -ReleaseId <id>`, then the candidate/newer installer with
  `-RepairInterruptedInstall`. Audit receipts stay in `StateRoot\receipts` after
  candidate cleanup. Never recommend deleting hold/runtime files.
- Reboots/crashes that leave unreported stop-event attribution can accumulate
  holds. No background expiry; Uninstall requires acknowledgement of every release.
  Whole-root census now also blocks on helper/benchmark images in the release root.
  Blank-image rows with missing/blank Name refuse in both scripts; they are not
  evidence of an idle root. Legitimate concurrent-tool use needs the §12 proof.
- Owner-neutral Reconcile, untouched pre-activation repair, and post-milestone
  cleanup do not hold action.lock, so even a pre-hold wrapper can restore its owner
  on natural exit. Interrupted same-key retirement takes run only after wrapper
  drain; these actions do not stop a serving incumbent. A lease with no proven
  successor refresh nevertheless blocks destructive candidate cleanup: the
  diagnostic requires explicit managed Stop, not an inferred lease release id.
  This may defer cleanup while the incumbent continues serving.
- A repair missing the lifecycle-restored milestone cannot assume the installed
  files are already restored just because state names the incumbent. It must
  acquire the bounded action/run fence before snapshot re-application; if a
  restored incumbent serves, the operator must Stop it and retry. A durable
  milestone is never overwritten by later errors, and cleanup-only retry does not
  wait on a serving incumbent's run lock, even when no candidate hold exists.
- Qualification admission and evidence phases in §2 keep strict Stop; do not add
  AllowHeld globally. Restart's existing graceful/unforced assertions stay intact.
  **Final `qualify_native.restore()` is a recovery caller**, however: currently its
  strict scratch Stop precedes incumbent Start ([1993–2038][e-q-restore]), so leaving it unchanged
  would reproduce R1-A. It must tolerate only a proven held Stop (capability-detected
  AllowHeld), complete owner/census checks and incumbent restoration, and retain a
  failing qualification outcome independently of whether host restoration succeeds.
  **Persist the union of observed held launch ids as `held_launch_ids` in the
  existing restoration record on every attempt**, before subsequent recovery
  work can fail. Separate observation/persistence of the Stop result from later
  remote recovery steps; an error later in that script must not discard the held
  observation. Merge this durable marker into every later `required`, `failed`,
  or `passed` restoration update; never replace it with an empty retry result.
  `execute()`'s finally/cleanup path and any resumed restore must preserve it,
  including an already-empty scratch Stop on the second attempt.
  When owner/host/incumbent recovery succeeds, `restoration.status` may truthfully
  be `passed`, but any recorded held id forces qualification state/summary to
  `failed`, `qualified_utc` to null, and prevents passing lane publication. Keep
  the summary, phase receipt and their hashes consistent; all non-restore phases
  being passed is insufficient. An actual recovery failure remains a restoration
  failure. No qualification schema-version bump is required for this detail.
  Do not auto-Reconcile, publish a passing final-save result, or catch exception
  text as a result enum. T13 must exercise phase 15 after the receipt phase and
  the automatic cleanup/restore re-run against the emptied slot.
- A pre-hold incumbent restored by rollback can Start because candidate attribution
  and any other exited attribution are already retired by successor code. It
  cannot enforce hold-aware Uninstall or preserve new holds for its own launches.
  Plain Stop failure during upgrade must therefore pass through successor
  retirement before rollback invokes that incumbent's Start. Explicit use of its
  old Uninstall may delete retained evidence with the root; use updated packaged
  tools for acknowledgement/cleanup.
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
2. **No unsafe “held runtime is harmless” exception.** Installer retires every
   exited slot, not just candidate attribution, before activation or a restored
   Start of either controller generation. Hold-plus-runtime crash windows must
   finish retirement; corrupt runtime never counts as absent.
3. **Actual exclusion, not a stale snapshot.** Retain action/run fencing across
   forward replacement and snapshot application/re-application, even if state
   already names the incumbent. Journal restoration before Start and never erase
   the milestone on later failure; only post-milestone or untouched candidate
   cleanup is owner-neutral.
4. **Sticky receipt and crash ordering.** First non-clean evidence must survive
   retries once persisted, even when hold publication fails. An unwritten first
   receipt has the explicit §3 limitation, not a fabricated durability promise.
   Same-key retirement, not a JSON comparison, is decisive; a leaf lock is not
   multi-file atomicity. Old-or-new replacement and a never-deleted hold envelope
   prevent missing files from silently becoming lost evidence.
5. **Exceptions are not a liveness enum.** Receipt/retirement/owner I/O can fail after
   stopping; hard errors still refuse destructive progress but do not imply all
   earlier writes rolled back. Start cleanup cannot hand back a lease after an
   unproven wrapper stop.
6. **Reconcile requires liveness or idle proof.** Only an attributable live owned
   process plus its wrapper permits active-new-launch acknowledgement. Census
   stopped active, exited-newer, installed-inactive and removed roots alike.
   Never treat a missing root or unattributable CIM row as idle evidence.
7. **Real test scheduling.** Two processes and parent-controlled barriers, not a
   writer waiting for an operation blocked by its own lock; real Start/Run for the
   restored incumbent, not a sentinel substituted for lifecycle code.
8. **Qualification recovery is in scope.** Rather than leaving the qualification
   caller unchanged, final restoration must finish safe incumbent recovery on held
   scratch Stop while keeping qualification failed. Durable held ids survive every
   cleanup retry; safe host restoration and final-save qualification are separate
   verdicts. All evidence/admission Stops remain strict.
9. **No owner-neutral action claim.** Avoid action.lock for Reconcile and neutral
   installer cleanup instead of changing the wrapper probe or adding a second
   ownership mechanism. This preserves a pre-hold wrapper's handback behavior.
   Refresh reused outer leases on successor Acquire; unknown provenance refuses
   cleanup rather than inventing a release binding.

## 12. Implementation order and open questions

One cohesive successor implementation; do not merge intermediate behavior where
controller held semantics and installer/caller handling disagree.

1. Add state-faithful dispatcher fixtures and red T1/T2/T5/T6/T9 proofs first.
2. Implement strict readers, stable-key helpers, common leaf lock, and centralized
   runtime/hold/sticky-receipt old-or-new replacement; never delete a created hold
   envelope. Keep packaged-file inventory unchanged.
3. Implement verdict/retirement, admission, structured Stop, and every controller
   caller together, including owner-proof gating and Uninstall's second hold check.
4. Add owner-neutral Reconcile and StateRoot acknowledgement receipts, successor
   lease-binding refresh, installer held_cleanup, forward/restoration fences,
   monotonic restored milestone, root-last cleanup, and pre-hold handoff retirement.
5. Update qualification recovery with durable held ids across cleanup re-entry,
   focused regressions, and Windows release
   notes. Supersede the explicit-refusal-only containment paragraph (E15) with the
   new behavior, commands, and old-tool limitations; no claim to recover unsaved data.
6. Review the frozen implementation after focused red/green proof; then normal
   Windows/package and hardware qualification before release. This draft has none
   of those implementation receipts and does not authorize deployment.

**Open questions / required implementation proofs:** these remain unexercised.
`defer` below means the implementation must supply the named evidence, not that
release may waive the boundary. Defaults and fail-closed behavior above apply.

- **D50-11, with D50-16 — replacement/lock contention:** on Windows PowerShell 5.1
  **and** pwsh, run multi-process continuous Status polls plus Run/Stop/Reconcile
  leaf writes with antivirus scanning enabled. Require zero unhandled IOExceptions
  or lock timeouts at the proposed 60-second bound; test empty/one/many hold-array
  round trips. Kill/fault each existing-file replacement for holds, runtime and
  last-stop: destination must remain old or new valid bytes, and missing-with-temp
  must refuse. No remove-then-move gap may be accepted as an empty envelope.
  This proves D50-16's required mitigation as well as the deferred contention
  question. Do not relax refusal or hold the leaf across external work.
- **D50-12 — Task Scheduler restoration fence:** on supported Windows, cause a
  nonzero wrapper exit and enter the one-minute failure-retry interval; interrupt
  rollback/repair while retaining run across task unregistration/registration and
  snapshot restoration (T10). Prove no queued candidate Run executes across or
  after restoration with stale candidate state. Include before-milestone and
  after-milestone crash/retry schedules for both incumbent generations. A failure
  requires fixing that boundary, not accepting a census-only proof.
- **D50-17 — explicit Stop stickiness against task failure restarts:** rehearse
  standalone held Stop of an unreported launch on supported Windows and observe
  task state/runtime/holds for at least four minutes, covering all three configured
  one-minute retries. Establish whether Stop remains stopped or managed retries
  relaunch/add holds. Record observed behavior and any required correction before
  claiming the intended Stop contract; do not assume the report-based retry
  suppression applies when there is no bound report.
- **D50-18 — concurrent release-root tools:** inventory qualification phases that
  run packaged CLI, benchmark or qualification images concurrently with Stop or
  wrapper owner restoration. Rehearse those overlaps under the broadened census.
  If any exclusion is legitimate, name it and prove why it cannot hide the server
  or candidate workload before making the census exclusion explicit. Until then
  the whole-root rule refuses; no silent exception for qualification binaries.

Operator acknowledgement wording must distinguish preserved checkpoints from
unrecoverable refused sessions. The specified default is release-scoped explicit
Reconcile, a StateRoot audit receipt, and no automatic cleanup. Qualification's
durable held-id detail is required by §10, not an open optional schema choice.

## 13. Closed design-council dispositions

Neutral identifiers enumerate all eighteen ledger rows in their recorded order;
duplicate claims retain separate rows. Severity is the lead's residual rating
(reported rating shown where changed). `mitigate` records the design change, not
an implemented or tested fix; `defer` records a mandatory implementation proof in
§12. No row authorizes deployment.

| Row | Short claim | Severity | Disposition | Design change / required proof |
|---|---|---|---|---|
| D50-01 | Blank image plus absent/blank CIM Name was treated as idle | P2 (reported P1) | mitigate | §5/§10 fail closed in both scripts; §8 matrix and T10/T12 replace the unsafe cleanup expectation. |
| D50-02 | Snapshot retry before restored milestone could race a serving incumbent | P2 (reported P1) | mitigate | §4/§6 bounded action/run fence before every re-application; explicit Stop-then-retry refusal; T10 before-milestone crash schedules. |
| D50-03 | Owner-neutral actions could suppress wrapper lease handback | P2 (reported P1) | mitigate | §4/§5/§6/§7 remove their action lock; run only for drained interrupted same-key retirement; §8 and T3/T6 prove both wrapper generations. |
| D50-04 | Legacy incumbent refusal could be erased by rollback Start | P2 (reported P1) | mitigate | §6 retires every exited slot before restored Start; capability-detected AllowHeld for successor handoff, plain Stop plus successor retirement on legacy failure; §10 and T9/T13. |
| D50-05 | Exited/retry-pending incumbent bypassed forward activation fencing | P2 (reported P1) | mitigate | §4/§6 install/action/run fence before installed-file replacement and retire-before-activation even with NoStart; §8 and T10. |
| D50-06 | Qualification restore re-entry could erase held failure and publish passed | P1 | mitigate | §2/§10 durable restoration held ids merged through every attempt; safe host recovery remains independent of failed qualification; §8 and phase-15/re-entry T13. |
| D50-07 | Cleanup could delete its packaged repair entrypoint before a later target failed | P2 | mitigate | §6 secret/cache/stage before release root, stop on earlier failure; §8 and first-install/retry T9/T10. |
| D50-08 | Reconcile receipt destination could disappear with candidate cleanup | P2 | mitigate | §3/§7 require StateRoot receipts; §8, T7 and T9 retain byte-equivalent receipt after deletion. |
| D50-09 | Later errors could erase restored milestone and block serving cleanup retry | P2 | mitigate | §3/§6 monotonic phase across all pending statuses; explicit no-hold cleanup-failure transition, owner-neutral post-milestone retry; §8 and T9/T10. |
| D50-10 | Reconcile omitted stopped-active/exited-newer roots; lease release binding could be stale | P2 | mitigate | §5 successor Acquire refreshes reused release id; unknown attribution blocks cleanup until managed Stop. §7 requires process-plus-wrapper liveness or idle census; §8 and T3/T7/T13. |
| D50-11 | Windows replacement and handle contention are unproven | P2 | defer | §12 required Windows PowerShell 5.1/pwsh multi-process Status/write stress with antivirus enabled and zero lock timeouts/unhandled IOExceptions, together with D50-16. |
| D50-12 | Queued Task Scheduler retry might cross interrupted snapshot/task restoration | P2 | defer | §12 required supported-Windows failure-backoff/run-fence rehearsal; T10 before/after-milestone schedules. |
| D50-13 | Reconcile could strand a lease through the wrapper action probe | P2 | mitigate | Same root cause as D50-03; §4/§7 owner-neutral locking and T6's explicit two-process child-exit schedule. |
| D50-14 | An unwritten first non-clean receipt cannot enforce sticky failure on a fresh invocation | P3 (reported P2) | mitigate | §3 states the surviving-evidence limit and current-invocation refusal; §5 state table, §8 matrix and T8 distinguish persisted from unwritten evidence. |
| D50-15 | Fully restored snapshots without a durable milestone left retry ambiguous | P2 | mitigate | Same root cause as D50-02; §6 fenced idempotent re-application or Stop-then-retry refusal, §8 and T10 for both incumbent generations. |
| D50-16 | Delete-then-move publication could silently lose the holds envelope | P1 | mitigate | §3/§4 never delete a created envelope, require old-or-new replacement for holds/runtime/last-stop, and refuse missing-with-temp; T8/T12 and §12 Windows proof. |
| D50-17 | Explicit held Stop versus task failure relaunches is unproven | P2 | defer | §12 required at-least-four-minute Windows rehearsal of an unreported held launch and all configured failure retries. |
| D50-18 | Whole-root census may overlap legitimate packaged qualification tools | P3 | defer | §10/§12 require phase inventory and overlap rehearsal; any legitimate exclusion must be explicit and proven before narrowing census. |

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
