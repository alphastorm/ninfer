# A1 durable-store port

## State and scope

- Branch: `rebase/durable-store`; base: upstream master `64d10fbf` (`rebase/upstream-64d10fbf`).
- Tested source: **`148384bda50c0eac475ee6c77b245244df4934c1`**. The later report commit changes documentation only.
- Result: the six assigned Linux host targets built and **6/6 CTest tests passed, zero failures and zero skips**. The raw-syscall io_uring backend executed; it was not accepted through skip code 77.
- The approved CPU-lane helpers provided the build slot and Docker environment. No GPU option, model, production service operation, formatter, linter, or project-wide suite was used. The container reported that the NVIDIA driver was unavailable.
- This is store/backend qualification with fake K3 engine adapters, not a real engine restart, G1, G2, G3, native Windows execution, or release approval.

## Implementation and behavior changes

1. **K1 schema v3 replaces the fork's outer continuation format.** Main's final ordering rule permits an allocation-free bounded lexical probe of only the top-level `schema_version` integer. Values below 3 return `Incompatible/IncompatibleSchema` without import, quarantine, or any root mutation regardless of MAC; this also precedes remembered corruption handling. Escaped schema keys and whitespace are recognized, nested schema fields are not mistaken for the envelope. Values at least 3 require origin verification before further parsing; authenticated unknown versions are incompatible rather than silently upgraded. Session, generation, fingerprint, codec, bounds, and exact payload inventory remain checked. Relative paths and generation identifiers are constrained; descriptors carry size, SHA-256, and alignment. Separate low-level binary `CheckpointManifestV1` remains version 1, not a schema-2 continuation compatibility path.
2. **Canonical authenticated metadata.** Apart from that refusal-only schema probe, authentication covers the exact bounded stored manifest bytes before parsing. Manifest JSON then requires canonical compact UTF-8 re-encoding, sorted unique ASCII keys, bounded integers, file count, and size. A present tag is never ignored for an eligible generation: unavailable keys return `OriginKeyUnavailable`; verified mismatch returns `OriginMacMismatch`. Strict policy rejects missing tags with `OriginRequired`. Explicit unsigned compatibility policy remains, not a fallback for an invalid present tag. Tags must be exactly 64 lowercase hexadecimal characters.
3. **Publication order remains transactional.** Payloads and checksums are completed and synced before the terminal manifest/authentication set. Nested payload directories are synced deepest-first on POSIX, followed by the generation directory. Only then can the atomic current-generation replacement occur. Deferred buffered-write failures are surfaced before publication. Failed pre-publication work cannot advance current.
4. **K3 replaces engine-facing fork aliases.** `ContinuationCheckpointWriter` and `ContinuationCheckpointReader` are the engine boundary, including offset/total/alignment-aware writes, descriptors, size/read access, and completion verification. `failure_reason()` preserves the first actual read failure: queue failures are `IoUnavailable`, missing/nonsequential requests `InvalidIndex`, bounds violations `PayloadSizeMismatch`, and streamed digest divergence `PayloadDigestMismatch`. Incomplete observations alone do not invent a failure. `fresh_reader()` shares immutable verified descriptors while resetting consumption/error state and retaining the same generation; known-corrupt sources refuse fresh retry. The old `EngineExporter`/`EngineImporter` and duplicate statistics type are not carried. Statistics use frozen `DurableCheckpointStats`/`DurableExportResult`. Tests supply explicit fakes; production native serialization remains Persistence's responsibility.
5. **Publication is revision/epoch guarded without catalog inversion.** Save requires a `PublishExport` callback taking the captured epoch and a commit closure. There is no optional unguarded live-engine publication path. The catalog mutex is released before both exporter invocation and publication-gate entry; only the final commit closure reacquires it under the gate. Stage/published-generation leases prevent erase or GC from invalidating that interval. The existing single-flight save/quota serialization is preserved with nonblocking `TransactionBusy` refusal rather than making an Engine worker wait on another save. Serialization, hashing, and bulk I/O precede the publication closure. The live service must bind the callback to its real gate and reject stale ResponseStore snapshots: disk erase cannot identify a snapshot captured before deletion but first submitted after deletion.
6. **Import verifies the bytes actually delivered.** Reader consumption is sequential and tracked per file. Digest verification covers the delivered bytes, not a prior verification pass followed by an unchecked reopen. Missing, overlapping, incomplete, replaced, or corrupted input cannot finish a successful import. Verified streamed digest faults are retained for quarantine handling; transient I/O is not corruption. Transparent map lookups avoid allocating a temporary key on every read/descriptor lookup.
7. **K6 is consumed, not reimplemented in the store.** Exclusive no-follow opens, exact I/O, sync, and atomic replacement use the shared platform layer. Every tombstone, quota, and quarantine rename/removal uses anchored `io::replace_path`/`io::remove_tree`, including best-effort cleanup. Directory iteration failures are not treated as an empty store. `quarantine(session_sha256, generation, reason) noexcept` accepts only K7 integrity reasons, removes current only when it still names that generation, and defers payload-directory rename until the final active lease retires. Native capacity/identity/policy/I/O must not call it. Regressions cover ancestor swaps, older-generation quarantine preserving a newer current, and live-reader payload paths remaining usable.
8. **Windows durability remains PortableCore-owned.** NTFS directory sync is the documented nonthrowing no-op and reports `dir_sync: "ntfs-write-through"`, never POSIX fsync; POSIX reports `"fsync"`. The platform dependency snapshot here contains the earlier file-flush/`MoveFileExW` replacement implementation, not a Windows-qualified result. After that snapshot, Main authorized strict-pin NT-relative replacement under the unchanged `io::replace_path` API; PortableCore announced its qualification work. Integration must preserve its qualified provider rather than overwrite it with this older snapshot. This Linux qualification proves neither native Windows replacement implementation.
9. **Quota, quarantine, and reclamation semantics are retained.** Health gates transient superseded-generation tolerance; short sessions are reclaimed before long sessions; active reader pins prevent premature reclamation. Origin-sensitive quota reads authenticate before interpreting manifest ordering. A post-publication reclamation failure does not revoke an already durable save acknowledgement. Publication-directory sync failure withdraws the healthy-store tolerance. Reduced staging budget returns `Incompatible/StagingLimit` without quarantining otherwise valid data.
10. **K7 controls policy.** Retry/shutdown decisions use canonical durable reasons, including busy/tag/stale-publication/stale-revision distinctions. A contradictory legacy coarse diagnostic cannot override K7. Human-facing legacy diagnostics remain display-only. Reason formatting reuses Contracts' `durable_reason_name()` rather than introducing another mapper.
11. **Upstream Responses semantics are preserved in the CBOR snapshot.** The codec retains content-part fields, cache boundaries, resize policy, tool-result name/error status, private cache boundaries, optional thinking state, context DAG structure, relative publication order, and role order. Function/custom tool kinds are explicit; raw custom input/result text is not JSON-wrapped. The single codec label is `ninfer-responses-cbor/1`; incompatible codec identities are refused. Runtime fingerprint checking delegates to ServePort's pure validator and K2 projection.
12. **Queue and checkpoint policy retain the shipped behavior.** Nonblocking enqueue, running-session requeue, pending-session coalescing before the cap, overflow dropping, idempotent drain, shutdown classification, retry bounds, and compacted-on-disk handling remain covered.
13. **Linux stays liburing-free.** The backend uses raw io_uring syscalls and the shipped lifecycle seccomp profile. Missing `STATX_DIOALIGN` or an unsupported query falls back to 4096-byte alignment and an actual O_DIRECT/ring read-write-fsync proof. Larger reported alignments are honored up to the existing 1 MiB ceiling. Tests cover absent metadata, 8192-byte memory/16384-byte offset alignment, and unaligned logical reads with real I/O, following dev `4ec903a0`'s design rather than claiming ZFS execution. The rooted queue factory now creates its root through K6, not raw filesystem recursion; a root-ancestor symlink regression verifies no outside directory is created.
14. **Native read queues preserve confinement.** Linux opens through anchored K6 handles before duplicating a direct-I/O descriptor and pins batches. Windows pins the file before DirectStorage opens it, then compares native volume/file identity. Windows source and SDK DLL registration remain WIN32-gated. Stateless close deleters replace attributed function-pointer deleter instantiations in the queue code.
15. **Obsolete fork assumptions were deliberately removed.** The old `host_kv_restorable_tokens` two-pool arithmetic helper/test was dropped with Persistence's agreement: upstream native multi-store reservations replace that accounting, which A1 does not prove. NVFP4-specific fingerprint coverage was replaced by BF16/int8/fp8 layout incompatibility coverage; unshipped NVFP4 KV, monolithic Program/resource accounting, and standby-pinning retries are not carried. The unpinned oracle `recoverable()` boolean is replaced by `pin_recoverable()` returning a retained Reader: B2 must use `load(active_fingerprint, tag)`, require newest response/tag equality, and retain the engine Reader. `covers()` remains a skip-save predicate, not final reclaim authority. A3 owns fresh current/tag validation before each destructive attempt and retention across bounded retries.

