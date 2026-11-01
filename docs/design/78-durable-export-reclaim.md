# Durable export: reclaim before a contended Host-capacity refusal

Status: proposed design, not an implementation or a release qualification.
Issue: [#78](https://github.com/alphastorm/ninfer/issues/78).
Source examined: `0628db0b0997c8bda4ac37c8e84da95986890014`.

This document verifies and revises the preparation plan `prep-ninfer-78-export-re-BAC09D`.
Its recommendations are proposals; the source citations below describe the baseline.
The existing [durability contracts](../durable/CONTRACTS.md), especially K3 and K7,
remain authoritative until an implementation updates them. This follows the
[documentation map](../README.md): maintainer references describe current contracts;
this issue-specific document describes planned work. Only this document changes in
this PR. No runtime, checkpoint format, controller, or deployment changes are made.

## 1. Problem and scope

The incident recorded in #78 ended with `saved=1, skipped=8, refused=1,
complete=false`; the refused save's reason was `capacity`. The Host arena was
97.1% occupied (87 state images, each 195,897,344 bytes, plus approximately 3.9 GiB
of KV). The refused session's durable generation was four minutes behind its
acknowledged endpoint. Automatic saves had also been refused before shutdown.
The controller correctly failed closed. Transcript data remained available for
replay; the missing guarantee was an up-to-date native continuation and a complete
managed Stop. These are reported incident observations, not measurements made by
this design PR.

The mechanism remains present at the examined source: export attempts native
preparation once; import already has bounded durable-victim reclaim. A full arena
can therefore prevent an export even when another session has a proven on-disk
copy and its resident Host bytes could be released. See [E1–E6](#2-verified-current-behavior).

**Decision:** reclaim on a specifically identified, contended Host-allocation
failure, for explicit, automatic, and final saves alike. Do not add a stopping
parameter or retry only in the service. Preserve the complete selected checkpoint
set, publication gate, byte proof, and final-save failure semantics. The fix must
work after the automatic queue has stopped accepting work.

Non-goals: increasing the arena, exporting a reduced chain, enabling durability
for a new architecture, changing on-disk schemas or fingerprints, changing import
policy, parallel disk proofs, a general compaction allocator, and changing managed
Stop's fail-closed contract. This draft has no production effects; implementation
review and host qualification belong to the subsequent change and release.

## 2. Verified current behavior

All source links are pinned to the examined commit. The labels are reused below.

### E1. Stop, save, and store locking

- [`src/serve/session_checkpoint_service.cpp:482–506`][e-final] serializes final
  saves, sets `stopping_`, drains the automatic queue, and calls `save_session` once
  per response-store session. Saved increments `saved`; covered or `NoSessionRecords`
  increments `skipped`; every other outcome increments `refused`. There is no retry.
- [`src/serve/session_checkpoint_service.cpp:219–259`][e-save] holds a per-session
  operation mutex, takes the Responses snapshot, optionally checks coverage, and
  passes export and publish callbacks to the store. This mutex is not an Engine
  publication gate or the store catalog mutex.
- [`src/serve/session_checkpoint_store.cpp:2031–2146`][e-store-export] takes the
  catalog mutex during staging, **unlocks before `exporter(buffered)` and
  `buffered.finish()`**, and reacquires it afterwards. `CheckpointSaveLease` retains
  a save-in-progress flag and generation pins, not a continuously held mutex
  ([`:1084–1138`][e-save-lease]). It prevents another save, not a load.
- `pin_recoverable` calls `store_.load` with the active fingerprint and requested
  tag, rejects responses-only generations, and requires the loaded latest tag to
  match ([`session_checkpoint_service.cpp:384–393`][e-oracle]). Load takes the
  catalog mutex ([`session_checkpoint_store.cpp:2324–2355`][e-load]); fresh readers
  take it briefly too ([`:1608–1622`][e-fresh]). Neither takes the service's session
  operation mutex. Thus victim lookup during the unlocked export callback does
  **not** self-deadlock on the store lock (U1 resolved).
- Final publication separately drops the catalog lock before entering the Engine
  gate; its callback reacquires the catalog lock only for pointer publication
  ([`session_checkpoint_store.cpp:2228–2275`][e-store-publish]).

### E2. Engine call chain and lock scopes

- [`src/runtime/engine/engine_checkpoint.inl:72–160`][e-export]: export reserves its
  mandatory retirement command before acquiring native readers. One `checkpoint_call`
  prepares the lease; it holds the target gate while capturing epoch, choosing
  `session_points`, checking tag, and calling `prepare_checkpoint_export` **once**.
  The writer runs off the execution lock, including an inline worker save. Assigning
  `lease->write(writer)` replaces the result; only the epoch is explicitly restored.
  Retirement destroys the lease under `checkpoint_change` and advertises capacity
  once for a prepared lease.
- `checkpoint_call` runs on the device-bound worker, acquiring execution ownership
  when called inline from an unlocked pressure callback ([`:1–15`][e-call]).
  `checkpoint_change` wraps it with runtime-stat publication even on an exception
  ([`:366–390`][e-change]). Adding reclaim to plain `checkpoint_call` would omit
  that immediate stats publication, although later retirement also publishes.
- [`src/runtime/engine/checkpoint_control.h:17–20,92–105`][e-gate] defines the
  independent gate and final tag/epoch compare-and-publish. Disk publication must
  not wait for worker work while holding that gate. `Exported` from the Engine is
  staged export evidence, **not** the store's publication acknowledgement
  ([`docs/durable/CONTRACTS.md:434–448`][e-writer-contract]).

### E3. What native `Capacity` actually means

- [`src/models/qwen3_5/program/storage/checkpoint_api.cpp:351–543`][e-native-export]
  acquires checkpoint leases and constructs the full index. Capacity checks cover
  metadata staging (415–418), Host state preparation (447), copy staging (507),
  and restorability into an empty arena, including logical state/history capacity
  (531–535). `bad_alloc` also maps to `Capacity` (541–542). Busy native transactions
  instead use `TransactionBusy` (358–359). `Capacity` alone does not identify a
  reclaimable shortage.
- [`src/models/common/context/state_store.cpp:55–92`][e-host-export] rejects a
  destination pin, pending transfer, or Device image with a source reader before
  reserving copies. It reserves **all** missing Host replicas before copying or
  changing residency, then publishes the transfers and drops Device replicas.
  Its false-return/no-residency-change contract is explicit in
  [`state_store.h:140–145`][e-host-declaration].
- [`state_store.cpp:475–497`][e-reserve] shows that reservation can fail because
  there is no arena, the image is not eligible, or `host_->allocate(image_bytes())`
  fails. Allocation failure is not interchangeable with a pin or eligibility failure.
- A **successful** `prepare_host_export` can be followed by export refusal:
  unsupported KV `tokens_per_row` (475–476), staging (507), codec validation (522),
  empty-arena capacity (532–535), or later allocation failure. Replacing `Impl` in
  the catches releases leases; it does not restore Device residency. Consequently
  a refused **export** can leave its sources Host-only even though a false return
  from **Host preparation** does not. These are different guarantees.
- `ExportLease` lives in [`src/models/qwen3_5/program/program.h:407–425`][e-lease],
  not an unresolved header location. Its writer finishes payload/index writes
  before returning Engine `Exported`
  ([`checkpoint_api.cpp:293–338`][e-native-write]).

### E4. Existing import proof is necessary but its guarded release is insufficient

[`src/runtime/engine/engine_checkpoint.inl:190–291`][e-import] performs import
preparation under `checkpoint_change`, captures a fixed victim set, and bounds
attempts by `2*V+1`. For each victim it obtains a generation pin, reads the index
through a fresh reader, calls `validate_checkpoint_import`, re-pins current, and
compares index bytes and SHA-256. Both generation pins survive the command.
After a successful release it notifies the scheduler and requests admission.
The native validator consumes all payloads and requires `verify_complete()`
([`checkpoint_api.cpp:574–602`][e-validate]); a manifest or coverage boolean alone
is not this proof.

However, [`resource_manager.h:179–194`][e-durable-victims] calls `allow_release`
inside `release_durable_victim`. Engine installs **`checkpoint_pressure`** as that
guard ([`engine_core.h:109–112`][e-guard-install]); it is not a direct call to
`CheckpointControl::settle` (U2 resolved). `checkpoint_pressure` returns immediately
for an empty victim list; otherwise it unlocks execution around `settle`
([`engine_checkpoint.inl:423–429`][e-pressure]). Settle maintains a Pending latch
and separately byte-proves a successful coverage answer
([`checkpoint_control.h:147–207`][e-settle]).

After queue drain, enqueue returns Dropped and a newly requested pressure victim
remains Pending ([`automatic_checkpoint_queue.cpp:19–25,62–74,149–177`][e-queue-stop]).
Ordinary completed saves do not leave pressure-cache entries indefinitely
([`:229–237`][e-queue-run]). Therefore the import-style guarded call cannot release
the incident's covered victims at Stop: it asks an unavailable queue to rediscover
a proof the Engine already possesses. A precise qualification matters: a retained,
completed, settled **pressure** entry can still answer Settled after drain
(157–163). “Every possible victim always fails after drain” is too broad; every
attempt requiring a new queue task fails. The design must work without any such
cached answer, as in the reported failing stop.

Pressure callbacks may synchronously save on the worker, outside execution
([`continuation_checkpoint.h:582–598`][e-handler]). Reclaiming recursively inside
such a save can invalidate an outer quoted release; the existing commit detects
this as `quoted cache release changed before commit`
([`resource_manager.h:1534–1554`][e-commit-release]).

### E5. Ownership, sharing, victim policy, and physical bytes

- [`resource_manager.h:123–177`][e-session-points] clears obsolete durable names on
  supersession, exports all valid points of the newest matching inactive owner,
  and offers inactive, named, nonempty owners other than the target as durable
  candidates. Candidate enumeration is owner-vector order, not a separately sorted
  “oldest durable save first” policy. Membership alone proves no disk coverage.
- [`resource_manager.h:319–380`][e-prepare-source] carries a donor owner's points
  into a fork. [`:1137–1146`][e-forget] removes an erased handle from **every** owner,
  shared entry, and waiting record. `allow_release` consequently enumerates every
  intersecting durable `(session, tag)`, including active owners
  ([`:1557–1572`][e-allow]). Proving one owner does not authorize forgetting another
  owner's acknowledged endpoint.
- `ReclaimRights{ReclaimPurpose::OptionalWrite}` is valid aggregate construction;
  `request` defaults to zero ([`context_cache/types.h:14–21`][e-rights]).
  `may_revoke` rejects every intersecting waiting source under those rights,
  including binding sources ([`resource_manager.h:959–972`][e-revoke]). This is
  stricter than import's current default Execution rights.
- Existing Host reclaim uses redundant-replica removal, native physical-release
  plans, recovery-loss ranking and whole-plan commit
  ([`resource_manager.h:661–750`][e-reclaim], [`:1575–1663`][e-host-victims]).
  A cursor contains fixed candidate membership. **It does not exclude active
  owners by itself:** `victims()` enumerates all owner points, shared entries, and
  waiting-held handles ([`:1229–1281`][e-victim-order]). OptionalWrite protects
  waiting sources, not all active-owner points. Explicit active-owner exclusions
  are required if the design promises not to evict them.
- Redundant Host removal protects the state image and all mapped KV pages reachable
  from excluded checkpoints, and also every leased checkpoint
  ([`qwen3_5/program/storage/checkpoints.cpp:276–339`][e-redundant]). U3 is resolved:
  exclusion is transitive through the native contents, not just a descriptor ID.
  The exported set cannot lose a Both replica in this tier when correctly excluded.
- Native release accounting accounts for shared image references and released KV
  pages, not the sum of logical checkpoint sizes
  ([`qwen3_5/program/transactions/reclaim.cpp:304–355`][e-physical-release]). Use
  `host_bytes_released` as a preflight and measured physical occupancy as the
  progress authority; Device-only or physically shared candidates may free zero.

### E6. Automatic retry and receipts

[`automatic_checkpoint_queue.cpp:206–237`][e-queue-run] makes Capacity retryable,
but only an observed **engine** progress change wakes that deferred save. I/O
refusals use storage progress. The service separately counts successful storage
operations ([`session_checkpoint_service.cpp:126–134,273–276,419–449`][e-service-progress]);
`notify_checkpoint_progress` deliberately does not advance observed engine progress
([`engine_checkpoint.inl:30–36`][e-notify]). Thus another session becoming durably
provable does not currently wake a Capacity retry on an otherwise idle engine.

Receipt keys are produced by [`session_checkpoint_service.cpp:89–96,139–151`][e-receipts].
K7 explicitly permits additive bounded counts/durations
([`docs/durable/CONTRACTS.md:876–893`][e-k7]); it does not permit raw identities or
weakening `committed=true`. No new reason or shutdown-report schema is needed.

## 3. Proposed minimal mechanism

### 3.1 A typed native shortage, not another public refusal reason

Replace the internal boolean Host-preparation result with a small value containing
`Kind {Prepared, Busy, NoHostArena, HostCapacity}` and `shortage_bytes` (zero unless
Host allocation actually failed). Migrate the native caller and existing tests;
do not keep a defaulted output-pointer compatibility overload. The low-level
classification need not duplicate missing-image count, required bytes, free bytes,
and a separately parameterized deficit method in a long-lived object.

After eligibility checks, calculate missing-image demand with checked arithmetic
and sample free arena bytes **before reservations**. Allocate each missing image's
Host allocation explicitly, then pass it to the existing reservation overload
(E3). An allocation miss is HostCapacity; a failed eligibility/reservation is not.
On HostCapacity report `required-free` when positive; when free bytes suffice but
no contiguous allocation fits, request at least one image's allocation size. This
is a reclaim target, not a promise that freeing that total eliminates fragmentation.
Rollback all temporary reservations before returning; preserve the all-or-nothing
false/refused Host-preparation invariant.

Expose only `host_shortage_bytes()` on the qwen3_5 export lease. Preserve that value
through the refusal catch that currently replaces `Impl`. Engine enables reclaim
only for `CapacityRefused`, reason `Capacity`, and a positive shortage. Other
Capacity sites and all other states remain non-reclaiming. Keep existing public
state/reason mappings; this is an internal distinction, not a new K7 reason.

Move the already-computable deterministic checks before Host materialization:
KV plane support, maximum per-record staging, total native state+KV payload versus
empty-arena capacity, and logical state/history counts. Use the existing geometry
and checked arithmetic, not index encoding or duplicate payload construction.
The empty-arena comparison excludes `engine/index.cbor`, as the baseline does;
receipt `payload_bytes` includes the index. Moving these checks prevents a known
impossible export from evicting cache first. It **does not** promise that every
later corrupt/allocation/I/O/stale-publication failure leaves original residency
unchanged. A valid, fully demoted source remains a valid continuation on such a
failure; no rollback to Device is needed.

### 3.2 One worker command; three finite reclaim tiers

Keep writer and pre-reserved retirement unchanged. Run the preparation/reclaim
phase in one `checkpoint_change` command. Keep the requested `(digest, tag)` fixed;
never silently switch to a newer acknowledged tag.

For each native prepare attempt:

1. Check Engine availability. Hold the target gate only to capture its epoch,
   resolve `session_points`, check the tag, and create the lease. Keep the same
   gate object for the whole command. Empty/pending/tag-mismatch behavior is unchanged.
2. On Prepared, increment the in-flight export count once and leave the loop.
3. Otherwise destroy the refused lease on the worker before reclaim; retain its
   reason/shortage as plain values. If the shortage is zero or this command is
   inside a pressure callback, stop with that refusal.
4. At the first contended refusal, freeze durable candidates and ordinary-cache
   cursor membership. Allocate the bounded pin vector and exclusion scratch before
   destructive work. Do not refresh either candidate set during this command.
5. Consume the following tiers in order until enough measured Host bytes have
   been freed for the shortage, or candidates are exhausted. Retry only after
   strictly positive physical Host progress. Prefer accumulating sufficient bytes
   before preparing again rather than rebuilding the export after each small release.

| Tier | Action | Protection / authority |
|---|---|---|
| 1 | One pass of `release_redundant_host` | Lossless replica removal. Exclude export handles, active-owner points, and held waiting/binding sources; native lease protection still applies. |
| 2 | Initial `durable_victims(target)` in existing order | Skip zero-Host-gain or ineligible candidates before disk I/O. Fresh native byte proof and the exclusive, guard-free release below. Never save a victim recursively. |
| 3 | Existing Host-only `ResourceManager::reclaim`, with one fixed cursor and OptionalWrite rights | Exclude export handles, **every durable-named handle**, active-owner points, and held waiting/binding sources. Offer no paused snapshots. Reuse native release quotes/ranking; no new eviction score. |

Tier 3 includes obsolete, unnamed target-session cache as well as other ordinary
cache. There is no special “trim the target's chain” tier: the newest owner's
entire valid point set is the export set; superseded owners are ordinary cache
(E5). Even if an old owner is unnamed, a handle shared with the exported owner
remains excluded. Add a narrowly named ResourceManager exclusion collector rather
than deriving ownership in Engine or assuming the existing cursor protects active
owners. Collect/deduplicate by logical handle; native helpers protect the reachable
physical contents.

Tier 3's exclusion of all durable names makes `allow_release`'s victim list empty.
The installed guard then returns immediately without unlocking execution or
consulting Pending (E4). Tier 2 does not call the guard at all. No oracle means
Tier 2 is unavailable, not that unproved durable victims become ordinary cache.
Tiers 1 and 3 may still make progress within their non-destructive/durability-safe
scope.

### 3.3 Guard-free proven release and its exact exclusivity proof

Add a narrowly scoped ResourceManager operation for a **proven durable victim**,
not a general `ignore_guard` flag. Keep import's existing guarded release behavior.
The new operation rechecks, immediately before commit:

- The original owner token still exists, is inactive, has the same nonempty
  `(session, tag)`, and has a nonempty point set.
- Every released handle is valid, unleased, outside the protected export set, and
  revocable under OptionalWrite (no held waiting/binding source).
- **No other owner references any released handle**, including unnamed, same-tag,
  and active owners. This deliberately conservative rule is simpler than allowing
  same-name or unnamed sharers. If any intersection exists, decline the whole
  victim. Shared physical images behind *different* checkpoint handles remain
  native reference-counted; they merely reduce the predicted Host gain.
- The full set has a strictly positive native Host-release quote. Allocate all
  required scratch before commit; use existing native release/forget primitives,
  not a second ownership catalog. Recheck the cheap predicates after disk proof.

The Engine supplies the authority immediately before that operation: pin the
victim's current generation; obtain a fresh reader; decode its index; validate
its native identity, metadata and all payload bytes; re-pin `(session, tag)`;
compare index byte length and SHA-256; retain both pins until the prepare/reclaim
command ends. This is E4's proof, not `covers()` and not a bare `Settled` result.
It may be factored once with import's proof, provided import's subsequent guard,
rights, attempt bound, reader refresh and failure behavior remain unchanged.

**Why the bypass is needed:** the guard is a request to arrange a save, not a
primitive for accepting already established recoverability. That request remains
Pending after queue drain. The new path establishes the stronger byte proof
itself and proves that forgetting the handles can affect only that one owner.
Therefore there is no other durable owner for which `allow_release` would have
needed a separate answer. Refuse shared-owner cases rather than building a
multi-session proof/locking transaction. The target is excluded before proof.

This is a caller/callee invariant under worker serialization, not authority from
a public boolean, a manifest field, or a caller-supplied session name. Tests must
exercise the real Engine entry point and attempt cross-owner sharing violations.

### 3.4 Bounded retry, re-entry, and failure handling

Let `V` be initial durable candidates and `C` the distinct handles in the frozen
ordinary cursor. Consume each durable candidate at most once, perform redundant
cleanup once, and permit at most `C` successful ordinary destructive steps. A
Host-only ordinary step must delete at least one still-present cursor handle and
increase free Host bytes. After Tier 1, no successful action in this command
creates Host replicas: failed contended prepares roll reservations back, and
release steps only remove content. Therefore redundant cleanup inside the reused
Host reclaim path cannot create another unbounded source of progress.

The conservative upper bound is **`prepare_attempts <= 2 + V + C`**: one initial
attempt, one after redundant cleanup, and at most one per consumed durable or
ordinary step. Usually steps are accumulated and the actual count is smaller.
Stop on no positive physical progress, exhausted membership, a noncontended
refusal, stale/tag change, Engine failure, or pressure re-entry. Do not sleep,
refresh membership, poll the queue, or retry based only on catalog changes.
If the remaining known deficit cannot be met, retain Capacity; do not erase
protected content to force success. This is a count bound, not a disk-latency bound.

Add a worker-only scoped pressure-callback depth marker around the nonempty
`checkpoint_pressure` callback boundary. An export invoked within that boundary
may prepare/write normally but may **not reclaim**. This preserves the current
outer plan while still allowing a handler to save. Empty-victim guard calls do
not enter this boundary. Keep lock/depth restoration exception-safe.

Optional reclaim allocation failure stops with the last Capacity refusal. Do not
catch an invariant failure and report success. Preallocate bounded bookkeeping
before destructive work; retain proof pins until cleanup; destroy any native
lease on the worker on every exit. Any actual catalog/occupancy mutation must
notify `scheduler_.capacity_released()` and request admission even if a subsequent
step refuses or throws. `checkpoint_change` publishes the resulting stats. The
unchanged retirement path remains responsible for the prepared export's separate,
exactly-once in-flight decrement and capacity notification.

### 3.5 Lock discipline and concurrency

| Boundary | Held | Must not be held / awaited |
|---|---|---|
| Select points, epoch, tag, native prepare | Worker execution lock and target gate | No victim proof, store publication, or queue callback |
| Reclaim and victim byte proof | Worker execution lock; generation readers | No target/victim publication gate; no save callback; no worker wait from inside a store lock |
| Store load / fresh-reader / pin retirement | Store mutex briefly, inside the proof | No service operation mutex acquired by the oracle; no Engine command awaited |
| Lease writer and buffered finish | Immutable copy leases only | No Engine execution lock or publication gate |
| Final pointer publication | Target gate, then store mutex in callback | No Program access or worker command wait |
| Native lease retirement | Worker via `checkpoint_change` | No outstanding writer/copy using the lease |

Incoming submissions can advance epochs while proof is running, but cannot run
native work concurrently with this worker command. Recapture epoch/points/tag
under the gate for each attempt; the writer still undergoes the final gate check.
Do not keep the target gate across multi-file victim I/O. Do not temporarily drop
execution to reduce that I/O latency: the fixed ownership/exclusivity argument
would then require a different revalidation protocol. Other in-flight writers
remain safe through their native leases. Waiting sources, active owners and
paused snapshots are not sacrificed to make this optional write fit.

### 3.6 Invariants

| Invariant | Enforcement / regression |
|---|---|
| Full acknowledged set, no chain truncation | `session_points` and native codec unchanged; target handles excluded in all tiers. |
| False/refused Host preparation is all-or-nothing | Reserve whole set before copies; distinguish this from later export failure. |
| Same requested tag, current publication epoch | Per-attempt capture under the retained gate; mandatory final compare-and-publish unchanged. |
| No fabricated durable acknowledgement | Engine write/finish is staging; store flush, quota, rename, sync and gate still decide `committed`. |
| No loss of another owner's durable endpoint | Fresh native byte proof plus strict logical-owner exclusivity before guard-free release. |
| No unproved durable release | All named handles protected in Tier 3; absent/stale/corrupt proof declines Tier 2. |
| No active/waiting/binding/paused victim | Explicit owner/source exclusions, OptionalWrite rights, no paused snapshots. |
| No release of in-flight export contents | Checkpoint lease predicate and native replica/reader checks. |
| No callback-induced re-entry | Guard-free Tier 2, empty-victim Tier 3, scoped pressure-depth suppression. |
| Physical progress, not guessed bytes | Native Host quote before release, arena occupancy delta afterwards. |
| Bounded work | Fixed V/C, consumed candidates, positive progress, `2+V+C` attempt ceiling. |
| Worker affinity and truthful stats | Preparation/destruction under `checkpoint_change`; notifications on mutation and unchanged retirement. |
| Stable refusal and disk contracts | Same K7 states/reasons, unchanged checkpoint schema, no new controller policy. |
| Retry exhaustion never authorizes destruction | Existing Pending/retry-budget rules remain intact. |

## 4. Receipts and automatic queue change

Add a small trailing default-initialized reclaim summary to `DurableExportResult`
(current declaration: [`durable_engine_api.h:129–134`][e-result]). Count native
prepare attempts, released durable **owners**, declined durable candidates,
ordinary destructive steps, and actual Host arena bytes freed. Do not call owner
count “sessions”: candidate membership is per owner and need not be unique by
session. Sum the per-release deltas before re-materialization; this is not the
net occupancy reduction after a successful export.

Add bounded integer receipt keys when reclaim was considered:
`prepare_attempts`, `reclaimed_owners`, `reclaim_declined`,
`reclaimed_cache_steps`, `reclaimed_host_bytes`. Include exhausted/no-safe-victim
results even when attempts remains one. A positive byte count can come from
redundant replicas with zero released owners. Do not infer successful publication
from any reclaim field.

Preserve this summary **and** captured epoch across `result = lease->write(writer)`.
Capture it in service `SaveOutcome` from the export callback even if store quota,
I/O or publication later refuses. Use one receipt-field helper for explicit,
automatic and final saves. Keep `dir_sync`, origin and committed semantics, and
the shutdown report's saved/skipped/refused/complete fields unchanged. Reuse
published occupancy metrics; no new persistent stats schema is necessary.

For deferred Capacity, change the ready predicate to
`now.engine != before.engine || now.storage != before.storage`. A successful save
can create a provable durable victim even without execution progress. Keep the
existing bounded retry counter, publication waits, I/O predicate, and Pending
semantics. Do not count the save-completion notification itself as observed
progress, and do not add an outer final-save retry loop. Update the existing test
that asserts storage progress cannot cure Capacity
([`test_automatic_checkpoint_queue.cpp:159–196`][e-queue-test]); it is intentionally
superseded, not merely supplemented with a new test.

## 5. Resolved preparation unknowns and explicit corrections

| Item | Resolution / correction |
|---|---|
| U1: store locking | E1 proves the callback/finish run outside the catalog mutex. Save-in-progress ownership is not that mutex. No store lock redesign is needed. |
| U2: installed guard | E4 proves the installed closure calls `checkpoint_pressure`, whose empty-list return bypasses the Pending latch without unlocking. |
| U3: redundant exclusions | E5 proves exclusions protect reachable state images and KV pages, plus leased checkpoints. The plan's hypothetical recopy of an excluded Both image is not expected behavior. |
| U4: qwen4_exp | Repository-wide symbol search finds no `prepare_host_export`, `prepare_checkpoint_export`, `ExportLease`, or `checkpoint_layout` in this architecture. Its [`program.h:251–334`][e-qwen4] exposes ordinary reclaim but no durable codec. Engine's requires check returns UnsupportedCodec (E2). Do not add an accessor or claim durability for Qwen3.8-Flash-Next. The documentation map distinguishes that qwen4_exp instance from the qwen3_5 Qwen3.8-27B instance. |
| U5: rights and geometry | E5 confirms OptionalWrite with request zero. Arena alignment is 256 bytes; allocation rounds and requires a contiguous free extent ([`host_context_arena.h:59–94`][e-arena-header], [`host_context_arena.cpp:52–102`][e-arena]). State reservations request `image_bytes()` (E3). `--host-context-mib` accepts decimal MiB resolving to an exact whole byte ([`serve_options.cpp:119–167`][e-mib]); Program uses configured Host bytes directly ([`program_impl.cpp:84–122`][e-host-capacity]). Do not round a calibrated test arena without rechecking its inequalities. |
| U6: header, stop, layout, registration | Lease header is E3. Windows flag is `--stop-event`, with `(Global\|Local)\\NInfer-Serve-Stop-<32 lowercase hex>` validated by [`serve_options.cpp:62–84`][e-stop-flag]. The server creates the event; the test opens it after readiness and signals it, rather than precreating it ([`stop_event.cpp:69–102`][e-stop-event]). Current is `sessions/<digest>/current`; data are `sessions/<digest>/generations/<generation>/engine/state/<id>.bin` ([`session_checkpoint_store.cpp:1994–1999,2070–2076`][e-layout], E3). Real tests are in [`tests/cmake/ProductTests.cmake:51–69`][e-product-tests]; the C++ service fault test is Linux-only, not a portable Windows stop test. |
| Universal drain claim | Corrected to new pressure work after drain, with the retained settled-pressure-entry exception (E4). The reported all-victims-declined scenario remains explained. |
| Active owners | The plan's general “active owners are never victims” was not established by OptionalWrite or the ordinary cursor. Add explicit active-owner exclusions (E5), and reject every other logical owner in Tier 2. |
| Refused export residency | “No partial demotion on false Host preparation” is true; “every refused export leaves residency unchanged” is false. Move deterministic checks early, but retain the narrower invariant (E3). |
| Durable acknowledgement | Engine `Exported` precedes current-pointer publication. Only the store/service receipt can acknowledge publication (E2/E6). |
| Smaller native interface | Replace the internal bool result and migrate callers; no compatibility output pointer, redundant diagnostic fields, or optional accessor fallback for Programs already implementing export. |
| Retry budget / order | Use owner-vector order, not an invented durable-age sort; derive `2+V+C` from consumed work, not inconsistent C+1/zero-victim examples. Keep import's existing bound unchanged. |
| Fake proof readiness | Existing `IndexReader::file_descriptor()` returns null ([`test_engine_core.cpp:1119–1144`][e-fake-reader]); it cannot prove a reclaimable victim unchanged. Add real fixture descriptors and mismatch controls. The fake also needs real Host-release quotes and plans, not only counters. |
| Test locations | Deterministic Host export tests already use the real store with CUDA interposition in [`tests/runtime/test_export_state.cpp:1–77,188–220`][e-state-test], not a guessed models/common test directory. Queue tests are `tests/test_automatic_checkpoint_queue.cpp`. |
| Real harness / fixture | The existing Python `Server` is nested in `main`, so it is a pattern, not an importable shared class ([`test_serve_restore_real.py:65–170`][e-real-harness]). A fixed 16-extra-slot setting and state-only arithmetic do not prove a constructible pressure scenario. Assert actual occupancy, Host KV, shared content, and both empty-arena fits; keep each restart pair's identity stable. |
| Shipping coverage | Shared source does not imply qwen4_exp durable support, nor does a container pass establish Windows stop-event behavior. Qualify each shipped component/route separately. |

## 6. Test plan (not executed by this design PR)

### 6.1 Deterministic Engine and ownership tests

Use the production `EngineCore` with the existing fake Program in
`tests/runtime/test_engine_core.cpp`, registered GPU-free with CUDA interposition
in [`tests/cmake/RuntimeTests.cmake:24–38`][e-runtime-tests]. Extend the existing
Host ledger ([`test_engine_core.cpp:685–706`][e-fake-host]), not a parallel simulation:
per-record resident bytes, real positive `host_bytes_released`, bounded
`plan_releases`, one-shot redundant replicas, scripted native refusal kinds,
and refusal leases that acquire no long-lived readers. Existing fake release
already honors pins; its release quote/plans currently return zero/empty
([`:743–790`][e-fake-release]). Make the fixture accept multiple sessions/tags;
`Harness::save` currently hard-codes one session ([`:1233–1251`][e-fake-harness]).
The proof reader needs valid index descriptors and configurable fresh-reader,
validation, and re-pin failures. These are test controls, not production fallbacks.

| Case | Required observation |
|---|---|
| Covered victim, stopped/Pending handler | Export B succeeds after proving and releasing A, without invoking the handler; exact attempts/bytes/owner receipt; final publication succeeds. This catches naive import-path reuse. |
| Missing oracle; missing/stale/wrong-tag generation; changed index; corrupt payload; read failure | Durable victim intact; no zero-proof release; unchanged refusal taxonomy. Tiers 1/3 may work only on eligible non-durable contents. |
| Native Busy, no arena, staging/absolute capacity, unsupported codec, allocation refusal without shortage | One prepare attempt, no oracle or destructive reclaim. |
| Full-set preservation | Every selected target point reaches the writer; old target off-chain cache can be dropped, shared target handles cannot. |
| Fork sharing | Second owner with a different tag/session, same tag, or unnamed active owner shares a handle: whole proven-victim release declines. Use the existing ResourceManager test fixture in `tests/runtime/test_resource_manager.cpp` for real carry/forget behavior. |
| Physical sharing / zero gain | Different handles share image/KV backing: skip zero-gain victims; quote and measured freed bytes agree, no double-counting. |
| Priority and protection | Redundant before durable before ordinary; active/waiting/binding/leased handles and paused snapshots survive. Ordinary cache keeps existing ranking. |
| Bounded exhaustion / fragmentation | Native keeps refusing; candidate cursors never refill; attempts at most `2+V+C`; zero-progress Changed cannot spin. |
| Reentrant handler | Inline export during an outer pressure release makes only its initial prepare; outer quoted plan remains valid, no recursive handler or catalog mutation. |
| Concurrent epoch / writer / cleanup | Submission during victim proof or writer makes publication stale when appropriate; writer still off-lock; refused and prepared leases destroyed on worker; writer failure retires exactly once. |
| Stats and scheduling | Reclaim followed by success, refusal or injected failure publishes catalog/occupancy; queued/resident work sees capacity after retirement. Existing `durable_import_commit`, `durable_import_abort`, `durable_export_release`, `resident_waits_for_export` remain valid. |

Extend `ninfer_export_state_test` for exact shortage, fragmented allocation,
Busy, no arena, partial reservation rollback, and failed-copy cleanup. Add native
export coverage proving staging/absolute refusals occur before demotion and later
codec/allocation failure is not falsely documented as a residency rollback.
Existing state test registration is [`RuntimeTests.cmake:66–83`][e-state-registration].

Update queue tests for both engine and storage progress waking Capacity, no retry
on unchanged progress or self-notification, unchanged budget, and exhausted
pressure remaining Pending. Include retained settled-pressure versus newly
requested pressure after drain. Add service/store tests that a save's export
callback can pin another generation without deadlock, and that reclaim receipts
survive later store/publish refusal. These tests do not replace native byte proof.

### 6.2 Real-GPU stop regression, red predecessor then green successor

Add `tests/test_serve_shutdown_capacity_real.py`, registered beside the existing
restore test with `gpu;real;durable`, skip code 77 for an absent explicit artifact,
a timeout and serial execution. Use stdlib Python. Reuse the existing harness's
provenance, authenticated HTTP, private scratch and sanitized receipt conventions;
factor only the portable process helper actually shared by both runners. Missing
artifact may skip; a configured fixture failing to create pressure must **fail**,
not pass without exercising reclaim.

1. **Calibrate a disposable root.** Use the exact selected production model/backend
   and roomy Host arena. Create disjoint A and B sessions and measure each complete
   exported set: unique state-file bytes, KV payload, and index bytes separately.
   Also measure the B continuation used by the restart check, so its new stored
   endpoint is known to fit the constrained arena rather than making the second
   stop an accidental absolute-capacity test.
   Select prompt length/extra Device slots/KV capacity so B's latest endpoint can
   remain Device-only without forcing unrelated admission eviction. Do not assume
   `--device-state-slots 16` fits every model/GPU; it is extra capacity, not total
   capacity (`serve_options.cpp:244–246`).
2. **Choose a byte-exact constrained arena H**, then use a fresh root. Both A and B
   native state+KV payloads must individually fit H, A's durable export must fit,
   and before stopping B's missing Host-state demand must exceed `H - occupied`.
   Releasing A must supply enough real Host space. Prefer distinct contents from
   the beginning of each prompt to avoid shared prefixes; verify the resulting
   physical occupancy rather than assuming independence from different cache keys.
   Require no unintended Host KV/demotion or extra cached session to invalidate the
   arithmetic. The original `states_A*S + floor(states_B*S/2)` is only a candidate
   H, not proof; receipt payload includes index bytes while arena fit does not.
3. **Construct the failure state.** Disable automatic saving for the selected
   workload with `--session-checkpoint-min-tokens 4294967295`. Complete and explicitly
   save A; complete B without explicitly saving it; status B must be missing. Record
   actual Host capacity/used/reserved bytes, Host state images, Host KV and Device
   state occupancy from existing metrics
   ([`src/serve/metrics.cpp:215–241`][e-metrics]). Verify the pressure inequalities
   immediately before stop. No background workload or retry should change them.
4. **Stop through the production route.** POSIX uses SIGTERM; Windows passes a fresh
   valid `--stop-event` name, then opens and signals the server-created event after
   readiness. The event ACL grants access to SYSTEM and administrators; execute
   that test in the authorized native qualification context, not by weakening its
   ACL. Always request/read `--shutdown-report`. In expected-defect mode on
   `0628db0b`, require `saved=0, skipped=1, refused=1, complete=false`, the B save
   refusal `reason=capacity`, and nonzero exit for an otherwise normal stop. In the
   corrected build require `saved=1, skipped=1, refused=0, complete=true`, exit zero,
   and the B exported receipt with positive reclaimed Host bytes and at least one
   released owner. This distinguishes actual reclaim from favorable timing.
   The two-session totals differ from the incident's ten-session totals by design.
   `HttpServer::listen` propagates failed final-save to process failure
   ([`http_server.cpp:681–705`][e-http-stop], [`apps/serve/main.cpp:103–111`][e-main-stop]).
5. **Restore the result.** Restart with the same semantic fingerprint, constrained
   capacity, artifact and backend. B's first keyed stored continuation must restore
   the saved frontier within the fixture's declared page/frontier tolerance and
   match a deterministic reference continuation. Require A's current-pointer digest
   unchanged and its generation still loadable. Keep disk quota generous enough
   that unrelated disk eviction cannot defeat this assertion. If the continuation
   creates another stored turn, assert its own subsequent stop result separately;
   do not silently turn off the shutdown check for that second stop.

The baseline red is currently established by the reported incident and the source
path above, **not** by an execution of this not-yet-written fixture. Implementation
acceptance must bind an expected-defect receipt to `0628db0b` and a green receipt
to the exact candidate binary. Run both under the lead's authorized GPU window;
this document claims no host run. Also retain the long-session managed-Stop release
rehearsal: the small-arena regression is a deterministic trigger, not a substitute
for the observed workload.

## 7. Shipping components and implementation order

The implementation affects the shared context store, qwen3_5 native checkpoint
preparation/lease, EngineCore checkpoint orchestration and ResourceManager, and
serve's receipt/automatic-save code. No storage backend or controller algorithm
change is intended. A source/header fix requires rebuilding the runtime, not a
client-only release.

- Native Windows lane specifications exist for [RTX 3090][ship-3090],
  [RTX 4090][ship-4090], and [RTX 5090][ship-5090]. Rebuild each component that is
  shipped from these sources and qualify the applicable named-event Stop, report,
  and restore route. A pass on one native architecture does not qualify another.
- The RTX 5090 Linux container builds and copies `ninfer-serve` from these same
  sources; its runtime image does **not** build tests (`BUILD_TESTING=OFF`), and its
  stop signal is SIGTERM ([`Dockerfile:34–77`][e-docker]). Run the Python production
  regression against the built image in the external qualification harness; merely
  adding a CTest registration does not execute it in the runtime-image build.
- Do not advertise qwen4_exp durability as part of this fix. Preserve the existing
  compile-time unsupported path while requiring the new shortage accessor of each
  Program that actually implements durable export, including the fake Program.

Implementation sequence (one coherent functional change; no partially enabled
reclaim feature shipped):

1. Extend the deterministic fixtures and define the internal shortage result;
   update all Host-preparation callers and refusal classification tests.
2. Add native preflight ordering/lease shortage, proof reuse if useful, strict
   ResourceManager proven release and the explicit protection collector. Keep
   import behavior unchanged and cover its existing regressions.
3. Integrate the bounded `checkpoint_change` loop, pressure-depth suppression,
   worker cleanup and notifications together. Add the Engine-level failure matrix.
4. Carry reclaim summary through Engine/service, update existing receipts and
   queue predicate/tests together; update K3/K7 and the test guide in the
   implementation PR rather than making this proposal a second contract authority.
5. Complete the portable GPU stop runner and registrations; obtain predecessor
   red and exact-candidate green evidence, then native lane and container release
   qualification under the existing managed windows. No host actions are part of
   this design PR.

## 8. Rejected alternatives, risks, and reviewer decisions

Rejected:

- **Blind Capacity retry or retry only in final_save:** no physical progress on an
  idle engine, and repeated deterministic/limit refusals waste work.
- **Reuse guarded durable release unchanged:** it cannot obtain a new pressure
  answer after drain; live use also risks recursive save/reclaim and Pending churn.
- **Disable the pressure guard globally:** bypasses other owners' durability and
  admission policy; a byte proof for one owner is not a blanket authorization.
- **Prove all sharers with a new multi-owner transaction:** unnecessary for the
  observed defect. Decline shared logical handles; keep native physical sharing.
- **Trim the exported chain / save responses-only as automatic fallback:** changes
  the promised native continuation instead of recovering available capacity.
- **Keep Device state pinned for the long disk write:** reverses the current Host
  export design and can starve scarce execution slots.
- **Only reclaim target off-chain points or only increase Host capacity:** misses
  reclaimable covered sessions and hides the lifetime/capacity boundary.
- **Change import's policy or fix qwen4_exp durability here:** separate behavior;
  not required to close this export failure.

Risks and proposed conservative decisions for founder/reviewer confirmation:

1. **Live worker latency:** a fixed candidate count still permits expensive byte
   reads/hashes. Cheap exclusion/Host-gain checks run first and stop once the
   shortage is covered. Measure proof latency in qualification; do not add an
   asynchronous ownership protocol or silently skip byte verification.
2. **Cache-performance tradeoff:** durability may evict ordinary unprotected cache
   even on a live save. Proposed priority is lossless replicas, proven durable
   owners, then ordinary cache; active and waiting requests are protected. Confirm
   this policy, rather than weakening it implicitly in implementation.
3. **Conservative sharing:** strict single-owner logical handles can leave Capacity
   even when a more complex multi-owner proof could reclaim enough. Accept this
   safe refusal for #78; the invariant is never to erase a different owner's
   point on one proof, not to guarantee every theoretically feasible export fits.
4. **Fragmentation and insufficient capacity:** positive freed bytes do not prove
   contiguity; bounded attempts can still refuse. Preserve the refusal and receipt,
   not a compactor or unbounded retry. Use current arena geometry in tests.
5. **Residual refusal after demotion/reclaim:** a later writer, quota, corrupt-index
   or epoch failure can occur after safe reclaim. Victim durability stays proven;
   target content stays valid; current pointer publication remains authoritative.
   Test this rather than claiming rollback of all caching side effects.
6. **Qualification fixture availability:** the exact prompt/Device-slot shape that
   satisfies both empty-arena fits and pre-stop shortage is a measured prerequisite
   per selected artifact/backend/GPU. It is intentionally not asserted by this
   source-only design. No fake green if the pressure setup is not constructible.

All six preparation unknowns are resolved from source. Open review decisions are
the live-save cache tradeoff, acceptance of the conservative shared-owner refusal,
and the measured fixture/latency envelope for the shipped components; none requires
changing a durability or publication invariant.

## Source references

[e-final]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_service.cpp#L482-L506
[e-save]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_service.cpp#L219-L259
[e-store-export]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_store.cpp#L2031-L2146
[e-save-lease]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_store.cpp#L1084-L1138
[e-oracle]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_service.cpp#L384-L393
[e-load]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_store.cpp#L2324-L2355
[e-fresh]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_store.cpp#L1608-L1622
[e-store-publish]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_store.cpp#L2228-L2275
[e-export]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/engine_checkpoint.inl#L72-L160
[e-call]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/engine_checkpoint.inl#L1-L15
[e-change]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/engine_checkpoint.inl#L366-L390
[e-gate]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/checkpoint_control.h#L17-L105
[e-writer-contract]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/docs/durable/CONTRACTS.md#L434-L448
[e-native-export]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/models/qwen3_5/program/storage/checkpoint_api.cpp#L351-L543
[e-host-export]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/models/common/context/state_store.cpp#L55-L92
[e-host-declaration]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/models/common/context/state_store.h#L140-L145
[e-reserve]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/models/common/context/state_store.cpp#L475-L497
[e-lease]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/models/qwen3_5/program/program.h#L407-L425
[e-native-write]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/models/qwen3_5/program/storage/checkpoint_api.cpp#L293-L338
[e-import]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/engine_checkpoint.inl#L190-L291
[e-validate]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/models/qwen3_5/program/storage/checkpoint_api.cpp#L574-L602
[e-durable-victims]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/context_cache/resource_manager.h#L168-L194
[e-guard-install]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/engine_core.h#L109-L112
[e-pressure]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/engine_checkpoint.inl#L423-L429
[e-settle]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/checkpoint_control.h#L147-L207
[e-queue-stop]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/automatic_checkpoint_queue.cpp#L19-L177
[e-queue-run]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/automatic_checkpoint_queue.cpp#L206-L237
[e-handler]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/contract/continuation_checkpoint.h#L582-L598
[e-commit-release]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/context_cache/resource_manager.h#L1534-L1554
[e-session-points]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/context_cache/resource_manager.h#L123-L177
[e-prepare-source]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/context_cache/resource_manager.h#L319-L380
[e-forget]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/context_cache/resource_manager.h#L1137-L1146
[e-allow]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/context_cache/resource_manager.h#L1557-L1572
[e-rights]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/context_cache/types.h#L14-L21
[e-revoke]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/context_cache/resource_manager.h#L959-L972
[e-reclaim]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/context_cache/resource_manager.h#L661-L750
[e-host-victims]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/context_cache/resource_manager.h#L1575-L1663
[e-victim-order]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/context_cache/resource_manager.h#L1229-L1281
[e-redundant]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/models/qwen3_5/program/storage/checkpoints.cpp#L276-L339
[e-physical-release]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/models/qwen3_5/program/transactions/reclaim.cpp#L304-L355
[e-service-progress]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_service.cpp#L126-L449
[e-notify]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/engine_checkpoint.inl#L30-L36
[e-receipts]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_service.cpp#L89-L151
[e-k7]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/docs/durable/CONTRACTS.md#L876-L893
[e-result]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/contract/durable_engine_api.h#L129-L134
[e-queue-test]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/test_automatic_checkpoint_queue.cpp#L159-L196
[e-qwen4]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/models/qwen4_exp/program/program.h#L251-L334
[e-arena-header]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/core/host_context_arena.h#L59-L94
[e-arena]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/core/host_context_arena.cpp#L52-L102
[e-mib]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/serve_options.cpp#L119-L167
[e-host-capacity]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/models/qwen3_5/program/program_impl.cpp#L84-L122
[e-stop-flag]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/serve_options.cpp#L62-L84
[e-stop-event]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/stop_event.cpp#L69-L102
[e-layout]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_store.cpp#L1994-L2076
[e-product-tests]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/cmake/ProductTests.cmake#L51-L69
[e-fake-reader]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/runtime/test_engine_core.cpp#L1119-L1144
[e-state-test]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/runtime/test_export_state.cpp#L1-L220
[e-real-harness]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/test_serve_restore_real.py#L65-L170
[e-runtime-tests]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/cmake/RuntimeTests.cmake#L24-L38
[e-fake-host]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/runtime/test_engine_core.cpp#L685-L706
[e-fake-release]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/runtime/test_engine_core.cpp#L743-L790
[e-fake-harness]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/runtime/test_engine_core.cpp#L1233-L1251
[e-state-registration]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/tests/cmake/RuntimeTests.cmake#L66-L83
[e-metrics]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/metrics.cpp#L215-L241
[e-http-stop]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/http_server.cpp#L681-L705
[e-main-stop]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/apps/serve/main.cpp#L103-L111
[ship-3090]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/lanes/rtx3090/release-spec.json#L1-L13
[ship-4090]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/lanes/rtx4090/release-spec.json#L1-L13
[ship-5090]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/packaging/windows/lanes/rtx5090/release-spec.json#L1-L13
[e-docker]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/Dockerfile#L34-L77
