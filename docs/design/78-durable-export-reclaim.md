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

**Decision:** reclaim on a specifically identified, contended Host byte deficit.
Final saves and explicit operator saves (`POST` checkpoint) may use all three
tiers, including byte-proven durable-victim release closures. Automatic saves use
only lossless Tier 1 and protected ordinary-cache Tier 3; they never byte-prove
victims on the worker. Contiguity-only failures permit lossless cleanup, not
destructive reclaim. Return victim proof pins to the service and retain them
through the reclaiming store save. Do not add a stopping parameter or substitute
a service-only retry for Engine reclaim. After its first pass, `final_save` makes
one additional pass over Capacity-refused sessions only if at least one first-pass
save published; there are no further passes. Preserve the complete selected
checkpoint set, publication gate, byte proof, and fail-closed final-save result.
The fix must work after the automatic queue has stopped accepting work.

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
- Quota inventory protects generations with readers in `active_generations`, not
  native residency. Other sessions' unpinned current generations can be
  `CurrentSession` victims ([`session_checkpoint_store.cpp:1247–1300`][e-quota-inventory]).
  `save_impl` enforces quota after export, before publication and again after
  publication ([`:2225–2273`][e-quota-save]). Ending proof pins with the Engine
  command would leave a just-reclaimed victim exposed to that same save.
- `erase_response` has a separate survivor-export callback and save receipt,
  outside `save_session`/`SaveOutcome`
  ([`session_checkpoint_service.cpp:336–358`][e-erase-export]).

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
- `host_bytes_released` counts an image only when all its checkpoint references
  occur in the released set, and quotes KV through `released_pages` over that set
  ([`qwen3_5/program/storage/checkpoints.cpp:187–219`][e-host-quote]).
  A durable owner and ordinary cache can therefore pin the same physical bytes
  while neither set alone has a positive quote. Tier 2 needs their eligible release
  closure, not a victim-only quote followed by a disjoint ordinary tier.
- `host_victims_in_order` requires a plan that covers the entire requested byte
  shortage or returns no plan ([`resource_manager.h:1575–1663`][e-host-victims]).
  Passing the original shortage after earlier progress can miss a sufficient plan.

### E6. Automatic retry and receipts

[`automatic_checkpoint_queue.cpp:186–237`][e-queue-run] samples progress before
saving and makes Capacity retryable, but only an observed **engine** progress
change wakes that deferred save. I/O refusals use storage progress. The service
separately counts successful storage operations
([`session_checkpoint_service.cpp:126–134,273–276,419–449`][e-service-progress]).
`request_admission_check` calls `notify_progress()` with its default
`observed=true` ([`engine_core.h:591–596`][e-admission],
[`checkpoint_control.h:130–138`][e-observed]). Reusing it after reclaim would let a
refused automatic save advance its own retry clock.
`notify_checkpoint_progress` instead signals admission and completion without
advancing observed engine progress ([`engine_checkpoint.inl:30–36`][e-notify]).
Prepared export-lease retirement is different: it releases real native pins and
must use observed admission progress so another session's deferred TransactionBusy
or Capacity save can retry without unrelated engine activity. A refused prepare
holds no lease and has no retirement event. Keep the engine-only Capacity
predicate; automatic saves will not use Tier 2.

Receipt keys are produced by [`session_checkpoint_service.cpp:89–96,139–151`][e-receipts].
K7 explicitly permits additive bounded counts/durations
([`docs/durable/CONTRACTS.md:876–893`][e-k7]); it does not permit raw identities or
weakening `committed=true`. No new reason or shutdown-report schema is needed.

## 3. Proposed minimal mechanism

### 3.1 A typed native shortage, not another public refusal reason

Replace the internal boolean Host-preparation result with a small value containing
`Kind {Prepared, Busy, NoHostArena, HostCapacity, HostContiguity}` and
`shortage_bytes` (positive only for a Host byte deficit). Migrate the native caller
and existing tests; do not keep a defaulted output-pointer compatibility overload.
The low-level classification need not duplicate missing-image count, required
bytes, free bytes, and a separately parameterized deficit method in a long-lived
object.

After eligibility checks, calculate missing-image demand with checked arithmetic
and sample free arena bytes **before reservations**. Allocate each missing image's
Host allocation explicitly, then pass it to the existing reservation overload
(E3). Classify an allocation miss separately from a failed eligibility/reservation.
Report HostCapacity with `required-free` when positive; when free bytes suffice
but no contiguous allocation fits, classify the miss as HostContiguity with zero byte deficit.
Do not invent a destructive byte target for fragmentation. Roll back all temporary
reservations before returning; preserve the all-or-nothing false/refused
Host-preparation invariant.