No community implementation was copied. The implementation ports the shipped S1 code and contracts onto upstream types, plus the alignment-fallback design noted above.

## Exact ported test inventory

Six POSIX binaries execute **61 top-level test groups** (30 + 7 + 14 + 8 + 1 + 1). Subcases/assertions are not counted as separate tests. The native Windows binary retains another four groups but was not executed here.

### `tests/test_session_checkpoint_store.cpp` — 30 POSIX groups

```text
test_sha256_streaming
test_codec_round_trip
test_export_refusal_diagnostics
test_kv_layout_runtime_fingerprint_rejection
test_transaction_restart_compatibility_and_corruption
test_active_reader_delete_and_gc
test_may_hold_prefilter
test_store_wide_quota_across_sessions
test_quota_reclaims_short_sessions_first
test_load_scan_failure_does_not_deadlock
test_large_session_resaves_within_quota
test_resave_keeps_other_sessions_within_quota
test_failed_reclamation_keeps_other_sessions
test_serialising_read_queue_streams_batches_in_order
test_post_publish_reclaim_failure_keeps_save_acknowledged
test_native_read_queue_is_required
test_post_verification_replacement_fails_closed
test_buffered_write_backpressure_round_trips
test_buffered_deferred_failure_fails_before_publication
test_hmac_sha256_rfc4231_vectors
test_manifest_origin_authentication
test_manifest_origin_compatibility_window
test_quota_and_erase_refuse_ancestor_symlink_swaps
test_v3_manifest_validation_and_publication_guard
test_schema_v3_and_stream_completion
test_export_and_publication_callbacks_are_catalog_unlocked
test_reader_failure_reasons_and_fresh_retries
test_native_corruption_quarantine_is_generation_bound
test_legacy_schema_bad_mac_is_refused_without_mutation
test_manifest_origin_transient_tag_failure_preserves_current
```

