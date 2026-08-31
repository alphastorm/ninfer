# Oracle Response



# Durable-4090 rebase plan

## Recommended integration order

1. Inventory both upstream patch series and freeze the branch baseline.
2. Transplant only the v0.4.0 status serializer and its regression test.
3. Apply the v0.4.1 checkpoint hardening in dependency order.
4. Apply the `udp4090` GPU/runtime ports, keeping related `program_impl` and DirectStorage changes adjacent.
5. Run the macOS host gate.
6. Only then produce and test the external Windows package.

This order first aligns shared serving code with `main`, then applies 4090-specific changes. It avoids resolving older UDP changes only to revisit the same code while importing checkpoint contracts.

---

## 0. Record exact patch manifests before editing

Enable recorded conflict reuse and capture the exact files changed by every source commit:

```bash
git config rerere.enabled true

git diff-tree --no-commit-id --name-status -r 2b18a2dd
git diff-tree -m --no-commit-id --name-status -r 43f9a53b
git diff-tree --no-commit-id --name-status -r ab3d07f8
git diff-tree --no-commit-id --name-status -r bb290513

for c in \
  ef4605a8 0978ac09 4b66c882 \
  c15e0e9e 5e76d11a \
  92c360fb dfe932b8 \
  72e931b2 387affc0 \
  8bba5eb4
do
  git diff-tree --no-commit-id --name-status -r "$c"
done
```

For `43f9a53b`, determine whether it contains an independent merge-resolution delta or merely includes `2b18a2dd`:

```bash
git merge-base --is-ancestor 2b18a2dd 43f9a53b
git diff 43f9a53b^1 43f9a53b
git log --oneline 43f9a53b^1..43f9a53b^2
```

Do not blindly cherry-pick both the individual commit and the full merge. Apply `2b18a2dd`, then apply only the merge’s unique commits/resolution; use `git cherry-pick -m 1 43f9a53b` only if that merge delta is genuinely required.

---

# 1. Fix `/v1/ninfer/status` first

## Current branch location

The selected code establishes that `GenerationService` does **not** construct the status JSON:

```cpp
ninfer::RuntimeStats runtime_stats() const { return engine_->runtime_stats(); }
```

It only forwards engine statistics. The `/v1/ninfer/status` document is therefore built in:

- `src/serve/http_server.cpp` — expected home of `format_status_json`
- Possibly endpoint plumbing in `src/serve/http_server.h`
- Existing coverage should be placed in:
  - `tests/test_serve_metrics.cpp`, if the formatter is exposed through metrics helpers
  - otherwise `tests/test_http_contract.cpp`, through the status endpoint contract

No changes should be needed in:

- `src/core/disk_state_cache.*`
- `src/core/direct_storage_engine.*`
- `src/serve/session_checkpoint_store.*`

## Divergence to correct

The released 4090 response is known to emit:

- `telemetry_available: false`
- `materializing: null`

The pinned OMP 18.0.9 client expects the v0.4.0 hierarchy and rejects the null value before a session starts.

Transplant the body of `format_status_json` from `main` at `f4fb315a`, rather than recreating its schema manually. Preserve all branch-specific status fields around it, but make the engine-telemetry subtree exactly type-compatible with main:

- same keys;
- same nesting;
- same scalar/object/array types;
- concrete numeric zeros where telemetry has no samples;
- no `null` in any OMP-required field;
- copy main’s exact value for `telemetry_available` rather than deriving a new convention.

Do not change `RuntimeStats` merely to manufacture telemetry. The compatibility fix belongs at the status serialization boundary unless the main implementation proves otherwise.

## Files for this step

Modify:

- `src/serve/http_server.cpp`
- `tests/test_serve_metrics.cpp`

Conditionally modify, only if endpoint-level access is required:

- `src/serve/http_server.h`
- `tests/test_http_contract.cpp`

Acceptance test: compare the complete required telemetry subtree with a fixture taken from `f4fb315a`, including JSON types—not just values.

---

# 2. Import v0.4.1 checkpoint hardening

Use `git cherry-pick -x` and keep each hardening unit as a separate commit.

## 2.1 Named skip reasons and replay-successor retagging

Apply:

- `2b18a2dd`
- the unique part of merge `43f9a53b`

Expected resolution files:

- `src/runtime/contract/continuation_checkpoint.h`
- `src/runtime/engine/checkpoint_engine_access.h`
- `src/runtime/generation/resource_manager.h`
- `src/serve/session_checkpoint_store.h`
- `src/serve/session_checkpoint_store.cpp`
- `src/serve/generation_service.cpp`
- `src/serve/http_server.cpp`
- `tests/test_session_checkpoint_store.cpp`
- potentially `tests/test_automatic_checkpoint_queue.cpp`

### Conflict risk