Expose `host_shortage_kind()` and `host_shortage_bytes()` on the qwen3_5 export
lease. Preserve both through the refusal catch that currently replaces `Impl`.
Engine enables destructive reclaim only for `CapacityRefused`, reason `Capacity`,
HostCapacity and a positive shortage. HostContiguity permits only Tier 1 and at
most one further prepare after positive measured arena progress; otherwise retain
Capacity. Other Capacity sites and all other states remain non-reclaiming. Keep
existing public state/reason mappings; these are internal distinctions, not new
K7 reasons. The predecessor red fixture must record the actual refusal site and
this classification before any green claim (§6.2/§8).

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
Thread save intent from the service: only final saves and explicit operator
checkpoint saves enable Tier 2. Automatic saves, including pressure-triggered
saves, and `erase_response` survivor exports do not enable it. This is a per-export
policy, not a stopping parameter or permission to bypass pressure re-entry.

For each native prepare attempt:

1. Check Engine availability. Hold the target gate only to capture its epoch,
   resolve `session_points`, check the tag, and create the lease. Keep the same
   gate object for the whole command. Empty/pending/tag-mismatch behavior is unchanged.
2. On Prepared, increment the in-flight export count once and leave the loop.
3. Otherwise destroy the refused lease on the worker before reclaim; retain its
   reason/shortage classification as plain values. Stop for non-Host-capacity
   refusals or pressure re-entry. For HostContiguity, use only the command's
   single Tier 1 allowance; if it makes positive measured arena progress, prepare
   once more, then return its result without entering either destructive tier.
   Zero progress or an already-consumed Tier 1 retains the refusal without
   another prepare.
4. At the first Host byte deficit, freeze durable candidates for Tier-2-enabled
   saves and ordinary-cache cursor membership. Allocate the bounded pin vector,
   closure and exclusion scratch before destructive work. Do not refresh either
   candidate set during this command.
5. Consume the following tiers in order until enough measured Host arena bytes
   have been freed for the shortage, or candidates are exhausted. Retry only after
   strictly positive measured arena progress since the preceding prepare. A
   zero-progress step consumes its candidate but does not end the command; continue
   with the remaining fixed candidates. Prefer accumulating sufficient bytes before
   preparing again rather than rebuilding the export after each small release.

| Tier | Action | Protection / authority |
|---|---|---|
| 1 | One pass of `release_redundant_host` | Lossless replica removal. Exclude export handles, active-owner points, and held waiting/binding sources; native lease protection still applies. |
| 2 | Initial `durable_victims(target)` in existing order; final/explicit checkpoint saves only | Plan each victim's release closure; skip zero closure quotes or ineligible closures before disk I/O. Fresh native byte proof and the exclusive, guard-free whole-plan release below. Never save a victim recursively. |
| 3 | Existing Host-only `ResourceManager::reclaim`, with one fixed cursor and OptionalWrite rights | Request only the remaining measured deficit. Exclude export handles, **every durable-named handle**, active-owner points, and held waiting/binding sources. Offer no paused snapshots. Reuse native release quotes/ranking; no new eviction score. |

Tier 3 includes obsolete, unnamed target-session cache as well as other ordinary
cache. There is no special “trim the target's chain” tier: the newest owner's
entire valid point set is the export set; superseded owners are ordinary cache
(E5). Even if an old owner is unnamed, a handle shared with the exported owner
remains excluded. Add a narrowly named ResourceManager exclusion collector rather
than deriving ownership in Engine or assuming the existing cursor protects active
owners. Collect/deduplicate by logical handle; native helpers protect the reachable
physical contents.

Tier 2's closure combines the victim's entire handle set with eligible ordinary
handles from that same frozen cursor which pin the same physical images/pages,
including superseded unnamed owners and shared prefix entries. Ordinary closure
members must pass all Tier 3 eligibility checks; the named victim must pass the
single-owner proof in §3.3. Expand a physical release group only when every
not-yet-included holder is an eligible ordinary member. Skip a group containing
a protected holder rather than declining the victim: that holder keeps the
shared pages resident, while the victim's private backing may still be freed.
Use native sharing information and existing catalog
membership, not logical-size sums or a second ownership catalog. Quote the union
once and commit it as one plan; neither the victim-only nor ordinary-only quote
is the release authority. Removed ordinary members are also consumed in the
cursor, not reconsidered in Tier 3.

After each release batch, Tier 3 requests
`max(0, shortage_bytes - measured_arena_bytes_freed_since_prepare)`, not the
original shortage. E5's all-or-nothing planner must see the remaining demand.
Native Host KV page quotes need not equal arena progress: pages live in extents,
so releasing pages can leave their arena allocation occupied. Only the measured
arena delta authorizes another prepare, not a positive quote or catalog mutation.

Tier 3's exclusion of all durable names makes `allow_release`'s victim list empty.
The installed guard then returns immediately without unlocking execution or
consulting Pending (E4). Tier 2 does not call the guard at all. No oracle means
Tier 2 is unavailable, not that unproved durable victims become ordinary cache.
Tiers 1 and 3 may still make progress within their non-destructive/durability-safe
scope. Automatic saves never consult the oracle or perform victim byte proof.

### 3.3 Guard-free proven release and its exact exclusivity proof

Add a narrowly scoped ResourceManager operation for a **proven durable victim**,
not a general `ignore_guard` flag. Keep import's existing guarded release behavior.
The operation plans the release closure above and rechecks, immediately before
committing that whole plan:

- The original owner token still exists, is inactive, has the same nonempty
  `(session, tag)`, and has a nonempty point set.
- Every released handle is valid, unleased and revocable under OptionalWrite
  (no held waiting/binding source). Any intersection between the closure and the
  protected export set declines the **whole victim**, never a partial named owner.
- **No other owner references any of the named victim's handles**, including
  unnamed, same-tag and active owners. Decline the whole victim on any such
  logical-owner intersection. Different handles that share physical images/pages
  are not this logical-owner violation: their eligible ordinary references belong
  in the closure. Do not include another durable-named or active owner's handles.
- Every ordinary closure member passes Tier 3's exclusions and OptionalWrite
  eligibility. It may be referenced by multiple eligible unnamed inactive owners
  or shared entries only when forgetting it violates none of those protections.
- The complete closure has a strictly positive native `host_bytes_released` quote.
  Allocate all required scratch before commit; use existing native release/forget
  primitives for a single plan, not separate victim/ordinary commits or a second
  ownership catalog. Recheck the cheap predicates and whole-set quote after disk
  proof; decline the victim if any closure member becomes ineligible.

The Engine supplies the authority immediately before that operation: pin the
victim's current generation; obtain a fresh reader; decode its index; validate
its native identity, metadata and all payload bytes; re-pin `(session, tag)`;
compare index byte length and SHA-256; retain both pins until the prepare/reclaim
command hands them back in `export_session`'s result. The service moves these
reader handles into a local lifetime holder **outside** the export callback and
keeps them until the enclosing `store_.save` returns or unwinds, including staging
quota, pointer publication and post-publish quota. Preserve the pins on refused
results too; native lease retirement must not retire them early. This is E4's
proof with a longer export-save lifetime, not `covers()` or a bare `Settled`
result. It may be factored once with import's proof, provided import's subsequent
guard, rights, attempt bound, reader refresh and failure behavior remain unchanged.

The reclaiming save consequently cannot quota-evict a victim generation it just
released from Host. With tight disk quota it may instead refuse with `Quota`,
leaving the victim cold but durable. This is not permanent quota immunity: after
the service releases the pins, later saves follow ordinary disk-quota policy and
may retire that victim's only native copy, just as for other non-resident sessions.
Transcript replay remains available.

**Why the bypass is needed:** the guard is a request to arrange a save, not a
primitive for accepting already established recoverability. That request remains
Pending after queue drain. The new path establishes the stronger byte proof
itself and proves that forgetting the handles can affect only that one durable
owner. The rest of the closure is independently eligible non-durable cache. Therefore
there is no other durable owner for which `allow_release` would have needed a
separate answer. Refuse shared logical-owner cases rather than building a
multi-session proof/locking transaction. Any target/export-set intersection
declines the whole victim before proof.

This is a caller/callee invariant under worker serialization, not authority from
a public boolean, a manifest field, or a caller-supplied session name. Tests must
exercise the real Engine entry point and attempt cross-owner sharing violations.

### 3.4 Bounded retry, re-entry, and failure handling

Let `V` be initial durable candidates (`V=0` when Tier 2 is disabled) and `C` the
distinct handles in the frozen ordinary cursor. Consume each durable candidate
at most once, perform redundant cleanup once, and permit at most `C` ordinary
destructive steps. Each ordinary step deletes at least one still-present cursor
handle; zero measured arena progress still consumes that work and continues to
the next candidate without preparing again. A Tier 2 closure is one consumed
durable candidate, however many eligible ordinary handles it removes; those
handles cannot be reused later. After Tier 1, no successful action in this command
creates Host replicas: failed contended prepares roll reservations back, and
release steps only remove content. Therefore redundant cleanup inside the reused
Host reclaim path cannot create another unbounded source of progress.

The conservative upper bound is **`prepare_attempts <= 2 + V + C`**: one initial
attempt, one after redundant cleanup, and at most one per consumed durable or
ordinary step. Usually steps are accumulated and the actual count is smaller.
Closure expansion adds no independent retry allowance. A contiguity-only command
uses at most two prepares, the second only after positive Tier 1 arena progress,
and never enters Tiers 2/3. Do not stop merely because one candidate freed zero
arena bytes. Stop on exhausted membership, a noncontended refusal, stale/tag change,
Engine failure, or pressure re-entry. Do not sleep, refresh membership, poll the
queue, or retry based only on catalog changes.
If the remaining known deficit cannot be met, retain Capacity; do not erase
protected content to force success. This is a count bound, not a disk-latency bound.

`final_save` freezes its first-pass session list and retains only sessions refused
with reason Capacity for a possible second pass. Run that pass **only if at least
one first-pass save published**: a newly covered session may now be a provable
victim for an earlier refusal. Retry each retained session once, never run a third
pass, and do not retry other refusals. Each command freezes its own V/C as above.
For `S` first-pass sessions and `R <= S` eligible refusals, there are at most
`S+R <= 2*S` save calls, with total native attempts bounded by the sum of
`2+V_i+C_i` over the calls that prepare. Count each session's final outcome once
in saved/skipped/refused; a successful second attempt replaces its first refusal.
This publication-conditioned extra pass complements Engine reclaim; it is not the
rejected unbounded or service-only retry. Remaining refusals still fail Stop closed.