The ancestor-swap case is POSIX-gated, leaving 29 groups for a native Windows build. Added subcases include optional thinking values, all upstream fields, raw custom tool data, unsupported codec, origin-key unavailability, staging-budget refusal, catalog reentry, single-flight refusal, typed I/O/integrity failure, fresh retries, and pinned generation-bound quarantine. Strict-policy legacy tests cover versions 0/1/2/-1, escaped top-level key, whitespace, bad present MAC, and a prior streamed corruption mark; every root entry/content remains identical and no payload import occurs.

### `tests/test_checkpoint_io_contract.cpp` — 7 groups

```text
test_sha256_and_manifest_anchor
test_compatibility_matrix
test_save_order_and_abort
test_save_race_and_failure_resume
test_save_validation_precedes_pause
test_restore_integrity_and_order
test_shared_single_flight_lock
```

The obsolete eighth group, `test_restorable_bound_is_the_host_pool`, was removed for the accounting reason above.

### `tests/test_io_uring_checkpoint_backend.cpp` — 14 groups

```text
test_unavailable_capability
test_transaction_and_trusted_abort
test_cross_instance_stale_generation
test_uncertain_marker_and_foreign_entries_fail_safe
test_partial_failure_cleanup
test_partial_submit_failure_poison_and_recovery
test_commit_failure_preserves_prior_generation
test_double_fsync_failure_persists_uncertainty
test_marker_fsync_failure_does_not_lock_out_root
test_corruption_rejection
test_truncation_and_oversize_rejection
test_expected_allocation_bound
test_continuation_read_queue
test_zfs_wsl_missing_and_large_dio_alignment
```