| File | Risk | Reason |
|---|---:|---|
| `session_checkpoint_store.{h,cpp}` | High | Direct target of the hardening; this branch already contains substantial durability, quota, reader-lifetime, and deletion work. |
| `continuation_checkpoint.h` | Medium | The 4090 branch added the asynchronous checkpoint read-queue contract before the writer/reader contracts. Preserve it while adopting new result/skip-reason types. |
| `resource_manager.h` | High | Replay-successor retagging is likely here, and the UDP MTP/K=15 work may touch the same lifecycle. |
| `generation_service.cpp` | Medium–high | Preserve Windows DirectStorage read-queue construction and staging settings while adopting new checkpoint result propagation. |
| `http_server.cpp` | Medium | It will already contain the status transplant; resolve checkpoint status/skip fields without reverting that serializer. |

### Required branch preservation

Keep the Windows-specific serving checkpoint path:

```cpp
runtime::windows::make_direct_storage_checkpoint_read_queue(...)
```

Also preserve:

- `ServeOptions::session_checkpoint_staging_bytes`;
- propagation of that value to `SessionCheckpointStoreOptions`;
- the engine checkpoint/restore staging limit;
- `DirectoryCheckpointReader`’s Windows queue-backed reads;
- the Windows constructor requirement that the read queue be available.

Review every designated `SessionCheckpointStoreOptions` initializer. New upstream fields can make C++ designated-initializer ordering fail even when the logical values are correct.

---

## 2.2 Quota transition tolerance and health-gated deferred reclamation

Apply:

- `ab3d07f8`

Primary files:

- `src/serve/session_checkpoint_store.h`
- `src/serve/session_checkpoint_store.cpp`
- `src/runtime/generation/resource_manager.h`
- `src/serve/generation_service.cpp`
- `tests/test_session_checkpoint_store.cpp`

Possible propagation files, depending on the upstream manifest:

- `src/runtime/contract/continuation_checkpoint.h`
- `src/serve/http_server.cpp`
- `tests/test_automatic_checkpoint_queue.cpp`

### Current behavior being replaced

This branch presently reclaims before publication and strictly rejects the save when:

```cpp
!enforce_disk_quota_locked(...)
```

That is safe, but it has no explicit health-gated transition tolerance. Reconcile the incoming logic so that:

- the generation currently being published remains protected;
- active readers remain protected;
- temporary over-quota transitions are tolerated only under the upstream health condition;
- reclamation remains deferred and quota-accounted;
- a failed transition does not silently discard the old `current`;
- unreadable/corrupt `current` pointers are not treated as healthy eviction candidates.

Do not weaken tenant/session isolation or manifest verification while resolving the quota changes.

---

## 2.3 Post-publish reclaim acknowledgement and cleanup degradation

Apply:

- `bb290513`

Primary files:

- `src/serve/session_checkpoint_store.h`
- `src/serve/session_checkpoint_store.cpp`
- `tests/test_session_checkpoint_store.cpp`

Possible caller propagation:

- `src/serve/generation_service.cpp`
- `src/serve/http_server.cpp`

### Important current-branch behavior

There are two relevant sites today:

```cpp
(void)cleanup_tombstone(options, tombstone);
```

and `cleanup_tombstone()` directly invokes the injected hook without containing exceptions. Therefore:

- a failed cleanup is often ignored after logical publication/retirement;
- a throwing hook can escape quota/save paths and turn cleanup degradation into an availability failure.

Adopt the upstream distinction between:

1. logical publication/retirement;
2. acknowledged physical reclamation;
3. deferred but quota-accounted cleanup;
4. a throwing cleanup hook, which must degrade safely rather than unwind through a committed operation.

Retain the existing invariant that a successfully renamed session is logically erased even if physical tombstone cleanup is deferred.

---

# 3. Import `udp4090` port groups

Preserve commit order inside each group. Use `git cherry-pick -x`.

## 3.1 MTP K=15 and GQA decode kernels

Apply:

1. `ef4605a8`
2. `0978ac09`
3. `4b66c882`

Expected files to inspect or resolve:

- `src/ops/launcher/gqa_attention_decode.cu`
- `src/ops/launcher/gqa_attention_prefill.cu`
- `src/ops/wrapper/gqa_attention.cpp`
- `src/targets/qwen3_6/impl/runtime/program_impl.h`
- `src/runtime/generation/resource_manager.h`
- `tests/ops/test_gqa_attention.cpp`
- `tests/ops/test_mtp_round.cpp`
- `src/CMakeLists.txt`
- `tests/CMakeLists.txt`, only if a commit introduces or renames sources

Conflict warning: `resource_manager.h` may now contain replay-successor retagging from the checkpoint hardening. Preserve that state machine while importing the expanded MTP window and resource accounting.

---

## 3.2 Chunked KV snapshot staging and MTP restore stride