Add a worker-only scoped pressure-callback depth marker around the nonempty
`checkpoint_pressure` callback boundary. An export invoked within that boundary
may prepare/write normally but may **not reclaim**. This preserves the current
outer plan while still allowing a handler to save. Empty-victim guard calls do
not enter this boundary. Keep lock/depth restoration exception-safe.

Optional reclaim allocation failure stops with the last Capacity refusal. Do not
catch an invariant failure and report success. Preallocate bounded bookkeeping
before destructive work; retain proof pins through result handoff; destroy native
leases on the worker on every exit. After handoff, the service retains proof pins
through the full store save as required in §3.3. Any actual catalog/occupancy
mutation must wake admission even if a
subsequent step refuses or throws, but must **not** advance observed checkpoint
progress. Paused pressure owners now retry restoration each cycle; there is no separate
scheduler capacity-event latch. Use the save-completion path's non-observed notification (E6), or an
equivalent admission signal; do not call the observed `request_admission_check`
for save-owned reclaim/cleanup notifications. No-op tiers emit no capacity or
admission notification: count a change only after redundant cleanup releases
content, ordinary reclaim returns Changed, or proven-victim release succeeds.
A refused automatic save must not
wake its own retry. `checkpoint_change` publishes the resulting stats. Preserve
the prepared export's separate, exactly-once in-flight decrement and capacity
notification on retirement, using observed `request_admission_check` and a queue
wakeup after the native pins retire. This can unblock another session waiting on
the export/save lease; refused prepares have no lease to retire and cannot
self-wake through this path. Service completion wakeups remain non-observed.

### 3.5 Lock discipline and concurrency

| Boundary | Held | Must not be held / awaited |
|---|---|---|
| Select points, epoch, tag, native prepare | Worker execution lock and target gate | No victim proof, store publication, or queue callback |
| Reclaim and victim byte proof (Tier-2-enabled saves only) | Worker execution lock; generation readers | No target/victim publication gate; no save callback; no worker wait from inside a store lock |
| Store load / fresh-reader / pin retirement | Store mutex briefly, inside the proof | No service operation mutex acquired by the oracle; no Engine command awaited |
| Lease writer and buffered finish | Immutable copy leases only | No Engine execution lock or publication gate |
| Store quota, publication and post-publish quota | Service-owned victim readers remain live through `store_.save`; store lock as in E1 | No early pin retirement at Engine command/writer completion; no worker command while holding the catalog lock |
| Final pointer publication | Target gate, then store mutex in callback | No Program access or worker command wait |
| Native lease retirement | Worker via `checkpoint_change`; observed admission progress after pins retire | No outstanding writer/copy using the lease |

Incoming submissions can advance epochs while proof is running, but cannot run
native work concurrently with this worker command. Recapture epoch/points/tag
under the gate for each attempt; the writer still undergoes the final gate check.
Do not keep the target gate across multi-file victim I/O. Do not temporarily drop
execution to reduce that I/O latency: the fixed ownership/exclusivity argument
would then require a different revalidation protocol. Other in-flight writers
remain safe through their native leases. Waiting sources, active owners and
paused snapshots are not sacrificed to make this optional write fit.
Automatic saves never perform this victim I/O; proof latency for final and explicit
checkpoint saves is a qualification observation, not a bounded live-save guarantee.

### 3.6 Invariants

| Invariant | Enforcement / regression |
|---|---|
| Full acknowledged set, no chain truncation | `session_points` and native codec unchanged; target handles excluded in all tiers. |
| False/refused Host preparation is all-or-nothing | Reserve whole set before copies; distinguish this from later export failure. |
| Same requested tag, current publication epoch | Per-attempt capture under the retained gate; mandatory final compare-and-publish unchanged. |
| No fabricated durable acknowledgement | Engine write/finish is staging; store flush, quota, rename, sync and gate still decide `committed`. |
| No unauthorized loss of another owner's durable endpoint | Fresh native byte proof, named-owner exclusivity, eligible ordinary closure, whole-plan commit; any export-set intersection declines the whole victim. |
| No unproved durable release | All named handles protected in Tier 3; absent/stale/corrupt proof declines Tier 2. |
| Reclaiming save cannot quota-evict its victim | Export result transfers proof readers to service ownership through `store_.save`; later saves retain ordinary quota policy. |
| No active/waiting/binding/paused victim | Explicit owner/source exclusions, OptionalWrite rights, no paused snapshots. |
| No release of in-flight export contents | Checkpoint lease predicate and native replica/reader checks. |
| No callback-induced re-entry | Guard-free Tier 2, empty-victim Tier 3, scoped pressure-depth suppression. |
| Physical progress, not guessed bytes | Native Host quote before release, arena occupancy delta afterwards. |
| No destructive fragmentation treatment | HostContiguity runs Tier 1 only; at most one extra prepare after measured progress. |
| No automatic worker-held victim proof | Tier 2 enabled only for final and explicit operator checkpoint saves. |
| No automatic self-wake | Refused-save mutation/cleanup is non-observed and no-op tiers do not notify; refused prepares hold no export lease. Prepared export retirement is observed and can wake another session's retry; Capacity predicate stays engine-only. |
| Bounded work | Fixed V/C, consumed candidates including zero-progress steps, `2+V+C` per-command ceiling; at most one publication-conditioned second final-save pass. |
| Worker affinity and truthful stats | Preparation/destruction under `checkpoint_change`; actual reclaim changes notify non-observed admission, prepared export retirement notifies observed admission exactly once. |
| Stable refusal and disk contracts | Same K7 states/reasons, unchanged checkpoint schema, no new controller policy. |
| Retry exhaustion never authorizes destruction | Existing Pending/retry-budget rules remain intact. |