The read-queue group includes intermediate symlink rejection and factory ancestor-symlink creation refusal with no outside side effect. The WSL negative fixture uses `NINFER_CHECKPOINT_TEST_DRVFS_ROOT` when provided and verifies its actual DrvFS/9p/FUSE superblock type. The default `/mnt/c` fixture is tested only when its observed filesystem is a Windows-drive filesystem: a container directory's name is not evidence of its backing mount. The final run exercised the explicit real 9p mount, not a skipped negative check.

### `tests/test_direct_storage_checkpoint_backend.cpp` — 8 generic groups

```text
test_transaction_and_completion
test_commit_failure_and_trusted_abort
test_stage_cleanup_stale_and_single_flight
test_orphan_cleanup_and_corrupt_manifest_recovery
test_preallocation_rejections
test_commit_corruption_uses_public_contract_error
test_corruption_revalidated_by_coordinator
test_explicit_queue_failures
```

These use the generic fake/native-filesystem boundary and do not imply that the Windows SDK was exercised.

### `tests/test_direct_storage_checkpoint_windows.cpp` — 4 native groups, NOT RUN

```text
test_native_round_trip_and_corruption
test_native_lock_timeout
test_native_reparse_staging_rejection
test_native_read_queue_root_confinement
```

### `tests/test_checkpoint_policy.cpp` — one main group

Canonical K7 retry classes; structural/quota/capacity final decisions; contradictory coarse diagnostic rejection; saved/already-covered/no-record/cache-disabled/refused shutdown outcomes; six-retry budget/cleanup; short-session and compacted-on-disk handling.

### `tests/test_automatic_checkpoint_queue.cpp` — one main group

Nonblocking enqueue; running-session requeue; pending-session coalescing before the cap; full-queue drops; idempotent drain; rejection of new work after drain.

## Exercised qualification

Final source: `148384bda50c0eac475ee6c77b245244df4934c1`.

Environment tag: `ov-build-env:master-64d10fbf-r2-cpu-local`. This is Infra's CPU-local environment, not a GPU-qualified release image. The image tag was printed by the runner; no independent immutable image-ID attestation is claimed.

Observed fixtures:

| Fixture | `stat -f` type | Superblock magic | Use |
|---|---|---|---|
| `/nativeio` | `ext2/ext3` | `ef53` | Docker-native Linux volume; real raw io_uring/direct-I/O cases |
| `$WORK` | `v9fs` | `1021997` | Actual Windows-drive bind mount; explicit WSL rejection case |

`stat` labels the ext-family magic as `ext2/ext3`; no separate filesystem-feature inspection was run.

| CTest target | Result | Elapsed |
|---|---|---:|
| `ninfer_checkpoint_io_contract_test` | passed | 0.00 s |
| `ninfer_direct_storage_checkpoint_backend_test` | passed | 0.00 s |
| `ninfer_io_uring_checkpoint_backend_test` | passed, not skipped | 0.55 s |
| `ninfer_session_checkpoint_store_test` | passed | 1.88 s |
| `ninfer_automatic_checkpoint_queue_test` | passed | 0.00 s |
| `ninfer_checkpoint_policy_test` | passed | 0.01 s |

