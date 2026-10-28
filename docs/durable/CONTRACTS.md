# Durable checkpoint contracts K1–K7

Status: contracts v1. Changes after this freeze require the program lead's approval.
Target: upstream master `64d10fbf`; outer session schema **3**, native checkpoint-set
format **`ninfer-durable-checkpoint-set/1`**, runtime fingerprint schema **2**.
These are separate version domains, not aliases for the fork's schema-v2 payload.

`MUST`, `MUST NOT`, `SHOULD` and `MAY` are normative. Headers in
`src/runtime/contract/{durable_checkpoint_set,durable_engine_api,runtime_fingerprint}.h`
define value types; this document defines wire names, validation and ownership.
They do not implement the store, native codec or Engine adapter.

## Compatibility and authority

- Use new release-isolated roots. Preserve the previous release's roots for rollback.
  Never discover, rewrite, quarantine or import fork-v2 generations as v3.
- Older outer schemas (`schema_version < 3`) return `Incompatible` before origin
  verification, without mutation even when a MAC is present and invalid. Unknown
  outer schemas at or above 3 and unknown native schemas are refused after their
  applicable authentication gates; schema incompatibility itself never quarantines.
  A v1 fingerprint fails closed. Native StateImages are not portable across builds,
  platforms or compiled GPU architectures. Same format does not imply same layout.
- Upstream owns scheduling, DFlash2 execution, context caches, grammar and frontend.
  Persistence transfers their committed state; it does not install a second scheduler.
- Disk contents are untrusted. MAC authentication is origin, not encryption. No raw
  bearer key, raw session name, prompt_cache_key, device address or process-local
  identity enters the checkpoint or a receipt.

## K1 — authenticated checkpoint set

### Publication envelope

One native generation contains `manifest.json`, `manifest.mac`, `responses.cbor`,
`engine/index.cbor`, `checksums.json`, `engine/state/<id>.bin`, and one
`engine/kv/<pool-id>.bin` for each present KV pool. State IDs and pool IDs are
unsigned decimal without leading zeroes. Paths are relative ASCII components;
no empty component, `.`, `..`, slash-at-start, backslash, drive colon, NUL or
platform alias. Generation names match `[a-z0-9][a-z0-9_-]{0,127}`. `current`
contains the generation name followed by one LF and is the only publication pointer.

Every manifest field below is mandatory except the explicitly optional `mode`;
unrecognized fields are rejected in version 3:

| Field | Type and invariant |
|---|---|
| `artifact_type` | `"ninfer_session_checkpoint"` |
| `schema_version` | integer `3` |
| `mode` | optional; absent means native, the only encoded value is `"responses_only"` |
| `session_sha256` | 64 lowercase hex digits; exact authenticated target session |
| `generation` | validated single component; equals directory and current pointer |
| `created_at_unix_ms` | uint64; ordering is not inferred from wall clock |
| `runtime_fingerprint` | complete K2 object, exact match |
| `latest_response_id` | exact native endpoint tag, or latest surviving Responses ID in responses-only mode; nonempty bounded UTF-8 |
| `response_records` | uint64, equals decoded DAG record count |
| `frontier_tokens`, `restored_tokens` | uint32; native: `0 < restored_tokens <= frontier_tokens`; responses-only: both zero |
| `engine_payload_bytes` | checked uint64 sum of all `engine/` files including index |
| `payload_bytes` | checked uint64 sum of all `files` entries |
| `files` | path-sorted unique descriptors `{alignment,bytes,path,sha256}` |

Each SHA-256 is 64 lowercase hex digits. Each size is uint64. Alignment is a
positive power-of-two uint32 describing the wire file/record alignment, not a
pointer. Metadata has alignment 1. Bounds are checked before multiplication,
addition, narrowing or allocation. Default manifest maximum is 16 MiB and maximum
file count is 4096; configured staging bounds also constrain the index and Responses
codec. Never truncate a set to satisfy bounds. `manifest.json` and `manifest.mac`
are excluded from `files`. `checksums.json` is the canonical array of all other file
descriptors, excluding itself; its descriptor is included in the manifest.

### Authenticated responses-only survivor generations

Schema 3 also supports `mode: "responses_only"` for the surviving Responses DAG
after a single-record deletion removes its native endpoint or when that endpoint
cannot be exported. This is a deliberate replay representation, not an empty
native checkpoint and not a new native codec.

- The generation's payload inventory is exactly `responses.cbor` and `checksums.json`,
  plus the usual `manifest.json` and `manifest.mac` publication metadata. It contains
  no `engine/index.cbor`, StateImage or KV file and exposes no Engine Reader.
- `frontier_tokens`, `restored_tokens` and `engine_payload_bytes` are zero;
  `payload_bytes` still counts the actual response/checksum files. All native
  checkpoint statistics are zero, never fabricated continuation evidence.
- The authenticated snapshot encodes `replay_only: true`. Absent manifest mode and
  absent snapshot flag retain the existing native representation and false default.
  The loader cross-checks mode and flag; disagreement is corruption.
- Origin authentication is mandatory for responses-only save and load even when
  the generic native import policy is non-strict. Missing key/MAC cannot become
  an unauthenticated survivor publication.
- `SessionCheckpointStore::save_responses(snapshot, fingerprint, skip)` requires
  `snapshot.replay_only`, uses normal staged writes, flushes, atomic publication and
  quota handling, and does not call Engine export. The complete K2 identity still
  applies. `covers()` is always false for this mode, and the reclaimability oracle
  must not return it as a native generation pin.

K4 governs the guarded Responses-only restore and DELETE acknowledgement. A
subsequent parent request replays the retained transcript; a new completed, stored
turn can establish a normal native endpoint again.

Disk-full and filesystem-quota write failures — POSIX `ENOSPC` and `EDQUOT`, Win32
`ERROR_DISK_FULL` (112), `ERROR_HANDLE_DISK_FULL` (39) and `ERROR_DISK_QUOTA_EXCEEDED`
(1295) — classify as `DurableReason::Capacity` (`capacity`, HTTP 409), not corruption.
A filesystem quota is not the store's own `--session-checkpoint-quota-mib` limit, which
refuses with `quota`. This holds at every save-path I/O site, for payloads and
for `manifest.json`, `manifest.mac` and the temporary current pointer alike: file
creation, write, flush, close (including a deferred write error reported by close),
directory sync and rename all fail with `std::system_error` carrying the OS code.
A failing Windows CRT stream call reports the Win32 code the CRT recorded, because
its errno mapping turns `ERROR_HANDLE_DISK_FULL` into `EINVAL`. The failed save
preserves the prior generation and DELETE survivors; no deletion is acknowledged
without the required durable survivor publication. K7 records the storage-capacity
automatic-retry limitation.

Store usage accounting fails closed. The same scan supplies total usage and each
reclamation candidate's bytes, so no substitute size is safe for an entry whose
status or size cannot be read: the scan fails with that OS code and the save is
refused instead of being measured against an undercount.

### Canonical JSON and MAC

Canonical bytes are UTF-8 without BOM, trailing LF or insignificant whitespace.
Object keys are ASCII and sorted lexicographically by byte; keys are unique.
Strings contain valid Unicode scalars, emitted as UTF-8 except `"`, `\\`, and
U+0000–U+001F. Escape quote/backslash as `\"`/`\\`; use `\b`, `\t`, `\n`, `\f`,
`\r` for those five controls, and lowercase `\u00xx` for the others. Do not escape
`/` or normalize Unicode. Arrays retain their specified order. Integers use minimal
base-10 syntax without `+`, leading zeroes or negative zero; range is int64 through
uint64, with stricter field ranges above. Booleans and null use JSON spelling.
Floating-point literals, NaN, Infinity, duplicate keys, invalid UTF-8 and surrogate
code points are rejected. Typed schemas reject null except explicitly nullable
Responses fields. This is a restricted encoding, not unrestricted RFC 8785/JCS.