## 4. Receipts and automatic queue discipline

Add a small trailing default-initialized reclaim summary to `DurableExportResult`
(current declaration: [`durable_engine_api.h:129–134`][e-result]). Count native
prepare attempts, released durable **owners**, declined durable candidates,
ordinary destructive steps, and actual Host arena bytes freed. Do not call owner
count “sessions”: candidate membership is per owner and need not be unique by
session. Sum the per-release deltas before re-materialization; this is not the
net occupancy reduction after a successful export. A committed Tier 2 closure
counts as one reclaimed owner; its ordinary members are not separate Tier 3 steps.
Also return its proof-reader handles in the export result; these are transient
lifetime state, not receipt fields or a persistent schema.

Add bounded integer receipt keys when reclaim was considered:
`prepare_attempts`, `reclaimed_owners`, `reclaim_declined`,
`reclaimed_cache_steps`, `reclaimed_host_bytes`. Include exhausted/no-safe-victim
results even when attempts remains one. A positive byte count can come from
redundant replicas with zero released owners. Do not infer successful publication
from any reclaim field.

Preserve the summary, proof pins **and** captured epoch across
`result = lease->write(writer)`. Capture the summary in service `SaveOutcome` from
the export callback even if store quota, I/O or publication later refuses; move
the pins into a service local that outlives `store_.save`, not a callback-local
temporary. `erase_response` has its own `store_.save`/`Access::export_session`
path outside `SaveOutcome` (E1): capture its summary separately and use the same
receipt-field helper for its survivor-save receipt too. Preserve a native
attempt's summary across the existing responses-only survivor fallback without
changing that fallback's semantics. Explicit checkpoint, automatic, final and
survivor-export receipt paths must all retain the applicable keys. Keep
`dir_sync`, origin and committed semantics, and the shutdown report's
saved/skipped/refused/complete fields unchanged. Reuse published occupancy
metrics; no new persistent stats schema is necessary.