- Incremental six-target build invocation: **662 s**, including helper slot/environment overhead; Ninja executed eight changed-object/link steps for the store, Linux backend, tests, and dependent queue binary. This is elapsed invocation time, not an isolated compiler benchmark.
- CTest: **2.53 s real**, host-label sum **2.45 s**; enclosing test invocation **3 s**.
- Runner terminal marker: **`A1_QUALIFICATION_EXIT=0`**.

### Exact commands

Private machine names and absolute private directories are intentionally normalized to variables. `$WORK` is the holder's persistent private work directory; `$SOURCE` and `$BUILD` are its source/build children; `$COMMON` is Infra's helper directory. The final execution was launched through the approved detached Windows/WMI-to-WSL wrapper, not by running WSL directly in an SSH session. The wrapper's wait exit code alone was not treated as test success: the explicit runner marker and CTest results above are the evidence.

```bash
set -euo pipefail
trap 'rc=$?; printf "A1_QUALIFICATION_EXIT=%s\n" "$rc"' EXIT
export NINFER_BUILD_HOLDER=A1
export NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2-cpu-local
SOURCE="$WORK/source"
BUILD="$WORK/build"
git -C "$SOURCE" fetch --quiet "$WORK/durable-store-r5.bundle" rebase/durable-store
git -C "$SOURCE" checkout --quiet --detach 148384bda
printf 'A1 subject: '
git -C "$SOURCE" rev-parse HEAD
printf 'A1 image tag: %s\n' "$NINFER_BUILD_ENV_IMAGE"
started=$SECONDS
"$COMMON/build-wsl.sh" "$SOURCE" "$BUILD" \
  -DNINFER_BUILD_APPS=ON -DBUILD_TESTING=ON -DNINFER_BUILD_BENCHMARKS=OFF \
  -- ninfer_checkpoint_io_contract_test ninfer_direct_storage_checkpoint_backend_test \
     ninfer_io_uring_checkpoint_backend_test ninfer_session_checkpoint_store_test \
     ninfer_automatic_checkpoint_queue_test ninfer_checkpoint_policy_test
printf 'A1 targeted build elapsed_seconds=%s\n' "$((SECONDS-started))"
started=$SECONDS
"$COMMON/run-in-env-wsl.sh" --name host-tests --mount "$WORK" \
  --volume ov-A1-durable-148384bda /nativeio \
  --env TMPDIR=/nativeio --env NINFER_CHECKPOINT_TEST_ROOT=/nativeio \
  --env NINFER_CHECKPOINT_TEST_DRVFS_ROOT="$WORK" \
  --seccomp "$SOURCE/tools/lifecycle/ninfer_io_uring_seccomp.json" \
  -- bash -c '
    stat -f --format="A1 fixture path=%n type=%T magic=%t" /nativeio "$1"
    ctest --test-dir "$2" --output-on-failure \
      -R "^ninfer_(checkpoint_io_contract|direct_storage_checkpoint_backend|io_uring_checkpoint_backend|session_checkpoint_store|automatic_checkpoint_queue|checkpoint_policy)_test$"
  ' a1 "$WORK" "$BUILD"
printf 'A1 targeted tests elapsed_seconds=%s\n' "$((SECONDS-started))"
```

The bundle contains the pushed branch's source history; it is not a required distributed product artifact. The persistent build directory was retained for integration reuse.

### Earlier attempts and resolved issues