Apply:

1. `c15e0e9e`
2. `5e76d11a`

Primary files:

- `src/core/disk_state_cache.h`
- `src/core/disk_state_cache.cpp`
- `src/core/direct_storage_engine.h`
- `src/core/direct_storage_engine.cpp`
- `src/targets/qwen3_6/impl/runtime/program_impl.h`
- `tests/test_disk_state_cache.cpp`
- potentially `src/core/paged_kv_cache.*`

### Very high-conflict areas

#### `disk_state_cache.cpp`

This branch already has:

- content-addressed CoW pages;
- a 4 KiB-aligned manifest payload;
- page-pool compaction;
- DirectStorage assembly of text pages followed by GDN/MTP/tail bytes.

Do not replace that format wholesale with an older monolithic snapshot path. Integrate chunking into the existing CoW layout.

#### `program_impl.h`

The current DirectStorage layout is:

```text
[text pages][GDN][MTP KV][tail hidden]
```

Both the K=15 work and the stride fix affect this consumer. Verify explicitly:

- restored page count versus allocated page IDs;
- per-page MTP byte stride;
- offset after GDN;
- MTP payload size versus `mtp_page_count`;
- tail-hidden offset;
- host fallback and DirectStorage path use identical layout calculations.

Avoid compacting invalid page IDs without also advancing the staging source offset; otherwise a hole in the destination page table can shift later staged pages.

#### `direct_storage_engine.cpp`

Preserve:

- dynamic, zero-permanent-reservation staging;
- D3D12 shared-resource/CUDA external-memory import;
- D3D12 fence → CUDA stream wait;
- serialized staging ownership under `recursive_mutex`;
- the final caller-side CUDA synchronization before `release_staging()`.

---

## 3.3 WDDM evictable-budget CLI toggle

Apply:

1. `92c360fb`
2. `dfe932b8`

Expected files:

- `include/ninfer/types.h`
- `src/core/device.h`
- `src/core/device.cu`
- `src/runtime/engine/public_types.cpp`
- `src/runtime/engine/request_memory.cpp`
- `src/runtime/generation/resource_manager.h`
- `src/serve/serve_options.h`
- `src/serve/serve_options.cpp`
- `src/serve/generation_service.cpp`
- `tests/test_serve_options.cpp`
- `tests/test_request_memory.cpp`
- `packaging/windows/qwen38-4090-v0.1/server-config.json`, if the shipped package exposes the toggle

Conflict warning: preserve all checkpoint option propagation in `generation_service.cpp`. Add the WDDM setting to `EngineOptions` without dropping:

- prompt-cache options;
- session-checkpoint read-queue setup;
- checkpoint staging size;
- runtime fingerprint construction.

If the toggle changes memory planning, decide whether it belongs in `session_checkpoint_runtime_fingerprint()`. A setting that changes restorable device layout must invalidate old checkpoints; an operating-policy-only toggle should not.

---

## 3.4 D3D12 residency fence fixes

Apply:

1. `72e931b2`
2. `387affc0`

Primary files:

- `src/core/direct_storage_engine.h`
- `src/core/direct_storage_engine.cpp`
- `src/core/device.h` or `src/core/device.cu`, if present in the exact commit manifest
- `tests/test_disk_state_cache.cpp`

Conflict risk is high in `direct_storage_engine.cpp`, because this branch already introduced:

- adapter matching by CUDA LUID;
- a shared D3D12/CUDA fence;
- dynamic staging allocation;
- device-removal recovery;
- immediate staging teardown tests.

Integrate residency fences into that lifecycle rather than restoring an older permanent-buffer model. Define separate ownership for:

- DMA completion;
- CUDA consumption completion;
- D3D12 residency/eviction completion.

A D3D12 DMA fence alone does not prove that CUDA has finished consuming the imported allocation. Keep the consumer’s `cudaStreamSynchronize()` before final staging release unless the imported fix provides an equivalent CUDA-completion handshake.

---

## 3.5 Streaming UTF-8 repair

Apply last:

- `8bba5eb4`

Expected files:

- `src/serve/http_server.cpp`
- `src/serve/responses_http.cpp`
- `src/serve/translate.cpp`, if the stream adapter is shared
- `src/text/unicode.h`
- `src/text/unicode.cpp`
- `tests/test_http_contract.cpp`
- `tests/test_responses_schema.cpp`

Applying this last avoids resolving its HTTP streaming edits twice against the status and checkpoint-status work.

Tests should split multibyte UTF-8 sequences across multiple deltas and cover:

- two-, three-, and four-byte code points;
- a trailing partial sequence;
- completion flush;
- tool-call filtering;
- reasoning and visible-content channels.

---

# Conflict summary