Withdraw the proposed storage-progress Capacity predicate change and its test
update. Keep `now.engine != before.engine` for deferred Capacity, the existing
bounded retry counter, publication waits, I/O predicate and Pending semantics.
Retain the existing test that storage progress alone cannot cure Capacity
([`test_automatic_checkpoint_queue.cpp:159–196`][e-queue-test]). Automatic saves
cannot use newly proven durable victims because they do not use Tier 2. Actual
reclaim mutations and service completion notify admission without advancing the
observed engine clock (§3.4), including after a partially reclaiming refusal.
Prepared export retirement advances the observed clock so another session's
TransactionBusy or Capacity retry can run; no-op reclaim tiers do not notify.
The sole additional final-save pass is publication-conditioned and bounded as in
§3.4, not a new automatic retry predicate.

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
| Active owners / sharing | OptionalWrite and the ordinary cursor alone do not protect all active owners. Add explicit exclusions (E5); the durable victim's handles remain single-owner exclusive. Eligible inactive unnamed/shared references behind different handles may join its physical release closure. |
| Refused export residency | “No partial demotion on false Host preparation” is true; “every refused export leaves residency unchanged” is false. Move deterministic checks early, but retain the narrower invariant (E3). |
| Durable acknowledgement | Engine `Exported` precedes current-pointer publication. Only the store/service receipt can acknowledge publication (E2/E6). |
| Smaller native interface / fragmentation | Replace the internal bool and migrate callers; preserve typed byte-deficit versus contiguity classification through the lease. No invented fragmentation byte target, compatibility output pointer or optional accessor fallback. |
| Retry budget / order | Use owner-vector order; derive `2+V+C` from consumed work, including closure removal and zero-arena-progress candidates. Final-save orchestration adds at most one Capacity-only pass after actual first-pass publication; import's bound stays unchanged. |
| Proof pin lifetime / disk quota | Command-scoped pins are too short: E1's quota passes can retire another session's current generation. Return readers to service scope through the reclaiming save; permit Quota refusal, and preserve ordinary quota eviction by later saves after pin release. |
| Automatic progress / save intent | Reclaim mutation and service completion use non-observed notifications; prepared export retirement remains observed to wake other sessions. Withdraw storage-triggered Capacity retries. Only final and explicit checkpoint saves use Tier 2; automatic and survivor exports use Tiers 1/3. |
| Physical release closure / residual deficit | Victim-only and ordinary-only quotes can both be zero for shared backing (E5). Quote/commit one eligible closure; any export-set intersection declines the victim. Tier 3 requests only shortage minus measured bytes already freed. |
| Survivor-export receipts | `erase_response` bypasses `save_session`/`SaveOutcome` (E1). Carry its native reclaim summary and share the receipt helper, including its existing replay fallback. |
| Incident refusal site | The reported Capacity reason does not prove which native refusal fired. Keep this open until the predecessor red fixture records the site through the new internal classification; no green claim before that evidence. |
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
| Tight disk quota after Tier 2 | The reclaiming save retains victim readers through staging quota, publication and post-publish quota. It either refuses with Quota leaving the victim cold/loadable, or succeeds without evicting the victim generation. A later independent save may evict it under ordinary policy after pins are released. |
| Missing oracle; missing/stale/wrong-tag generation; changed index; corrupt payload; read failure | Durable victim intact; no zero-proof release; unchanged refusal taxonomy. Tiers 1/3 may work only on eligible non-durable contents. |
| Native Busy, no arena, staging/absolute capacity, unsupported codec, non-Host allocation refusal without a classified shortage | One prepare attempt, no oracle or destructive reclaim. |
| Full-set preservation | Every selected target point reaches the writer; old target off-chain cache can be dropped, shared target handles cannot. |
| Fork sharing | Second owner with a different tag/session, same tag, or unnamed active owner shares a handle: whole proven-victim release declines. Use the existing ResourceManager test fixture in `tests/runtime/test_resource_manager.cpp` for real carry/forget behavior. |
| Shared-image release closure | A durable victim plus eligible unnamed superseded/shared-prefix handles pin the same image/pages. Victim-only and ordinary-only quotes are zero; the union frees bytes and commits once. Neither single-owner-only nor ordinary-only quote is used as authority; no double-counting. |
| Closure/export intersection | A named victim handle intersects the protected export set: decline the entire victim before proof/commit, never partially release its named owner. Skip physical groups containing protected ordinary holders; release the victim's private bytes while shared pages remain resident. Recheck every included member before commit. |
| Physical quote versus zero arena gain | A positive page quote releases pages but not their Host extent. Consume that candidate, continue to a later candidate that frees arena bytes, and prepare only after the positive measured delta. A truly zero whole-closure quote skips proof. |
| Priority and protection | Final/explicit: redundant before proven closure before ordinary; automatic/survivor: redundant before ordinary. Active/waiting/binding/leased handles and paused snapshots survive. Ordinary cache keeps existing ranking. |
| Remaining Tier 3 deficit | Earlier tiers free part of the shortage; ordinary cache can cover only the remainder. Request the remainder so the all-or-nothing planner can commit the sufficient plan. |
| Contiguity-only shortage | Free bytes cover demand but no extent fits: Tier 1 makes measured progress, then exactly one additional prepare, no oracle or destructive tiers even if Capacity persists. With zero Tier 1 arena progress, retain the initial refusal without a retry. |
| Bounded exhaustion | Native keeps refusing; cursors never refill; attempts at most `2+V+C`; zero-progress steps consume candidates without ending the scan or spinning. Closures do not add retry slots. |
| Automatic save intent | An otherwise eligible durable victim exists, but an automatic save never invokes the oracle/proof and never enters Tier 2. It can succeed through Tiers 1/3 or retain Capacity. |
| Reentrant handler | Inline export during an outer pressure release makes only its initial prepare; outer quoted plan remains valid, no recursive handler or catalog mutation. |
| Concurrent epoch / writer / cleanup | Submission during victim proof or writer makes publication stale when appropriate; writer still off-lock; refused and prepared leases destroyed on worker; writer failure retires exactly once. |
| Stats and scheduling | Reclaim followed by success, refusal or injected failure publishes catalog/occupancy; scheduler/admission notifications do not advance observed checkpoint progress for save-owned mutations/cleanup. Existing `durable_import_commit`, `durable_import_abort`, `durable_export_release`, `resident_waits_for_export` remain valid. |
| Refused automatic save after partial reclaim | Sample progress before save; partial Tier 1/3 reclaim then Capacity refusal must not self-wake its deferred retry, including completion/cleanup notifications. Independent observed engine progress still wakes it within the existing budget. |
| Another export retires / no-op reclaim | A TransactionBusy pressure save retries after another session's export retires with no other engine activity. A refused prepare whose tiers change nothing emits no capacity/admission notification. |
| Survivor-export receipt | `erase_response`'s native survivor-save receipt contains the same reclaim keys as other save paths; preserve the summary through store refusal and any existing replay fallback, without changing committed/origin/dir_sync. |
| Order-dependent two-session final save | The first session is Capacity-refused; the second publishes and becomes a provable victim. One Capacity-only second pass saves the first and Stop completes, with each session counted once even if the second save throws. No second pass without first-pass publication, no retry of non-Capacity refusals, and never a third pass. |