- Initial leased host batch at `cb3a73260`: failed compilation after 13 s because two store fixtures still used the retired `ResponseStore` type. They were migrated to upstream `OpenAIResponsesStore` with the mandatory restore guard; no tests ran in that attempt.
- A replacement queued leased batch was explicitly canceled before execution after the CPU lane gained a Docker-native volume. Cancellation exit 130 was not reported as a build/test result.
- First CPU-lane script: failed before building because an unnecessary raw `docker image inspect` invocation assumed a Linux Docker CLI. It was removed; subsequent invocations used the provided Windows-Docker bridge helpers. No workstation integration settings were changed.
- CPU-lane run at `13423de9966949c9acbd438db9db4849fafcc56f`: all six targets built in 85 s; five CTest tests passed and io_uring failed the assumption that any directory named `/mnt/c` must itself be DrvFS. `3c1d84343` observes the filesystem and supplies the real Windows-drive fixture without weakening rejection. Its six-target batch passed (239 s build, 4.21 s CTest).
- After that earlier proof, ServePort identified the catalog/publication-gate inversion not covered by the old tests. Main approved single-flight/pins and typed/fresh Reader/pinned-oracle declarations; Integrator supplied exact patches. `202b17064` added the correction and three groups; its six-target batch passed (22 s build, 2.55 s CTest).
- ServePort then identified the strict-policy schema-2/bad-MAC edge. Main explicitly approved the schema-only pre-auth refusal rule. `148384bda` implements it, including status and remembered-corruption ordering, plus the strict immutable-root regression and Linux K6 factory correction. The final six-target proof above is for this latest source, not either earlier passing candidate.

## Integration map and limits

Own source commits: `0789709d3`, `2ef19348a`, `cb89c89a1`, `de8f6b235`, `5d0350a5b`, `9b505fefb`, `13423de99`, `3c1d84343`, `202b17064`, `148384bda`.

Integrator-authorized registration commits: `dc5ffcbb0` and `cb3a73260`. Shared CMake integration remains Integrator-owned; preserve the integration branch's current CLI/config union instead of replacing it with this branch's older dependency snapshot.

`5b2f9b75c` applies Integrator's exact authored/authorized `ReaderFailureReason` and `PinnedReaderOracle` declaration patches, with no default fallback or compatibility alias. Contracts recorded the accompanying availability/fresh-source semantics separately (`8b87b6cb`, superseding pin prose `cf0bc01e`); integration owns the current prepare/import `Unavailable` enum and shared header union.

Contracts' `6f64d0dd` records the final schema-only refusal and rooted `make_read_queue(Engine&, root)` boundary; Integrator owns that shared declaration/current union. A1 supplies the actual rooted factories, including `ninfer_checkpoint_io_uring_linux` registration in `src/runtime/CMakeLists.txt`.

Dependency origins are retained in cherry-pick commit trailers. They include Contracts `746b29d2`, `5a3aea200`, `1a4aff1f4`, `f954b7ce6`, `ef71455e0`, `52b4538e8`, `7c6ba673a`, `46ba2830f`, `c30e5746`; ServePort `70dc649f1`, `85278519`, `3ff7321d`; PortableCore `b0bbecabd`, `bb5bf4cc1`, `69d7b41f4`, `5ba2cc8b`, `c6041c344`. In particular, the actual anchored cleanup provider is included as local `900b8977d`, and the shared reason formatter as local `a033ec08c`.

Remaining qualification boundaries, not hidden fallbacks:

- Real native engine export/import, multi-store reservations, DFlash2/frontier durability, engine lock timing, and restart continuation belong to Persistence/EngineCache/integration. Fake index/state files in these tests are not native model-state compatibility evidence.
- Native Windows MSVC/DirectStorage SDK compilation and the four Windows-only groups remain unexercised by this Linux result; the WIN32 source/registration has been preserved for that lane.
- NativeOps owns the DirectStorage factory root-creation migration to qualified K6 no-follow primitives. PortableCore owns the later strict-pin Windows replacement implementation; neither pending native-provider correction is claimed as proved by this branch's Linux tests.
- ZFS behavior is represented by injected missing/larger alignment metadata plus real direct I/O, not a mounted ZFS dataset. The final run did exercise actual WSL 9p rejection and native Linux-volume I/O.
- No GPU throughput, model quality, production restoration, full CTest suite, or release gate is claimed. Main owns the integrated qualification and review gates.