Before origin verification, the loader may inspect only the top-level
`schema_version` integer from the bounded manifest read; nothing else is trusted.
If it is less than 3, return `Incompatible/incompatible_schema` with no quarantine,
mutation or import, regardless of whether the MAC is missing, valid or invalid.
A forged schema value can therefore cause only a refusal, never grant authority.
For a value at or above 3, verify the MAC before any further parsing. This narrow
schema-refusal probe does not permit pre-authentication trust of payload paths,
statistics, fingerprint fields or other manifest content.

Verify the MAC over the exact bounded bytes read from `manifest.json`, then perform
normal parsing with duplicate-key detection and require re-encoding to equal those exact bytes.
Do not authenticate a parsed-and-reserialized replacement for the received bytes.
The Python fixture test is the independent canonical/MAC reference; production
uses nlohmann's compact sorted-object encoding only after these validation gates.

Origin key derivation retains the shipped domain separation:

```
origin_key = HMAC-SHA256(raw_api_key_bytes, ASCII("ninfer-checkpoint-manifest-origin-v1"))
manifest_mac = HMAC-SHA256(origin_key, exact_manifest_bytes)
```

There is no NUL terminator in that origin domain. `manifest.mac` is exactly 64
lowercase hex digits, no LF. Compare MACs in constant time. Persistence-enabled
production launches require a nonempty configured API key; derived key bytes stay
outside the checkpoint root. The shipped strict-origin flag additionally requires
a configured checkpoint root and an API key of at least 32 bytes (offline manifest
MAC dictionary-guess resistance; council CR-20260831-originauth R1). This is the
existing CLI floor, not a new security mode. New saves MUST write the MAC. For explicitly
unauthenticated input, strict origin policy returns `Incompatible/origin_required`
without mutation; non-strict policy permits the input but records unauthenticated
origin. This generic compatibility allowance does not apply to responses-only
survivor generations, which always require authentic origin. A present MAC without
a verification key is `Unavailable/origin_key_unavailable`,
not proof of corruption. A verified mismatch is `Corrupt/origin_mac_mismatch`.
Production release launch tooling must not exploit the non-strict import allowance
to omit authentication on newly generated checkpoints.

### Native index and table invariants

`engine/index.cbor` is the CBOR encoding of the field-by-field document below.
Use definite lengths, shortest integer encodings, UTF-8 text keys, booleans, arrays
and maps only; no CBOR tags, indefinite items or floating-point values. Keys follow
canonical JSON's ASCII lexicographic order (this is not a claim of RFC 8949 map-key
length ordering). Reject duplicate/unknown keys. Its exact bytes are hashed in the
outer manifest, independently of semantic decoding.

Top-level keys are `format`, `architecture`, `native_codec`, `state_layout_sha256`,
`endpoint`, `points`, `identities`, `histories`, `states`, `pools`, `payloads`.
`format` is `ninfer-durable-checkpoint-set/1`. Architecture/codec/layout must match
K2. IDs are dense zero-based array indices, never serialized runtime generations.
Endpoint names exactly one `continuation` point and binds `latest_response_id`.
There are 1–64 points, at least one identity/state and no unreferenced table rows.

