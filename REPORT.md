# Persistence slice

## Current handoff

- **Incomplete qualification:** user requested handoff after the current diagnostic step. Native K3 source is implemented; production restart/D1 and joint A3 runtime acceptance are not proven.
- Content-safe evidence: `persistence-reservations.json` (both isolated red→green repairs), `persistence-native-r3.json` (native suite passes and genuine #414/#419 failures), `persistence-diagnostic-r4.json` (final stack/quote evidence), `persistence-cpu.json` (CPU builds and typed decoder tests).
- Last runtime source: `3a96e84d7417a005f409bb73d8a0ac80f8112515`, plus an uncommitted private diagnostic overlay; runner reverses the overlay at exit. No production write guard was relaxed.
- Both remaining reservation failures reach `EngineCore::run_prefill_step → Program::advance_prefill → ProgramImpl::advance_prefill → ensure_sequence_kv_mapped → KVHistoryStore::prepare_write`. Grammar: `[8,17)`, retained 8, mapped 1; MTP shared prefix: `[280,289)`, retained 280, mapped 5. **Quote 1, reservation 0**, both before and after any trim. Neither failure involves a rewind at the failing call.
- Native source boundary: `prefill.cpp:200-204` requires an existing permit, prepares the capture boundary, then maps KV. Investigate admission/capture sequencing with EngineCache2; the stack does not establish which earlier operation invalidated the reservation. No third repair is claimed.
- Corrected restart KV capacity now reaches native setup. None/DFlash2 still fail `matching native source missing` at fixture `generate()` source inspection (`test_checkpoint_restart.cpp:197-198`); MTP fixture still asks for draft window 7 although the runtime requires `[1,5]`. These must be corrected before any D1/Reader/preflight runtime claim.
- Final diagnostic job `20261025T035300Z-A2-persistence-diagnostic-3a96e84d-r4-484964`: build 9.442 s; grammar/shared MTP 11.850/13.750 s; restart none/MTP/DFlash2 12.301/5.030/12.851 s; whole job 69 s, exit 1, zero leftover/unowned containers.
- Next owner should isolate the prefill reservation invalidation without changing the guard, resolve the restart source-inspection failure and invalid MTP setup, then run only changed-input affected cases and the joint A3 production-path test. Historical G1 is not a substitute.

## Implemented source and earlier evidence

- Production cutover: removed both Engine tracer hooks, both Program tracer entrypoints, their native implementation, CMake registration and the experimental launcher. `NINFER_TRACER_*` no longer enables an alternative persistence path. G1 below remains historical tracer evidence, not production K3 qualification.
- Existing K3 export/prepare/commit signatures and `Program::checkpoint_layout()` are unchanged from `cc054663`; the additive Main-approved recoverability API is described below. Native and Engine-adapter runtime qualification remains pending. CMake/launcher removal authorized by Integrator; exact Engine-hook removal authorized by EngineCache.
- Native restart fixture covers terminal, genuinely partial accepted output, forced control and lagging-frontier pause, retaining anchor/input/endpoint and comparing their exact continuations after a separate-process import. None/MTP use the three-point terminal path. First runtime attempt refused invalid KV capacity; corrected to 1024. The final source-inspection/MTP setup failures above still block execution. **D1 restart remains unproven.**
- **#414 reproduced and still failing:** the corrected explicit-shared-marker fixture throws `KV write exceeds its page reservation` in both pre-fix and repaired MTP/fp8 candidates, at 14.303/13.750 s. DFlash2/fp8 passes: 12 clients, 1,766 reused tokens, 220 decode rounds / 423 rows, six partial-tail clients, subsequent Engine available; 17.514 s. The earlier raw-token fixture failed only its reuse assertion.
- Expanded candidate `25571c063cf753e78cf39e3cbf88f56a040725a8` compiled on the CPU lane: **42 Ninja steps**, all seven requested targets linked; codec CTest **PASS**, 0.00 seconds (0.03 seconds total). Detached wait observed 304.39 seconds, including queue/build time; not a GPU qualification.
- Main-approved K3/K7 amendment: typed Reader `IoUnavailable` becomes native `Unavailable/IoUnavailable`, never corruption; digest/size mismatch remains `Corrupt`. Native import releases all unpublished reservations through existing RAII. Fake-reader cases cover read and final-verification failure at each classification; runtime execution is pending.
- Amended candidate `465bccf926bad64ed28091a80e4ed3d858ad6b6d` compiled on the CPU lane: **42 Ninja steps**, seven targets linked; codec CTest **PASS**, 0.01 seconds (0.05 seconds total). Detached wait: 134.02 seconds. `persistence-cpu.json` retains both distinct-input builds.
- Initial GPU proof job `20261025T031133Z-A2-persistence-proof-465bccf9-465786` exited 1 in **6 seconds** before compilation/test execution: CMake could not write the historical tracer build's `cmake.check_cache`. No GPU result follows; zero leftover/unowned containers. `persistence-environment-refusal.json` preserves the failure.
- Batch `20261025T031554Z-A2-persistence-proof-465bccf9-r2-469146` proved both reservation regressions **red→green** with unchanged fixtures: pre-fix build 188.195 s; rewind red 1.070 s → isolated `117a166a6` green 1.020 s; admitted-tail-reader red 10.895 s, still red 10.541 s after only the rewind repair → isolated `d1f4832e1` green 10.445 s. `persistence-reservations.json` records commands and hashes. Later candidate link failed because switching `/src` trees on the same Ninja build retained the old Engine object with tracer references; no candidate GPU tests ran. Job exit 1, 259 s, zero leftover/unowned containers. The next candidate uses a distinct build directory.
- Main-approved native recoverability preflight: `validate_checkpoint_import` returns `CheckpointValidation` with `Validated`, `Unavailable`, `Corrupt` or `IdentityMismatch`, never `Prepared`. It shares exact metadata/layout/frontier/identity rebuilding with prepare, streams payloads through at most 4 MiB ordinary Host scratch, and verifies completion without native Host reservations or publication. The caller has already consumed `engine/index.cbor` and needs a fresh Reader for subsequent import. Tests validate under zero Host capacity, reject intact-schema native identity corruption and foreign layout, preserve transient I/O classification and assert unchanged native usage. This addition is not covered by the pinned `465bccf9` batch.
- Preflight source `e355e55afc0e373599f8a5f86edc834ef6fd47c5` compiled: **40 Ninja steps**, seven targets linked; codec CTest **PASS**, 0.01 s (0.07 s total); detached wait 314.25 s. This is CPU compilation, not native preflight runtime proof.
- Decoder correction `3a119f5f`: normalize CBOR parser, missing-field and type errors to `std::invalid_argument`, preserving A3 `Corrupt/InvalidIndex` instead of transient unavailability. Empty/truncated/malformed bytes, missing format/points and wrong table type require that exact class. CPU proof **PASS**, four Ninja steps, codec 0.00 s / CTest 0.04 s; detached wait 536.96 s. Corrected shared-prefix fixture `de640b20` also compiled in two steps; detached wait 464.78 s. Both receipts are in `persistence-cpu.json`.
- GPU `de640b20` batch `20261025T032946Z-A2-persistence-proof-de640b20-r3-477212`: context/KV/codec **PASS**, 1.170 s; full native transactions **PASS** none/MTP/DFlash2, 17.061/17.658/17.714 s. Repaired real grammar **FAIL**, same reservation guard, 11.650 s. Candidate build 196.372 s; whole job 331 s, zero leftovers. `persistence-native-r3.json` records exact commands, sources, models and log hashes. The third reservation cause is under investigation; no write-side guard is relaxed.
- Base: upstream `64d10fbf`; branch `rebase/persistence`.
- RepoPrompt preparation: `prep-v0-13-0-upstream-re-DF08C8`; export `oracle-plan-2026-10-24-174946-prep-v0-13-0-upstrea-1d64.md` (private, not committed).
- G1 tracer re-expressed on `CheckpointStore::Lease`, `StateImageStore::ReadLease`, `ContextSnapshot`, `KVHistoryStore` and native sequence mapping/commit helpers. Full StateImage includes the DFlash2 ring; settled ring frontier is required.
- G1 is a bounded experiment: one BF16/DFlash2 lane, text-only full-history Chat continuation, authenticated disk payload, real process restart, fresh native ownership. No Responses-store, multi-point, Host-only, concurrent, Windows or performance claim follows from this probe.
- Input: fork-owned tracer at `49bc4219`; community checkpoint codec read as design reference only, no community source copied.
- Historical tracer hooks honored `allow_prefix_reuse`; these hooks are now removed. Other architecture Programs are not given a fake persistence implementation.
- Historical tracer launcher confined containers to holder `A2` and loopback port `18120`; the launcher is now removed. Receipts contain hashes and counters, not host paths, model text or credentials.
- G1 candidate `e442df4d` compiled successfully: 627 Ninja steps, lease `20261025T010431Z-A2-tracer-build-e442df4d-210506`, exit 0, 192 seconds, zero leftover/unowned containers. Binary SHA-256 `da54f5a633527515f78eef98f9c967f99b176a9036cd2097098aa9bfd9f362cd`; source archive SHA-256 `ac994248f3396b13a86411d656520752f0329c01c4827d0061ab31000d7f8bd6`.
- Initial restart job `20261025T011218Z-A2-tracer-restart-e442df4d-218677` exited before health with `cudaErrorStubLibrary`; this is an invalid environment, not an engine G1 failure. Corrected image `ov-build-env:master-64d10fbf-r2`, SHA-256 `e7f34ca2f41ecea14da5cce0b2a30cae6b0877e3b3eb371bd92d103bca1121f1`, is GPU-qualified by Infra.
- G1 **PASS**, `G1.json`: real restart, **22,010 checkpoint-hit tokens**, **93 byte-identical continuation bytes**, 10 completion tokens. Priority-1 job `20261025T015558Z-A2-tracer-restart-e442df4d-r2-332480` exited 0 in 64 seconds, zero leftover/unowned containers. Export/import took 6.67914/4.2713 seconds; checkpoint payload 1,638,737,920 bytes. The earlier queued priority-2 proof was canceled before execution to prioritize this lead gate.
- Phase 2 uses frozen Contracts `746b29d2`, streaming/crypto prerequisites `0789709d` and `cb89c89a`. Native CBOR is field-wise and bounded; typed canonical round-trip rejects unknown fields, narrowing, duplicates and noncanonical containers.
- Native set export retains checkpoint, state and page leases, resolves views while serialized, then copies/hashes/writes through a dedicated stream and bounded pinned staging outside Engine serialization. Padding and uncommitted KV columns are zeroed. K2 layout derives from immutable geometry, not allocation capacity.
- Host-only import reserves StateImages, logical pages, extents, inactive histories and checkpoint slots; payload descriptors and delivered-byte verification precede native publication. Prepared reservations abort through RAII; capacity refusal is separate from corruption. Endpoint/input/anchor records preserve exact text/media identity and DFlash2 ring/frontier metadata.
- Phase 2 source `0e64cc77` built successfully on the approved CPU-only lane: **642 Ninja steps**, serving binary plus all five native/codec test targets linked; `ninfer_checkpoint_codec_test` **PASS**, 0.01 seconds (CTest total 0.04 seconds). CPU image SHA-256 `62a4feee8f308cde61095355ff11977986ecce388ad4f057c23014950b681326`. The queued NYC native job `...333221` was canceled before execution to move compilation off the GPU queue. GPU ownership/restart/regression tests were compiled but not executed; this is not native runtime qualification.
- Both original reservation regressions are now GPU-proven red→green, but release-blocking [#419](https://github.com/Neroued/ninfer/issues/419) and [#414](https://github.com/Neroued/ninfer/issues/414) remain reproduced in real prefill scenarios. See final stack/quote evidence above; the isolated repairs are insufficient.

## Upstream-ready draft: #414 / #419 reservation accounting

**Title:** Reserve post-rewind KV growth and refresh admitted units when readers change.

The strict reservation check in `KVHistoryStore::prepare_write` is correct and remains intact. Two independent planning paths can underquote its real work:

1. `write_pages(handle, begin, end)` previously counted all current history pages even when `begin < committed_frontier`. `prepare_write` first trims or forks the owner to `begin`, so later pages no longer belong to that writable history. The quote must use `page_count(begin)` for the post-rewind membership, then include tail COW. With native 64-token pages, rewind 65→8 and write through 65 needs one new page; retaining the old endpoint additionally requires one tail-COW page. This is the isolated `117a166a6` repair.
2. `ProgramImpl::reserve_units` previously skipped page accounting for an already-admitted execution permit. The permit fixes execution bounds, not page writability: another checkpoint/read lease can pin a shared partial tail after admission. Refresh the reservation for the unchanged unit while preserving its permit and rollback ownership. `admitted_unit_acquires_tail_reader` binds a retained 65-token prefix, admits one decode unit, pins the tail, then requires a one-page COW reservation and executes that same unit. This is the isolated `d1f4832e1` repair.

Patch summary: two production files (`kv_history.cpp`, `planning/request_plan.cpp`), no relaxed write-side assertion, no grammar bypass, no retry, and no change to token arithmetic or native page size. The minimal KV fixture is artifact-independent; the late-reader fixture uses real Program admission/execution. The separate Engine fixture launches two synchronized clients over identical Chat prompts with explicit shared-prefix markers, both cold and primed, and requires actual multi-row decode, partial-tail reuse on primed requests and continued Engine availability.

Evidence status: both isolated reservation regressions are GPU-proven red→green (`persistence-reservations.json`), but they are **not a complete repair**. The corrected two-client fixture reproduces #414 on both baseline and candidate MTP/fp8; the repaired real grammar scenario still reproduces #419's reservation guard. DFlash2/fp8 passes the same two-client fixture. The remaining cause is being isolated. This draft is for ProductOps; nothing has been posted upstream.

The final diagnostic narrows both genuine failures to prefill mapping with one required COW page and zero reserved pages, without a rewind. Preserve the write guard and investigate the interval between admission and `advance_prefill`; this is not evidence that either isolated repair should be reverted. Do not post this as a complete-fix PR until the real scenarios turn green.

## Commands

Host directory arguments below use the RUNBOOK variables `A2_SCRIPT_ROOT`, `A2_WORK_ROOT` and `V3_MODEL`; this keeps host paths out of committed evidence.
`CPU_COMMON`, `CPU_SOURCE` and `CPU_BUILD` denote the approved CPU lane's shared wrappers and holder-A2 Git source/build directories. Its source was detached at `0e64cc77`; the build wrapper held one of the two CPU slots and used 12 workers. No GPU model test or production-service change was performed on that lane.

```sh
gpu-lease submit --holder A2 --label persistence-proof-de640b20-r3 --priority 2 --minutes 30 -- \
  python3 "$A2_SCRIPT_ROOT/proof-r3.py" de640b205b6c32bf42f3a56a60d32b2197e06dab
gpu-lease wait 20261025T032946Z-A2-persistence-proof-de640b20-r3-477212 120
gpu-lease submit --holder A2 --label persistence-diagnostic-3a96e84d-r4 --priority 2 --minutes 30 -- \
  python3 "$A2_SCRIPT_ROOT/proof-r4.py" 3a96e84d7417a005f409bb73d8a0ac80f8112515
gpu-lease wait 20261025T035300Z-A2-persistence-diagnostic-3a96e84d-r4-484964 120
gpu-lease submit --holder A2 --label persistence-proof-465bccf9 --priority 2 --minutes 30 -- \
  python3 "$A2_SCRIPT_ROOT/proof-r1.py" 465bccf926bad64ed28091a80e4ed3d858ad6b6d
gpu-lease wait 20261025T031133Z-A2-persistence-proof-465bccf9-465786 120
gpu-lease submit --holder A2 --label persistence-proof-465bccf9-r2 --priority 2 --minutes 30 -- \
  python3 "$A2_SCRIPT_ROOT/proof-r2.py" 465bccf926bad64ed28091a80e4ed3d858ad6b6d
gpu-lease wait 20261025T031554Z-A2-persistence-proof-465bccf9-r2-469146 120
gpu-lease submit --holder A2 --label tracer-build-e442df4d --priority 2 --minutes 20 -- bash "$A2_SCRIPT_ROOT/build-e442df4d.sh"
gpu-lease wait 20261025T010431Z-A2-tracer-build-e442df4d-210506 50
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=120a -DNINFER_BUILD_APPS=ON -DBUILD_TESTING=OFF -DNINFER_BUILD_BENCHMARKS=OFF
cmake --build /build -j16 --target ninfer-serve
gpu-lease submit --holder A2 --label tracer-restart-e442df4d --priority 2 --minutes 10 -- bash "$A2_SCRIPT_ROOT/run-e442df4d.sh"
gpu-lease wait 20261025T011218Z-A2-tracer-restart-e442df4d-218677 50
python3 "$A2_WORK_ROOT/src/tools/durable_tracer.py" --workdir "$A2_WORK_ROOT/run-e442df4d" --build "$A2_WORK_ROOT/build" --model "$V3_MODEL" --model-sha256 "$MODEL_SHA256" --source-commit e442df4d
gpu-lease submit --holder A2 --label tracer-restart-e442df4d-r2 --priority 2 --minutes 10 -- bash "$A2_SCRIPT_ROOT/run-e442df4d-r2.sh"
gpu-lease wait 20261025T012810Z-A2-tracer-restart-e442df4d-r2-245426 50
python3 "$A2_WORK_ROOT/src/tools/durable_tracer.py" --workdir "$A2_WORK_ROOT/run-e442df4d-r2" --build "$A2_WORK_ROOT/build" --model "$V3_MODEL" --model-sha256 "$MODEL_SHA256" --source-commit e442df4d --image ov-build-env:master-64d10fbf-r2
gpu-lease submit --holder A2 --label native-19043a49 --priority 2 --minutes 20 -- bash "$A2_SCRIPT_ROOT/native-19043a49.sh"
gpu-lease wait 20261025T014145Z-A2-native-19043a49-287234 50
cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=120a -DNINFER_BUILD_APPS=ON -DBUILD_TESTING=ON -DNINFER_BUILD_BENCHMARKS=OFF
cmake --build /build -j16 --target ninfer-serve ninfer_context_store_test ninfer_kv_history_test ninfer_checkpoint_codec_test ninfer_qwen3_5_checkpoint_restart_test
ctest --test-dir /build --output-on-failure -R '^(ninfer_context_store_test|ninfer_kv_history_test|ninfer_checkpoint_codec_test|ninfer_qwen3_5_checkpoint_restart_test)$'
gpu-lease cancel 20261025T012810Z-A2-tracer-restart-e442df4d-r2-245426
gpu-lease submit --holder A2 --label tracer-restart-e442df4d-r2 --priority 1 --minutes 10 -- bash "$A2_SCRIPT_ROOT/run-e442df4d-r2.sh"
gpu-lease wait 20261025T015558Z-A2-tracer-restart-e442df4d-r2-332480 50
gpu-lease cancel 20261025T014145Z-A2-native-19043a49-287234
gpu-lease submit --holder A2 --label native-19043a49 --priority 2 --minutes 15 -- bash "$A2_SCRIPT_ROOT/native-19043a49.sh"
gpu-lease wait 20261025T015606Z-A2-native-19043a49-333221 50
gpu-lease cancel 20261025T015606Z-A2-native-19043a49-333221
NINFER_BUILD_HOLDER=A2 NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2-cpu-local \
  "$CPU_COMMON/build-wsl.sh" "$CPU_SOURCE" "$CPU_BUILD" -- \
  ninfer-serve ninfer_context_store_test ninfer_kv_history_test ninfer_checkpoint_codec_test \
  ninfer_qwen3_5_checkpoint_restart_test ninfer_qwen3_5_native_transactions_test
NINFER_BUILD_HOLDER=A2 NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2-cpu-local \
  "$CPU_COMMON/build-wsl.sh" "$CPU_SOURCE" "$CPU_BUILD" -- \
  ninfer_context_store_test ninfer_kv_history_test ninfer_checkpoint_codec_test \
  ninfer_qwen3_5_checkpoint_restart_test ninfer_qwen3_5_native_transactions_test \
  ninfer_qwen3_5_shared_prefix_real_test ninfer_qwen3_5_grammar_real_test
NINFER_BUILD_HOLDER=A2 NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2-cpu-local \
  "$CPU_COMMON/run-in-env-wsl.sh" --gpu --env CUDA_VISIBLE_DEVICES= \
  --mount "$CPU_SOURCE" --mount "$CPU_BUILD" -- \
  ctest --test-dir "$CPU_BUILD" --output-on-failure -R '^ninfer_checkpoint_codec_test$'
```