| Branch hotspot | Incoming groups | Risk |
|---|---|---:|
| `src/serve/http_server.cpp` | Status, checkpoint hardening, UTF-8 | High |
| `src/serve/generation_service.cpp` | Checkpoint hardening, WDDM | High |
| `src/runtime/generation/resource_manager.h` | Checkpoint retagging, MTP K=15, WDDM | Very high |
| `src/serve/session_checkpoint_store.{h,cpp}` | All v0.4.1 hardening | Very high |
| `src/runtime/contract/continuation_checkpoint.h` | Named results/reasons vs Windows read queue | Medium |
| `src/core/disk_state_cache.{h,cpp}` | Chunked staging | High |
| `src/core/direct_storage_engine.{h,cpp}` | Chunking and residency fences | Very high |
| `src/targets/qwen3_6/impl/runtime/program_impl.h` | MTP K=15, snapshot chunking, stride fix | Very high |
| `src/serve/serve_options.*` | WDDM CLI | Medium |
| `src/CMakeLists.txt` / `tests/CMakeLists.txt` | New or renamed GPU sources/tests | Medium |

---

# macOS host validation

## Tests that do not require CUDA or Windows

From the supplied `tests/CMakeLists.txt`, these are the existing host-safe gates:

1. **Public header compile test**
   - Target: `ninfer_public_api_test`
   - Source: `tests/test_public_api.cpp`
   - It deliberately receives only `include/` and no CUDA/core/engine libraries.

2. **Golden-equivalence Python test**
   - `tests/test_golden_equivalent.py`

3. **Release-authority Python test**
   - `tests/test_release_authority.py`

Suggested commands:

```bash
clang++ -std=c++20 -Iinclude tests/test_public_api.cpp -o /tmp/ninfer_public_api_test
/tmp/ninfer_public_api_test

python3 tests/test_golden_equivalent.py
python3 tests/test_release_authority.py
```

## Tests that are not macOS/no-CUDA tests as currently wired

- `ninfer_session_checkpoint_store_test` is logically host-oriented, but links `ninfer_serve`, which links `ninfer_engine` and CUDA-backed `ninfer_core`.
- `ninfer_disk_state_cache_test` directly uses CUDA and, on Windows, DirectStorage.
- Tests created through `ninfer_add_test` without an explicit host-only library generally link `ninfer_core`.
- The DirectStorage checkpoint read-queue test is registered only under `WIN32`.
- GPU op, target, arena, device, KV, and request-memory tests require the CUDA build.

Therefore the macOS gate is useful for source/package consistency but does **not** validate the central checkpoint or 4090 runtime changes.

## Pre-Windows gate

Before handing off:

```bash
git diff --check
git status --short
git grep -nE '^(<<<<<<<|=======|>>>>>>>)'
```

Additionally verify:

- every requested source commit appears once in history;
- no checkpoint initializer lost the Windows read queue or staging size;
- no UDP resolution reverted the status serializer;
- CMake contains every newly introduced source;
- host-safe tests above pass;
- package scripts still reference the intended deployment profile.

---

# Required external Windows acceptance gate

At minimum build and run:

- `ninfer_serve_metrics_test`
- `ninfer_http_contract_test`
- `ninfer_session_checkpoint_store_test`
- `ninfer_automatic_checkpoint_queue_test`
- `ninfer_direct_storage_checkpoint_read_queue_windows_test`
- `ninfer_disk_state_cache_test`
- `ninfer_gqa_attention_test`
- `ninfer_mtp_round_test`
- `ninfer_serve_options_test`
- `ninfer_request_memory_test`

Then smoke-test:

1. `/v1/ninfer/status` against pinned OMP 18.0.9.
2. Save, restart, and restore a durable session.
3. Healthy temporary over-quota transition followed by deferred reclamation.
4. Throwing tombstone-cleanup hook degradation.
5. K=15 MTP restore with bit-exact page stride.
6. Rapid DirectStorage restore/teardown under WDDM.
7. UTF-8 split across streaming chunk boundaries.

---

# Windows package entrypoint

The shipped 4090 packaging profile visible in this tree is:

```text
packaging/windows/qwen38-4090-v0.1/
```

The package-building entrypoint referenced by the release-assets test is:

```text
packaging/windows/qwen38-4090-v0.1/New-Package.ps1
```

Related shipped lifecycle scripts are:

- `Install-Release.ps1`
- `Control-Release.ps1`
- `Control-GpuOwner.ps1`

`tests/test_release_assets.ps1` explicitly treats `New-Package.ps1` as the package builder.

The exact CMake preset name used by the v0.3.1 zip cannot be established from the supplied file contents. Before the Windows handoff, inspect:

- root `CMakePresets.json`
- `packaging/windows/qwen38-4090-v0.1/New-Package.ps1`

Treat the preset invoked by `New-Package.ps1` as authoritative; do not substitute a guessed generic Windows preset.