| Table | Required fields and semantics |
|---|---|
| `points` | `role` (`continuation`, `input_replay`, `long_anchor`), `origin` (`semantic`, `intermediate`), `frontier`, `text_kv_frontier`, `backend_frontier`, `rope_delta`, `tail_hidden_valid`, `dflash_context_frontier`, `identity`, `state`, `histories` |
| `identities` | `ledger` (int32 token IDs), `token_types` (uint8), `positions` (three int32 arrays), `execution_frontiers` (ordered uint32), `vision_items`, `shortlist_tag` (two uint64 values) |
| `histories` | `pool` (pool-array index), `pages` (ordered indices into that pool's page table), `committed_tokens` (uint32) |
| `states` | `layout_sha256`, `payload` (payload-array index), `offset`, `bytes` |
| `pools` | `layout` (K2 pool layout object), `pages` (descriptors `{bytes,committed_tokens,offset,payload}`) |
| `payloads` | path-sorted `{alignment,bytes,path,sha256}` for state/KV files only; exactly agrees with their manifest descriptors |

A point's `histories` is an ascending-pool list of `{frontier,history}`. Text and
backend frontiers reflect the native `text_kv_valid` and `backend_kv_valid` fields;
`frontier` reflects `execution_frontier`. Validate each against its native backend
semantics, ledger length and history coverage; do not assume every backend frontier
must equal the execution frontier. `rope_delta` is int32; `tail_hidden_valid` is
boolean. DFlash2's cyclic context and continuation hidden state live in StateImages:
there is no invented DFlash2 paged backend-KV payload. Ring geometry participates
in the StateImage layout digest; `dflash_context_frontier` is preserved exactly.
MTP/DFlash payload presence follows their native pool inventory.

Identity array lengths agree with the committed ledger. Reconstruct native identity
and derive regular-prefix/position-origin accelerators after import. Recompute
shortlist digests from exact identity and reject disagreement; digests alone never
prove token/position identity. Preserve rewrite execution-frontier boundaries.

Vision inventory is explicit, not C++ memory: `modality` is `image` or `video`;
`temporal`, `height`, `width` are positive int32; `patch_begin`, `patch_count` are
uint64; `content_sha256` hashes encoded media; `timestamps_bits` is an array of
16-digit lowercase-hex IEEE-754 binary64 bit patterns for finite timestamps;
`token_spans` is an array of `{begin,count}` uint32 objects. Validate spans and grids
against native limits and identity length. This preserves every `VisionItem` field
without JSON float rounding. Resident identity does not own patch buffers; encoded
media necessary for replay remains in the exact Responses DAG codec. A codec unable
to reconstruct a supported modality must refuse explicitly, never erase it or
claim a text-only import is an exact continuation.

Deduplicate immutable StateImages and logical pages reachable from all retained
points, including shared physical backing. Export each logical page once at maximum
required committed coverage; histories retain logical page order and prefixes may
use less coverage. Pool IDs/layouts match K2, ranges fit referenced payloads without
overflow, distinct page/state records do not overlap, and offsets satisfy alignment.
Uncommitted/padding bytes must be initialized before serialization, never leak stale
GPU/host allocation contents. Preserve endpoint, input/rewrite points and long
anchors; omit unrelated shared-cache records, residency, retention heat and scratch.
Never serialize pointers, handles, generations, physical slots, row IDs, fences,
leases, CUDA streams or C++ object representations.

## K2 — runtime fingerprint v2

Wire object: `artifact_type="ninfer_session_checkpoint_runtime"`, `schema_version=2`,
then the mandatory objects below. Wire member names match `runtime_fingerprint.h`.
Exact validated object equality is required; missing/extra/unknown fields fail closed.

| Object | Identity fields |
|---|---|
| `build` | `upstream_base_sha`, `patch_stack_sha`, `source_dirty`, `build_profile`, `build_type`, `binary_sha256`, `platform`, `cxx_compiler`, `cuda_compiler`, `cuda_toolkit`, `cuda_architecture` |
| `model` | `artifact_sha256`, `architecture`, `model_id` (effective public alias), `weights_id` (immutable binding digest/identity, not a memory address) |
| `frontend` | `template_sha256` (effective content), `tokenizer_sha256`, `resources_sha256`, `vision_preprocessing` (digest or `disabled`), `parser_codec`, `identity_codec` |
| `native_layout` | `continuation_codec`, `state_layout_sha256`, `state_bytes`, ordered `pools` |
| pool layout | `pool_id`, `name`, `layout_sha256`, `storage_type`, `layers`, `tokens_per_page`, `kv_heads`, `head_dim`, `page_bytes`, `host_serialization_stride` |
| `engine` | `max_context`, resolved `kv_capacity` in tokens, `kv_dtype`, `max_concurrency`, `prefill_chunk`, `speculative_backend`, `draft_window`, `proposal_head`, `enable_vision`, `use_cuda_graph` |
| `responses` | `codec` |
| `deployment` | `profile`, `effective_config_sha256` |

Commit SHAs are full 40-character lowercase hexadecimal; digests are SHA-256 lowercase
hex. Stable names replace enum ordinals (e.g. `bf16`, `int8`, `fp8`, `k8v4`; `none`,
`mtp`, `dflash`, `dflash2`; proposal `full`/`optimized`). NVFP4 is not a shipped durable
profile. Configuration values are resolved values, not auto-policy requests.

**Excluded:** host bytes, cache slots, retention limits, Responses record/byte budgets,
disk quota, staging/write buffers, timeouts, keep-warm, filesystem location, GPU UUID,
CUDA ordinal, raw session/prompt-cache keys and secrets. They govern admission or
operations, not native identity. `kv_capacity` remains included by this contract's
explicit engine-layout binding. Native pool geometry/stride is included, not its
allocation capacity. `effective_config_sha256` hashes the canonical object
`{engine,frontend,model,native_layout,responses}` only, so excluded knobs cannot sneak
back through a full-config digest. The full `--config-sha256` remains an external
startup receipt, not this field. This intentionally supersedes the planning note
that included physical cache capacities and retention in fingerprint v2.

Persistence-enabled production requires known clean source provenance and every
required identity value; unknown/empty/dirty inputs reject launch. Tests may supply
explicit fixture identities. Model binding/frontend/layout identities come from
loaded resources; never synthesize them from a filename or host path.

Serving queries `CheckpointEngineAccess::checkpoint_layout(const Engine&)` and
`frontend_identity(const Engine&)` once at startup and caches the resulting K2
fingerprint. The native query returns `DurableNativeLayout` from the loaded Program's
actual geometry. The frontend query returns `DurableFrontendIdentity` computed once
from loaded tokenizer/resource bytes and the exact selected template source at
compilation. Never reopen the template filename after load or hash resources per save.
## K3 — Engine/Program ownership and transaction API

Only the internal `runtime::CheckpointEngineAccess` adapter exposes persistence.
The public `Engine` API stays unchanged except integration-owned friendship.
`durable_engine_api.h` declares Engine results, limits, target, epoch and move-only
`DurableEngineImport`; the store owns streaming Reader/Writer declarations in
`continuation_checkpoint.h`. Stats on the set API additionally count points/states/pages.

- `checkpoint_layout(const Engine&)` and `frontend_identity(const Engine&)` expose
  the immutable K2 startup identity described above; neither introduces a public
  Engine persistence method or derives identity from admission capacities.
- `set_pressure_checkpoint_handler(Engine&, shared_ptr<PressureCheckpointHandler>)`
  installs the live store-backed save-before-eviction provider.
  `notify_checkpoint_progress(Engine&) noexcept` wakes admission/reclamation after
  checkpoint work completes; the wakeup is not itself observed progress and does
  not authorize eviction or a retry.
- `response_stored(Engine&, std::string_view session_sha256, std::string_view tag)
  noexcept` acknowledges HTTP Responses storage after publication and outside the
  Responses mutex. It promotes only an actually completed native owner's endpoint
  in monotonic publication order.
- `checkpoint_progress(const Engine&) noexcept -> std::uint64_t` reports observed
  Engine/admission/publication progress separately from the service's completion
  wakeup. K7 retries must not manufacture a cause change by notifying themselves.
- `set_reclaimable_session_oracle(Engine&, shared_ptr<const ReclaimableSessionOracle>)`
  installs the trusted store-backed recoverability provider separately from Reader.
  An absent oracle permits no unrelated-session reclaim. Disk metadata, manifest
  flags and a Reader's mere existence never supply this authority.
- `export_session(engine, digest, expected_tag, Writer&, limits)` returns
  `Exported`, `Refused(reason)`, or `Stale`, with `captured_epoch` on success.
  No silent retry against a different tag. A tag with no native owner is
  `Refused(session_not_indexed)` whether or not the session's gate was retired;
  only a resident endpoint other than the acknowledged one is `Stale(tag_mismatch)`.
- `publish_export(engine, digest, tag, expected_epoch, commit_callback)` is mandatory
  for live saves. Under the same per-session publication gate used by submission,
  finish and import epoch changes, it rechecks tag/epoch and invokes only the final
  current-pointer replace+sync callback. It never holds the Engine execution lock.
  Stale never invokes the callback; callback failure/exception is refused/unavailable,
  not success. Tests may inject a fake guard; live service may not omit it.
  Do not hold the store catalog lock when entering this gate, nor enqueue/wait for
  Engine worker work while holding it. Bulk hashing/writing precedes the gate.
- `prepare_import(engine, target, Reader&, fingerprint, expected_stats, limits)`
  returns `Prepared` plus an owning transaction, or `CapacityRefused`, `Corrupt`,
  `IdentityMismatch`, `PolicyDisabled`, `Unavailable`. `Unavailable/IoUnavailable`
  is a retryable read failure, never corruption: abort unpublished reservations,
  preserve the generation, and publish no partial state. Prepared is not visible success.
- `DurableEngineImport::commit()` returns `Imported`, `CapacityRefused`, `Corrupt`,
  `IdentityMismatch`, or `PolicyDisabled`. Commit rechecks the captured epoch/policy;
  stale publication maps to `IdentityMismatch/stale_publication`, never corruption.
  After native publication, no operation may fail. `abort()` is idempotent; destruction
  aborts uncommitted work after all outstanding writers retire.
- `import_session(...)` is prepare→commit/abort convenience for non-Responses callers.
  Responses MUST use K4, not the convenience path. `DurableImportState::Unavailable`
  preserves a prepare-time `Unavailable/IoUnavailable` unchanged through this
  convenience API and service restore callbacks; never coerce it to corruption.
- Capture `session_epoch` before disk loading. Capture publication epoch from the
  submitted handle, not handler completion time. Tokens are runtime-only and compare
  by value. A token from `session_epoch` or `export_session` also retains its session's
  publication gate until its last copy is destroyed, so the later recheck and
  publication serialize on that same gate. The Engine retires a gate only when no
  request, operation or token holds it and no native owner names its session; session
  churn, including restore lookups that find nothing, leaves no per-session gate behind.
- `make_read_queue(Engine&, const std::filesystem::path& root)` receives the
  configured checkpoint root from serving. The factory preserves no-follow
  anchoring; an untrusted manifest never supplies or substitutes this root.

The `fingerprint` argument is the trusted cached active `RuntimeFingerprintV2`
collected by serving, never the untrusted parsed manifest object. Before producing
import input, the store authenticates the manifest and exact-compares its complete
v2 fingerprint against that active identity. Engine validates only the supplied
engine/native-layout/frontend subset against its actual loaded state. Build,
artifact and deployment identity remain the store/serving validation responsibility:
do not fabricate missing hashes or rehash the model artifact on every restore.
Non-Responses convenience callers must use the same authenticated complete-fingerprint
store gate; they do not gain a bypass by calling the internal adapter directly.

Program owns concrete move-only types, implemented beside native context ownership:

```
prepare_checkpoint_export(span<const CheckpointHandle>, DurableLimits) -> ExportLease
validate_checkpoint_import(const DurableCheckpointSet&, const Reader&, DurableLimits) -> CheckpointValidation
prepare_checkpoint_import(const DurableCheckpointSet&, const Reader&, DurableLimits) -> ImportReservation
commit_import(ImportReservation&&) -> vector<CheckpointHandle>
```

ExportLease exposes `state/reason/stats/index` plus `write(Writer&) -> DurableExportResult`.
ImportReservation exposes `state` (`DurablePrepareState`), `reason/stats/valid`; only
Prepared owns a reservation. Normal capacity refusal is not a corruption exception.
Program receives no HTTP session name. It returns handles only to the Engine, never
to the disk codec. A preallocated commit returns all handles atomically; misuse of a
foreign/stale Program is a programming error before publication, not partial success.

### Committed endpoints and pressure outcomes

Submission advances the session's capture epoch, invalidating in-flight export,
but does not replace the committed checkpoint tag. Native completion associates
the completed owner's identity; only the subsequent `response_stored` acknowledgement
promotes that endpoint after HTTP storage. A late handler cannot roll the committed
tag backward. Named durable owners cannot be consumed by a new unacknowledged
turn, and cancellation, expiry or abandonment preserves the prior committed
native endpoint. Acknowledging a newer endpoint, or committing an import,
supersedes every older owner named for the session: its tag can never again be
saved or proven recoverable, so it loses its durable session/tag and returns to
ordinary cache policy instead of naming a pressure or restore victim that never settles.
An older turn that completes after that is superseded at completion and is never named;
a save of its tag therefore reports `Refused(session_not_indexed)`.

`PressureCheckpointVictim::reason` is runtime-only `DurableReason`, default `None`;
it is not a manifest field or a new serialized enum ordinal. Existing
`PressureCheckpointOutcome` semantics remain unchanged:

- `Settled` with `reason == None` reports coverage, not a finished proof. Engine
  must pin and stream-validate the native copy, recheck current authenticated
  generation/tag evidence, and retain the pins through the destructive worker turn.
  Failed proof changes the outcome to `Pending` with its typed reason.
- `Settled` with a reported non-`None` terminal/absent-turn reason retains the
  existing nonretryable settlement policy; it does not claim byte-validated native
  recoverability. Responses-only survivors therefore never masquerade as coverage.
- Retryable failure or still-publishing state remains `Pending`. An unfinished
  paused request may settle through native replay; a completed native turn still
  waits for its HTTP publication.

The production pressure callback performs only bounded coordinator bookkeeping
and enqueuing. Responses/catalog inspection and save work execute on the checkpoint
thread, not synchronously on the Engine worker. This avoids the worker-to-Responses
mutex cycle while a K4 commit is queued; it does not claim all native import or
victim-validation I/O has been moved off the worker.

`Program::validate_checkpoint_import(set, reader, limits)` returns
`CheckpointValidation{state, reason, stats}` with state `Validated`, `Unavailable`,
`Corrupt`, or `IdentityMismatch`. It performs bounded streamed verification and the
same identity, frontier and layout checks as import, without reservation or
publication. `Validated` means the pinned checkpoint passed validation, not that
native state was imported or any victim is unconditionally safe to evict.
Export pins `CheckpointStore::Lease`, `StateImageStore::ReadLease`, page read leases
and immutable identity. Resolve immutable copy views under Engine serialization;
then perform dedicated transfer-stream copies with bounded pinned staging, hashing
and disk writes off-lock. No full duplicate of the checkpoint is required. Off-lock
work MUST NOT consult mutable Program scratch or resolve handles again. Existing
Host replicas stream under leases. Release pins on the Engine worker after transfers
retire, including cancellation. Recheck session/tag epoch before publishing `current`;
newer live work makes the export stale. The store must serialize that final check
with same-session publication so a check→publish race cannot overwrite newer state.

Import validates/authenticates bounded metadata before payload-sized reservations.
Reserve checkpoint slots, Host StateImage destinations, inactive KV histories and
Host extents; fill through a verified reader, call `complete_initialization`, and
prepare snapshots. Capacity failure aborts unpublished objects. Normal import is
Host-only; allocate Device pages/rows only on ordinary native binding/restore.
ResourceManager prepares capacity before commit, then uses adopt→publish→finish with
fresh `publication_order`. No fallible allocation remains after native commit.

`Writer.write_file(path, offset, total_bytes, bytes, alignment)` has mandatory stable
per-file alignment (metadata 1). `Reader.file_descriptor(path)` returns the authenticated
`DurablePayload` descriptor; native code compares path/size/SHA-256/alignment against
the index, not merely file size. Writer owns/copies every queued chunk before returning,
hashes those exact bytes,
requires contiguous non-overlapping complete file coverage, and `finish()` drains
all writes and verifies all declared lengths/digests. `finish()` MUST run off the
Engine execution lock, after bounded capture/lease preparation has released it;
never move the disk drain back into the locked export callback. Engine `Exported`
is staged export evidence, not a durable publication acknowledgement. Reader hashes bytes in the
actual destination or immutable delivered chunk, not a separately reopened file;
`verify_complete() const` requires all expected engine files consumed once, correct
lengths/digests, no holes/overlaps and no in-flight writes. Failed verification
prevents commit. A fresh attempt needs a fresh single-pass Reader. Store verification
of responses/index bytes obeys the same rule. Bounds apply before allocation.

Reader also exposes `virtual DurableReason failure_reason() const noexcept`.
After a failed read or completion check, callers preserve that typed cause rather
than treating every false result as corruption. `IoUnavailable` maps to
`DurablePrepareState::Unavailable`: retryable, no quarantine, no partial publication.
Verified size/digest mismatches report `PayloadSizeMismatch`/`PayloadDigestMismatch`
and map to `Corrupt`, permitting quarantine of that generation. `None` denotes no
recorded failure, not a substitute for an observed I/O error. A1 implements the
accessor in concrete readers/fakes; native and Engine import layers propagate it.
Import never bypasses `allow_prefix_reuse` or serving `CacheParticipation`:
`Disabled`, startup warmup, or disabled cache gives `PolicyDisabled/cache_disabled`
without native reservation. Disk cannot enable reuse, add authority or force eviction.
Capacity/reclamation attempts are bounded and require observed progress; no standby
pinning retry policy from the old fork is reinstated by these interfaces.

`ReclaimableSessionOracle::pin_recoverable(session_sha256, tag) const noexcept`
returns `shared_ptr<const ContinuationCheckpointReader>`: a generation-pinned reader,
or null when the session is not provably reclaimable. The live oracle calls
`store.load(session, active_fingerprint, tag)`, requires the loaded snapshot's
`latest_response_id == tag`, and retains its engine Reader. A cached `covers` boolean
is not an authority substitute. A3 captures the installed oracle for each restore
and retains the reader leases across that restore's bounded retries; no new
`prepare_import` parameter or global mutable per-request state is introduced.

`Reader::fresh_reader() const` returns `shared_ptr<const ContinuationCheckpointReader>`
over the same immutable verified descriptors and pinned generation, with fresh
single-pass read/hash state. It does not reuse a partially consumed verifier.
Destructive reclaim re-validates the pinned reader's generation/tag against the
current publication state per attempt (fresh lookup); stale or missing evidence
makes the session not reclaimable. Before destructive eviction, A3 MUST also call
`Program::validate_checkpoint_import` on the **victim's** pinned checkpoint set and
Reader, proving that victim's recoverable disk copy rather than merely checking
the incoming restore image. Only `Validated` permits proceeding to the still-required
per-attempt victim generation/tag check; any refusal preserves the live victim.
Validation is single-pass, so actual import after validation uses `fresh_reader()`.
`publish_export` still compares the captured
epoch under the gate. This pinned-reader shape supersedes the earlier optional
generation/epoch value return; runtime epochs remain absent from the manifest.
## K4 — Responses restore and session isolation

Extend `OpenAIResponsesStore`, not a parallel store. The snapshot owns a session
digest, explicit latest response ID, parent-before-child DAG and exact records.
Encode `optional<bool> preserve_thinking` as distinct null/false/true values.
Validate IDs, parents, cycles, media/tool types and byte/count bounds before publish.
A required response ID must exist in the authenticated target lineage.

Live serving installs the real store-backed pressure handler and reclaimability
oracle through the internal K3 adapter. These are process-owned providers, not
serialized restore inputs; no public Engine API is added. Restore without a proven
oracle must preserve unrelated sessions rather than infer their recoverability.

Any Engine pressure reclamation used to make room for this restore obeys K3's
fresh pinned-reader generation/tag validation at destructive eviction. A stale or missing pin
preserves the candidate's live state; it cannot be converted into a successful
restore or a claim that the candidate remains durably recoverable.
Native-mode state machine:

```
Absent/Live -> Guarded -> Verified -> EnginePrepared -> ReplacementBuilt
           -> EngineCommitted -> StorePublished
any precommit failure -> Aborted (old Engine and Responses state unchanged)
```

1. Capture the bounded per-session Responses revision guard and Engine session epoch
   before disk read. Active publication RAII starts before Engine submission and ends
   when the response is stored or abandoned. No unbounded revision tombstone table:
   retain a revision entry only while records/publications/guards require it.
2. Load manifest/MAC, exact response bytes, index and payload descriptors outside the
   Responses mutex. Verify fingerprint, target, endpoint and required response.
3. Prepare Engine import outside the Responses mutex. No native/session visibility.
4. Under Responses mutex recheck guard and no active publication; fully preallocate
   replacement DAG/index/LRU/accounting. Reject foreign-session ID collisions and
   insufficient capacity without evicting unrelated sessions. Also reject a loaded
   generation incompatible with the live endpoint or missing any live target-session
   record: unchanged post-capture revisions alone do not prove disk is current.
   Do not compare numeric publication orders across process restarts.
5. Invoke prepared Engine commit under that mutex. It rechecks epoch and refuses if
   newer live or waiting bindings exist. Engine MUST NOT call back into Responses
   while holding its execution lock (no lock-order inversion).
6. A true commit is followed only by no-throw swaps into the Responses store. Nothing
   is visible before commit; concurrent Engine work may see the committed native
   state, while Responses observers remain blocked until the matching DAG swap.
7. On failure abort the reservation and replacement; old state remains authoritative.
   A save waiting for a handler uses publication tracking, never sleep-based guesses.

After a newly completed HTTP response has been stored, serving calls
`CheckpointEngineAccess::response_stored` outside the Responses mutex, then signals
publication progress. Engine completion alone is not the committed-endpoint
acknowledgement; this distinction preserves the last savable endpoint when a later
request never becomes a stored response.

### Survivor restore and durable single-record deletion

For authenticated `mode: "responses_only"` / `replay_only: true`, restore uses the
same namespace, required-ID, revision, publication, live-lineage freshness and
capacity guards, but skips native prepare/import entirely. It preallocates and
atomically publishes only the Responses replacement under the store lock. Successful
restore retains the replay marker and reports `replay_only: true`; it is not a native
checkpoint hit. The next parent request reconstructs the transcript through replay.

```
Absent/Live -> Guarded -> Verified -> ReplacementBuilt -> StorePublished
any prepublication failure -> Aborted (old Engine and Responses state unchanged)
```

Single-record DELETE preallocates the survivor lineage and durably publishes its
replacement generation before erasing the selected RAM record or acknowledging
success. It preserves undeleted disk-only records as well as RAM survivors; the
last RAM record is not necessarily the last durable record. Whole-session erase
is permitted only when neither RAM nor disk has a survivor.

If the deleted record was the native endpoint, or that endpoint is unavailable or
racing, publish authenticated replay-only survivors rather than inventing native
state or returning a special leaf-delete conflict. Authorized leaf DELETE retains
HTTP 200 success semantics; parent/ancestor deletion also preserves undeleted
addressable survivors. Publication failure preserves previous authority. A new
completed stored turn establishes the normal endpoint and retires the replay-only
marker for that lineage; a save/restore of the same survivor endpoint retains it.

Public get/delete/snapshot/export/restore stay in the authenticated session namespace;
a foreign-session ID is indistinguishable from missing. Raw `prompt_cache_key` is
not a global authority or persisted filename.

**Two independent immutable cache scopes (lead-approved):**

- `ContextCacheHints::reuse_scope` is optional string, `http:<session_sha256>` for a
  named authenticated session. Private continuations, endpoints and retained anchors
  require exact scope equality; absent matches only absent. Owner scope survives
  clearing/replacing the mutable newest `session_key` binding.
- `ContextCacheHints::shared_reuse_scope` is optional string,
  `principal:<SHA256(ASCII("ninfer:api_principal:v1") || 0x00 || raw_api_key_bytes)>`.
  The explicit NUL belongs in this domain. SharedStablePrefix/tool-boundary entries
  match the principal scope, so different sessions of one principal retain shared
  fanout reuse. Do not disable this shared reuse or scope it to a private session.
- Serving computes principal scope once from configured authentication; anonymous
  requests have absent principal scope. Every authenticated request has the principal
  scope, even without a named session. Never persist or log the raw key.
- `store:false` keeps its applicable scopes but sets `update_session_index=false`;
  it publishes no named Responses/durable lineage. Import inherits both scopes from
  authenticated caller context, never from manifest-supplied scope authority.

Failure outcomes: schema/fingerprint mismatch → clean replay, no mutation;
capacity/foreign collision/revision or epoch race → refuse and preserve old state;
MAC/digest/native-shape corruption → abort, quarantine only verified corrupt generation;
missing required response → missing; transient I/O → unavailable, preserve for retry;
policy disabled → refuse before reservations. No failure publishes a partial DAG.

## K5 — configuration and CLI union

Master spellings are authoritative for equivalent native controls. Carry only missing
fork product controls; do not keep obsolete fork aliases or recreate old slot policy.
Controller translation is explicit and release-capability gated. Native server/release
specifications move to schema 4; older releases keep their own capability gates.

| Fork surface | Master/rebased surface | Dev mapping / constraint |
|---|---|---|
| `--max-context N` | same | `engine.max_context` |
| `--kv-capacity N\|auto` | same (resolved tokens) | `--context-capacity`, `engine.context_capacity`: shared Device KV+State **bytes**, not a numeric rename |
| no fork equivalent | `--admission-headroom-mib N` (v0.13.1 amendment): explicit admission floor, minimum 256 MiB; omitted retains 1024 MiB | product admission policy, not an alias for Device context capacity |
| `--device-state-slots N` | same: extra checkpoints beyond active lanes | part of shared `engine.context_capacity`; no independent dev slot knob |
| `--host-state-slots N` + `--host-kv-mib N` | `--host-context-mib N`, shared Host State/KV/in-flight bytes | `--host-cache-capacity SIZE`, `context_cache.host_capacity` |
| `--max-private-continuations`, `--max-shared-prefixes`, `--max-long-anchors-per-continuation`, `--max-cache-markers-per-request` | not aliases: upstream resource policy is authoritative; reject obsolete flags | no matching dev fields; admission/retention policy is not identity |
| `--no-prefix-reuse` | same | `context_cache.prefix_reuse=false` |
| `--context-cost-presets FILE` | same | `context_cache.cost_presets` |
| `--max-concurrency N` | same, 1–8 lanes | `engine.max_concurrency` |
| `--prefill-chunk N` | same, resolved native value | `engine.prefill_chunk` |
| `--kv-dtype TYPE` | master-supported shipped type | `engine.kv_dtype`; no NVFP4 product carry |
| `--spec`, `--draft-tokens` | same, upstream backend engine | `speculative.backend`, `speculative.draft_tokens` |
| `--lm-head-draft` | same | `--proposal-head optimized`, `speculative.proposal_head` |
| `--no-cuda-graph`, `--vision`, `--chat-template` | same (template is upstream master) | `engine.cuda_graph`, `model.vision`, `model.chat_template` |
| `--response-store-max-records`, `--response-store-max-mib` | same | `server.responses.max_records`, `server.responses.max_size` (`--response-store-max-size`) |
| `--session-checkpoint-dir` | carry, empty disables | no upstream dev field; product extension, not invented dev compatibility |
| `--session-checkpoint-quota-mib` | carry, 65536 MiB default | no upstream dev equivalent |
| `--session-checkpoint-staging-mib` | carry, 256 MiB default | no upstream dev equivalent |
| `--session-checkpoint-write-buffer-mib` | carry, 6144 MiB default | no upstream dev equivalent |
| `--session-checkpoint-min-tokens` | carry, 32768 default; also short-before-long quota threshold | no upstream dev equivalent |
| `--session-checkpoint-require-origin-auth` | carry, false default; requires configured root and API key ≥32 bytes; new saves still MAC authenticated | no upstream dev equivalent |
| `--gpu-keep-warm-ms` | carry, 0/off default | no upstream dev equivalent |
| `--artifact-sha256`, `--binary-sha256`, `--config-sha256`, `--deployment-profile` | carry product provenance; K2 computes resolved identity separately | no upstream dev equivalent |
| `--stop-event`, `--shutdown-report` | carry Windows lifecycle controls; stop event rejected off Windows | no upstream dev equivalent |
| `--device N`; installed `gpu_uuid` | preserve numeric engine flag; controller resolves exact UUID to current ordinal and verifies ownership | `engine.device`; no invented upstream `--gpu-uuid` flag |
| `--pending-timeout-ms`, `--max-pending-requests` | same | `engine.pending_timeout`, `engine.max_pending_requests` |
| `--log-stats-interval-ms` | same | `logging.stats_interval` |

Host conversion is layout-aware: old host KV bytes plus actual StateImage bytes and
required in-flight headroom, not an invented constant image size. Dev Device conversion
requires native geometry/startup accounting; `auto` remains auto. Preserve effective
shipped profiles via measured admission, not unsupported aliases. GPU UUID is stable
controller selection, not cross-GPU inference or checkpoint layout identity.

### v0.13.1 native admission-headroom amendment

`--admission-headroom-mib N` replaces the default 1 GiB admission-headroom floor
when explicitly supplied. Values below 256 MiB are refused. The calibrated automatic
headroom remains a lower bound: effective admission headroom is
`max(configured_floor_bytes, calibrated_headroom_bytes)`, with a 1024 MiB configured
floor when the option is omitted. Explicit KV capacity does not bypass this policy.

The v0.13.1 native lane specifications set the floor to **640 MiB for RTX 4090**
and **768 MiB for RTX 3090**. The RTX 5090 profile keeps the default.

Upstream `64d10fbf` applies `max(1 GiB, calibrated_headroom)` even in explicit-capacity
mode, refusing the shipped 24 GB native shapes by 224,618,240 bytes on RTX 4090 and
108,762,880 bytes on RTX 3090. The fork's explicit mode subtracted no headroom.
The explicit floor restores bounded native admission without removing calibration's
lower bound or reinstating zero-headroom behavior. The floor itself is admission
policy, excluded from K2; the actual resolved KV capacity remains bound by K2.

## K6 — durable file I/O and publication acknowledgement

PortableCore owns `core/io` primitives; DurableStore owns transaction ordering.
Required semantics: exclusive staging creation, complete short-write/EINTR handling,
file flush, same-volume atomic replace, explicit directory durability mode, bounded
regular-file reads and no-follow traversal. Validate every relative component and
reject symlinks/reparse points throughout traversal, not only the final basename.
Use anchored directory handles or equivalent race-resistant root confinement.
Never accept a path validated before an attacker-controlled replacement.

`io::remove_tree(path)` is the required removal primitive for both files and
directory trees. It returns true after removing the selected entry (including the
POSIX pinned-parent fsync), false when the entry or an ancestor is missing, and
throws on other faults. For removal only, a final symlink/reparse point is unlinked
itself, never followed; ancestor links remain rejected. Every tombstone,
quota-reclamation, quarantine and staging-cleanup
removal uses its anchored no-follow traversal; no raw filesystem removal may bypass
that boundary. A concurrent symlink/reparse-point substitution must never redirect
removal outside the intended tree. All renames, including generation, quarantine
and current-pointer moves, use `io::replace_path` under the same confinement policy.
`replace_path` reports when it attempts the rename system call; a failure before that
attempt changed neither name.
This lead-approved extension adds no dependency and does not change which generations
the store is authorized to reclaim. PortableCore owns the primitive; DurableStore
owns caller migration and the symlink-swap regression.

The generic native store writes `manifest.mac` when an origin key is configured.
This conditional API behavior does not weaken K1: persistence-enabled production
launches always configure a key and new production saves write the MAC.
Responses-only survivor generations always require origin authentication.

Publication order:

1. Create unique unpublished staging generation; failed export leaves current intact.
2. Complete/hash payloads, checksums and canonical manifest; flush every file. When
   an origin key is configured, write and flush `manifest.mac`; the production and
   responses-only policies above require it. Validate Writer.finish and final session/tag epoch.
3. Sync staging directory under platform policy; atomically rename generation and
   sync its parent. Create/flush a temporary current pointer.
4. Atomically replace current, sync session and newly-created session parents under
   platform policy, then acknowledge. Old generations remain until safe quota cleanup.
5. Cleanup after acknowledged publication is best effort; never report a committed
   save as failed solely because later garbage collection failed. A failure before
   publication preserves the prior current. Until `io::replace_path` reports its
   current-rename attempt, current is provably untouched, so the failed save also
   removes its temporary pointer and its renamed generation rather than leaving a
   stale generation for quota cleanup. From that attempt on, including a failed rename
   call or a Windows post-rename parent flush, the generation is retained and the save
   stays unacknowledged with its classified reason; reconciliation reads actual current
   and does not claim rollback that did not happen.

**POSIX:** fsync each file, same-filesystem rename for atomic replace, fsync relevant
directories. Acknowledgement `dir_sync: "fsync"`.

**Windows (binding lead decision):** atomic replacement uses handle-anchored
`NtSetInformationFile` with `FileRenameInformationEx = 65`,
`REPLACE_IF_EXISTS | POSIX_SEMANTICS`, `RootDirectory` set to the pinned destination
directory, and a leaf-only destination name. FlushFileBuffers flushes the staged
file before replacement and the pinned parent handle after replacement. Parent
directory pins permit `FILE_SHARE_WRITE` but still deny DELETE sharing: a rename
into the directory requires `FILE_ADD_FILE` access, which conflicts with a deny-WRITE
pin. Every operation remains handle-anchored. Immediately before each rename,
re-check the pinned handle's attributes: it must not be a reparse point, and its
file index and volume serial must equal those captured at pin time. Ancestor and
leaf reparse checks remain mandatory. No retry weakens the DELETE pin or substitutes
a pathname-based rename.
There is no Win32 `SetFileInformationByHandle` or MoveFileExW fallback.

The standalone Windows `sync_directory` primitive remains an intentional documented
NTFS no-op and never throws; it is distinct from the parent-handle flush within
replacement. Acknowledgement remains `dir_sync: "ntfs-write-through"` and never
claims a POSIX directory fsync occurred. This is the approved native publication
policy, not an equivalence claim to POSIX directory-fsync guarantees on arbitrary
filesystems. Required file/parent flush or replacement failure still prevents a
successful publication acknowledgement; no retries mask it.

**v0.13.1 implementation binding:** the native source at `e2ba490ae`, including
`656a80da1c847d79cf68512c6fef21321a4c7e44`, already implements this class-65,
handle-anchored core replacement and its post-replacement parent flush. The council's
pathname-rename finding applies to the frozen v0.13.0 Linux candidate, not that
native core. K6Windows follow-up `4c8ee39ac5bfa8077ece12c4b8856130d5f2497c`
routes `WindowsCheckpointFileSystem::atomic_replace_durable` through the same
`io::replace_path` primitive instead of the remaining pathname-based `MoveFileExW`
helper. Its failures, like the DirectStorage read queue's anchored payload open and
root creation, propagate as the `io::` primitive's `std::system_error` with the OS
code. The legacy backend's and read queue's own Win32 call failures also throw
`std::system_error`, while DirectStorage HRESULT and contract violations remain
`CheckpointContractError`.
Source presence is not Windows execution or crash-durability evidence; native
qualification must separately exercise the publication path. Successful
Windows publication acknowledges `dir_sync: "ntfs-write-through"`; POSIX publication
acknowledges `dir_sync: "fsync"`, including responses-only survivor saves.

The legacy `make_direct_storage_checkpoint_backend` factory's pathname-based
create/read/delete/lock methods are outside the production checkpoint-store path
and therefore outside the K6 proof scope. Production uses
`make_direct_storage_checkpoint_read_queue`; the legacy backend's
`atomic_replace_durable` now routes to `io::replace_path` as noted above. Qualifying
the production path does not qualify those other legacy factory methods.

A reader pins each accepted read batch's file handles until its writes retire and
verifies the exact bytes delivered to the consumer. Reopening between batches is
permitted only through the same confined/no-follow traversal and complete delivered-byte
hash accounting; changed bytes fail final verification before import commit. Hashing
a separate reopened descriptor instead of delivered bytes is forbidden.
Optional io_uring accelerates reads without changing verification, EOF/error semantics
or lifetime. The store rejects a missing or unavailable selected native read queue
as `Unavailable/IoUnavailable`: retryable, no quarantine and no fallback. It does
not silently substitute a buffered/direct reader or skip authentication.
No new dependency is authorized by this contract.

## K7 — result, error and receipt taxonomy

Enums in `durable_engine_api.h` use the stable snake_case form of their names on the
wire. The constexpr `durable_reason_name(DurableReason)` helper beside the enum is
the single spelling authority for reasons in diagnostics and receipts. Its `unknown`
fallback diagnoses an invalid enum value and is not a valid wire reason.
`none` appears only with success. Store load states are `available`, `missing`,
`incompatible`, `corrupt`, `unavailable`; export is `exported`, `refused`, `stale`;
prepare is `prepared`, `capacity_refused`, `corrupt`, `identity_mismatch`,
`policy_disabled`, `unavailable`; import is `imported`, `capacity_refused`, `corrupt`,
`identity_mismatch`, `policy_disabled`, `unavailable`. A prepared reservation is never an import ack.
Prepare/import `unavailable` with `io_unavailable` is retryable, preserves the generation,
never quarantines it and cannot publish partial state; Reader `failure_reason()`
is the source of that distinction from verified payload corruption.
The non-reserving native validation states are `validated`, `unavailable`, `corrupt`,
`identity_mismatch`; `validated` is neither an import acknowledgement nor authority
to skip eviction-time victim generation/tag revalidation.

| Reason class | Stable reasons | Mutation / retry posture |
|---|---|---|
| Policy | `store_disabled`, `cache_disabled`, `origin_required`, `unsupported_codec` | no mutation; explicit policy/capability change required |
| Identity | `incompatible_schema`, `fingerprint_mismatch`, `tag_mismatch` | clean replay; never quarantine solely for compatibility |
| Race | `stale_publication`, `stale_revision`, `transaction_busy` | preserve newer state; retry only after observed progress |
| Missing | `no_session_records`, `empty_tag`, `session_not_indexed`, `required_response_missing` | no mutation; no retry loop for absence |
| Integrity | `origin_mac_mismatch`, `invalid_manifest`, `invalid_index`, `payload_size_mismatch`, `payload_digest_mismatch` | abort; quarantine verified corrupt generation only |
| Capacity / admission | `metadata_limit`, `staging_limit`, `capacity`, `quota`, `foreign_session_collision` | old state intact; no unrelated eviction for DAG restore |
| Availability | `origin_key_unavailable`, `io_unavailable`, `durability_unsupported`, `cancelled` | preserve generation; retry only after cause/progress changes |

### Wired cause-sensitive retries

`AutomaticCheckpointCoordinator` uses `TransientRetryBudget` in the production
automatic-save/pressure path: at most six retry rearms per completed turn, not an
unconditional retry loop. Each retry waits for observed progress matching its cause:

| Refusal or wait | Progress that may rearm it |
|---|---|
| `transaction_busy`, `tag_mismatch`, `stale_publication`, `stale_revision` | actual Engine or HTTP-publication progress |
| `capacity` | actual Engine/admission progress, including storage-origin Capacity |
| `io_unavailable` | successful storage progress |
| HTTP response still publishing | HTTP-publication progress only; waiting consumes no save-attempt retry budget |

**N9 storage-capacity retry limitation:** disk-full and filesystem-quota writes use
K1's `Capacity` classification and HTTP 409 `capacity` response while preserving the prior
generation and DELETE survivors. `AutomaticCheckpointResult` does not distinguish
storage-origin Capacity from native-admission Capacity. The automatic-save
coordinator therefore rearms either on the next observed Engine/admission progress,
not on external disk reclamation. Reclaiming disk space alone does not trigger an
automatic retry. An explicit client save after sufficient reclaim succeeds without
waiting for that progress, subject to the unchanged publication and policy gates.

Every completed coordinator task notifies the Engine, including retryable refusal.
That completion wakeup is excluded from `checkpoint_progress` and cannot trigger
its own retry. Unrelated or unchanged progress does not rearm the attempt. A new
completed turn resets the bounded per-session retry budget; entries are retained
only while their save/retry bookkeeping needs them.

Exhausted retryable pressure remains `Pending` and does not authorize discarding a
live continuation. Reported nonretryable refusal or an absent/abandoned publication
retains the existing `Settled` policy with its non-`None` reason. `None`-reason
coverage still requires K3's Engine-owned native byte proof. A successful replay-only
save is not coverage: pressure reports the absence of a native endpoint, rather
than relabelling replay as a checkpoint hit.

K1's bounded top-level schema probe is the sole pre-origin refusal exception:
integer `schema_version < 3` yields `incompatible/incompatible_schema` with no
quarantine, mutation or import even if the MAC is present and invalid. At or above
3, MAC verification precedes further parsing. Do not report `origin_mac_mismatch`
or quarantine a legacy generation that has already met the schema-refusal rule.
Public Responses routes do not reveal foreign-session existence or low-level paths.
Detailed receipt reasons are bounded symbolic names, not raw exceptions. Reports must
not include keys, payloads, raw IDs, prompts, checkpoint root paths or machine names.

Operation receipt schema `ninfer-durable-operation/1` has mandatory fields:
`operation` (`save`/`restore`), `state`, `reason`, `committed` (bool), `origin`
(`authenticated`/`unauthenticated`/`not_checked`), `frontier_tokens`, `restored_tokens`,
`payload_bytes`, `points`, `states`, `pages` (unsigned integers).
`dir_sync` is mandatory on a successful save acknowledgement and is exactly `fsync`
or `ntfs-write-through`; absent on failed save or restore (not a fake empty success).
`committed=true` requires `state=exported` or `imported`, `reason=none`, and all
applicable byte/visibility/durability gates satisfied. Save success means durable
publication, not merely Engine export. Engine export stats are not a publication ack.
Receipts may add bounded elapsed durations/counts, never optional identity bypasses.
An acknowledged responses-only restore adds `replay_only: true` and has zero native
statistics; `imported` in that receipt denotes guarded Responses publication, not
native import or reclaimability. Its save still requires authenticated origin and
the same platform-specific `dir_sync` acknowledgement.

`/v1/ninfer/status` separately uses schema version 2 and preserves the shipped fork's
field names. Real master counterparts report real sampled values. Fields with no
counterpart may be present as null only with an explicit field/reason inventory
approved for that status schema, and all consuming harnesses must move with it.
Required non-null fields, especially GPU UUID and runtime attestation, may not be
nulled or fabricated. The operation receipt schema does not rename status fields.

The explicit schema-2 unavailable-field whitelist is:

| Status field | Null reason |
|---|---|
| `cache.private_catalog.capacity` | master catalog is resource-budgeted, not a fixed private-slot count |
| `cache.shared_catalog.capacity` | master catalog is resource-budgeted, not a fixed shared-slot count |
| `cache.host_state.capacity` | unified Host pool has no independently reserved State capacity |
| `cache.host_kv.capacity_bytes` | unified Host pool has no independently reserved KV capacity |
| `cache.private_evictions`, `cache.shared_evictions` | fork pressure-policy counters have no equivalent master counter |
| `cache.shared_catalog.active_references` | master has no shared-owner active-reference count ledger; a lease boolean is not a count |
| `cache.last_materialization_predicted_nanoseconds` | fork whole materialization-plan estimator has no equivalent master metric |

Catalog occupancies, actual state/KV occupancy, Device state capacity and reuse
telemetry remain real sampled values. Never substitute unified Host capacity for
both independent capacities and double-count it. `mtp.acceptance_ratio` and
`mtp.tokens_per_round` retain their existing null-on-zero-denominator meaning.
Optional pressure counters not emitted by the shipped status producer must not
be fabricated as continuity fields.

The audited mandatory non-null set includes the five objects `identity`, `runtime`,
`scheduler`, `cache`, `mtp`; identity `binary_sha256`, `model_artifact_sha256`,
`config_sha256`, `patch_stack_sha`, `deployment_profile`, `source_dirty`,
`upstream_base_sha`, `model_id`, `build_profile`; `runtime.public_model_id`;
scheduler `max_concurrency`, `running`, `prefilling`, `decode_ready`, `waiting`,
`materializing`, `capture_pending`, `committed_decode_tokens`,
`computed_prefill_tokens`; and cache `reused_prompt_tokens`, `last_selection.path`.
The request-log startup environment separately requires actual `gpu_uuid`, `device`,
`gpu_name` and process-correlated instance identity. Consumer updates must preserve
the restart counter-reset and selected-UUID proofs, not merely parse schema 2.

Qualification must distinguish corruption, wrong/missing MAC, stale epoch/revision,
identity mismatch, policy disablement, capacity refusal, cancellation and transient
I/O; assert unchanged live state on every precommit refusal. Header compilation and
canonical/MAC fixture tests are host-only K0 evidence. They do not prove native codec,
Windows crash behavior, G1 restart equality, G2 decode parity or K4 full failure matrix;
those remain the owning slice/integration gates.

`tests/fixtures/durable/manifest-{text,dflash2}.json` are canonical/authentication
envelope examples with explicit synthetic identities, not native restart payloads.
The matching `.mac` files and `mac-vectors.json` bind exact bytes and public test-only
key material; RFC 4231 cases independently check HMAC. `ack-{posix,windows}.json`
demonstrate the two acknowledged durability modes. The C++ test checks nlohmann
encoding against these same bytes and Unicode/control/integer boundary vectors.

## Deferred findings

Council `CR-20261025T085950Z-04b603aa` deferred the following P2 findings from the
v0.13.0 runtime cut to the next runtime release. These are ledger dispositions,
not new exceptions to K1–K7 or claims that the follow-up work has been qualified.
The overlapping Windows findings describe frozen candidate `04b603aa`, not the
current native core; their shared v0.13.1 implementation/qualification follow-up
is recorded in K6.

| Council finding | Deferred scope |
|---|---|
| `claude-R2` | Move bounded restore reads, hashing and victim verification off the Engine execution path without violating native ownership/thread affinity; current finite I/O may stall decode. |
| `claude-R3` | Complete and qualify Windows handle-anchored replacement with the required file/parent flushes rather than the frozen Linux candidate's pathname implementation. |
| `claude-U3` | Supply Windows execution evidence for pin-sharing, durable replacement and restore; Linux receipts do not establish native operation success. |
| `claude-mythos-R4` | Close the frozen Windows source/K6 mismatch with anchored rename, reparse/identity checks and native regression evidence. |
| `daybreak-blue-R4` | Bind runtime-image workflow base-image selection to the selected source's runtime-base receipt, not only repository/digest syntax. |
| `gemini-argon-R3` | Qualify the native pin-sharing and replacement policy instead of carrying the frozen candidate's deny-WRITE/pathname behavior. |
| `gpt-astra-R3` | Prove the native handle-anchored replacement's post-rename parent flush and truthful `ntfs-write-through` acknowledgement; standalone directory no-op is not that flush. |

Closed live-lineage rollback findings, rejected publication-permission claims and
the accepted release-isolated-root boundary are not deferred P2 work. P3 residuals
are outside this appendix's requested scope.