Extend `ninfer_export_state_test` for exact shortage, typed contiguity-only allocation,
Busy, no arena, partial reservation rollback, and failed-copy cleanup. Add native
export coverage proving staging/absolute refusals occur before demotion and later
codec/allocation failure is not falsely documented as a residency rollback.
Existing state test registration is [`RuntimeTests.cmake:66–83`][e-state-registration].

Keep queue tests for engine-only Capacity wakeup and storage-only non-wakeup;
add the partial-reclaim/self-notification refusal case rather than changing the
predicate test. Preserve the retry budget and exhausted-pressure Pending behavior.
Include retained settled-pressure versus newly requested pressure after drain.
Add service/store tests that an export callback can pin another generation without
deadlock, its proof pins survive the full quota/publication lifecycle, all receipt
paths retain reclaim keys after store/publish refusal, and the bounded final-save
second pass fixes the order-dependent stop. These tests do not replace native
byte proof.

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
   The red evidence must also identify the exact native refusal site through the
   new internal shortage classification; public `reason=capacity` alone is not
   proof of a reclaimable Host byte deficit. If classification-only instrumentation
   is needed on `0628db0b`, identify that diagnostic delta separately from the
   reclaim implementation. Do not claim green closure of the incident before
   this predecessor evidence is recorded.
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
change or automatic retry-predicate change is intended. Service orchestration
adds only the bounded final-save pass and save-intent/pin/receipt plumbing.
A source/header fix requires rebuilding the runtime, not a client-only release.

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
   update all Host-preparation callers and refusal classification tests. Record
   the instrumented predecessor's actual refusal site before any green claim.
2. Add native preflight ordering/lease shortage, proof reuse if useful, strict
   single-owner proven release closures and the explicit protection collector.
   Quote/commit each closure as one plan; keep import behavior unchanged and cover
   its existing regressions.
3. Integrate the bounded `checkpoint_change` loop, pressure-depth suppression,
   save-intent tier selection, residual-deficit requests, worker cleanup and
   non-observed admission notifications together. Add the Engine failure matrix,
   including zero arena progress and lossless-only contiguity handling.
4. Return proof readers through Engine to service scope until `store_.save`
   returns; carry the summary through every receipt path, including
   `erase_response`. Add the publication-conditioned second final-save pass and
   its once-per-session totals. Keep the automatic Capacity predicate/test intact
   and add no-self-wake, quota-pin and receipt regressions; update K3/K7 and the test
   guide in the implementation PR, not a second contract authority here.
5. Complete the portable GPU stop runner and registrations; obtain predecessor
   red and exact-candidate green evidence, then native lane and container release
   qualification under the existing managed windows. No host actions are part of
   this design PR.

## 8. Rejected alternatives, risks, and reviewer decisions

Rejected:

- **Blind/unbounded Capacity retry or service-only final-save retry:** no physical
  progress on an idle engine, and repeated deterministic/limit refusals waste
  work. The one publication-conditioned second pass has new victim evidence and
  never substitutes for Engine reclaim.
- **Reuse guarded durable release unchanged:** it cannot obtain a new pressure
  answer after drain; live use also risks recursive save/reclaim and Pending churn.
- **Disable the pressure guard globally:** bypasses other owners' durability and
  admission policy; a byte proof for one owner is not a blanket authorization.
- **Prove all sharers with a new multi-owner transaction:** unnecessary for the
  observed defect. Decline shared logical-owner handles; eligible ordinary
  physical sharers join a single-owner victim's release closure without another
  durable-owner proof.
- **Use storage publication to wake automatic Capacity retries:** withdrawn;
  automatic saves do not byte-prove durable victims. Keep the engine-only
  predicate and avoid reclaim-induced self-wake with non-observed notifications.
- **Worker-held Tier 2 for automatic saves:** out of scope for #78. A bounded live
  Tier 2 would require a later founder decision, not an implicit extension here.
- **Destructive reclaim for a contiguity-only miss:** no byte deficit authorizes
  it. Permit lossless Tier 1 and one prepare after measured progress, then refuse.
- **Trim the exported chain / save responses-only as automatic fallback:** changes
  the promised native continuation instead of recovering available capacity.
- **Keep Device state pinned for the long disk write:** reverses the current Host
  export design and can starve scarce execution slots.
- **Only reclaim target off-chain points or only increase Host capacity:** misses
  reclaimable covered sessions and hides the lifetime/capacity boundary.
- **Change import's policy or fix qwen4_exp durability here:** separate behavior;
  not required to close this export failure.

Risks and conservative decisions retained after the design council:

1. **Proof latency:** a fixed candidate count still permits expensive reads/hashes
   while holding the worker. Tier 2 runs only for final and explicit operator
   checkpoint saves; automatic saves never invoke victim proof. Cheap closure
   eligibility/Host-gain checks run first. Observe final/explicit proof latency in
   qualification. A bounded live Tier 2 is a possible later founder decision,
   not part of #78; neither asynchronous ownership nor weaker byte verification
   is introduced here.
2. **Cache-performance tradeoff:** durability may evict ordinary unprotected cache
   even on an automatic save. Final/explicit priority is lossless replicas, proven
   durable closures, then ordinary cache; automatic/survivor exports use only
   lossless replicas and ordinary cache. Active and waiting requests remain
   protected; ordinary closure members meet the same Tier 3 eligibility.
3. **Conservative sharing:** strict single-owner victim handles can leave Capacity
   when a multi-durable-owner transaction could reclaim more. Accept that safe
   refusal for #78, but include eligible inactive non-durable physical sharers in
   the victim's whole release closure. Any export-set intersection declines the
   whole victim; never partially release a named owner.
4. **Fragmentation and measured progress:** contiguity-only misses use lossless
   Tier 1 and at most one prepare after positive arena progress, never destructive
   tiers. Even byte-deficit reclaim can free insufficient contiguous space. Host
   KV page quotes can leave arena extents occupied; consume zero-progress
   candidates and continue without a prepare. Preserve bounded refusal, not a
   compactor or unbounded retry, and test current arena geometry.
5. **Residual refusal after demotion/reclaim:** a later writer, quota, corrupt-index
   or epoch failure can occur after safe reclaim. Service-held proof readers
   prevent the reclaiming save's quota passes from evicting its victim generation;
   tight quota may instead leave the victim cold/durable and refuse with Quota.
   Once those pins are released, later saves may retire its only native copy under
   ordinary disk-quota policy; transcript replay remains. Target content stays
   valid and current pointer publication remains authoritative. Do not claim
   rollback of all caching side effects or perpetual victim retention.
6. **Qualification fixture availability:** the exact prompt/Device-slot shape that
   satisfies both empty-arena fits and pre-stop shortage is a measured prerequisite
   per selected artifact/backend/GPU. It is intentionally not asserted by this
   source-only design. No fake green if the pressure setup is not constructible.
7. **Open incident refusal-site evidence:** the exact native refusal that fired
   in the reported incident is not proven. The implementation's red fixture on
   `0628db0b` must record it through the new internal shortage classification,
   identifying any classification-only diagnostic delta, before any green claim.
   The public Capacity reason and high occupancy alone do not close this item.

The six preparation unknowns are resolved from source; the incident's exact native
refusal site and the measured fixture/proof-latency envelope remain open
implementation/qualification evidence. The council dispositions below fix the
design policy without changing a durability or publication invariant.

## 9. Design council dispositions

- **D78-F1 (P1):** return proof readers to service ownership through `store_.save`,
  so its quota passes cannot evict its reclaimed victim; later saves retain ordinary quota policy.
- **D78-F2 (P2, re-rated):** use non-observed reclaim/admission notifications to
  prevent automatic self-wake; withdraw storage-triggered Capacity retries and the proposed predicate-test update.
- **D78-F3 (P1):** quote and commit a proven victim plus eligible ordinary physical
  sharers as one closure; disjoint tiers can each quote zero for the same backing.
- **D78-F4:** contiguity-only misses allow lossless Tier 1 and one progress-backed
  prepare, not destruction; Tier 3 requests the residual measured byte deficit.
- **D78-F5:** carry the reclaim summary through `erase_response`'s separate survivor
  export/receipt and existing replay fallback, avoiding a receipt blind spot.
- **D78-F6 (P1):** restrict Tier 2 to final/explicit operator checkpoint saves;
  automatic saves never byte-prove victims, and bounded live Tier 2 is a later founder decision.
- **D78-F7:** permit one Capacity-only second final-save pass after first-pass
  publication; newly covered victims can resolve order dependence without an unbounded retry.
- **daybreak U1:** zero measured arena progress consumes a candidate and continues;
  only positive arena progress permits another prepare, because page quotes can leave extents occupied.
- **mythos U2:** any closure/export-set intersection declines the whole victim;
  never partially release a named owner's selected points.
- **mythos U1 (deferred/open):** the incident's exact native refusal site remains
  unproven; predecessor red evidence must record the new internal classification before any green claim.

## Source references

[e-final]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_service.cpp#L482-L506
[e-save]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_service.cpp#L219-L259
[e-store-export]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_store.cpp#L2031-L2146
[e-save-lease]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_store.cpp#L1084-L1138
[e-oracle]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_service.cpp#L384-L393
[e-load]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_store.cpp#L2324-L2355
[e-fresh]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_store.cpp#L1608-L1622
[e-store-publish]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_store.cpp#L2228-L2275
[e-quota-inventory]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_store.cpp#L1247-L1300
[e-quota-save]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_store.cpp#L2225-L2273
[e-erase-export]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_service.cpp#L336-L358
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
[e-queue-run]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/automatic_checkpoint_queue.cpp#L186-L237
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
[e-host-quote]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/models/qwen3_5/program/storage/checkpoints.cpp#L187-L219
[e-service-progress]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/serve/session_checkpoint_service.cpp#L126-L449
[e-notify]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/engine_checkpoint.inl#L30-L36
[e-admission]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/engine_core.h#L591-L596
[e-observed]: https://github.com/alphastorm/ninfer/blob/0628db0b0997c8bda4ac37c8e84da95986890014/src/runtime/engine/checkpoint_control.h#L130-L138
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
