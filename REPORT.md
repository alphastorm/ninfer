# Persistence slice

- Production cutover: removed both Engine tracer hooks, both Program tracer entrypoints, their native implementation, CMake registration and the experimental launcher. `NINFER_TRACER_*` no longer enables an alternative persistence path. G1 below remains historical tracer evidence, not production K3 qualification.
- K3 signatures and `Program::checkpoint_layout()` are unchanged from `cc054663`; native and Engine-adapter runtime qualification remains pending. CMake/launcher removal authorized by Integrator; exact Engine-hook removal authorized by EngineCache.
- Added native restart coverage for terminal, genuinely partial accepted speculative output, forced control and pause with a lagging DFlash2 frontier. Each checkpoint set contains a real frontend long anchor, input replay point and endpoint; each point is compared against its live continuation after a separate-process Host-only import, including exact token bytes. None/MTP run the same three-point terminal path. These new cases are not yet executed.
- Added an isolated two-client #414 regression at 65/257/513-token prefixes, cold and primed, requiring shared-prefix hits, actual multi-row decode and a healthy subsequent request. BF16 is the portable default; `NINFER_TEST_KV_DTYPE=fp8` selects the reported configuration. The native admitted-tail-reader regression now has an explicit standalone selector for red→green diagnosis; the complete suite still runs it.
- Base: upstream `64d10fbf`; branch `rebase/persistence`.
- RepoPrompt preparation: `prep-v0-13-0-upstream-re-DF08C8`; export `oracle-plan-2026-10-24-174946-prep-v0-13-0-upstrea-1d64.md` (private, not committed).
- G1 tracer re-expressed on `CheckpointStore::Lease`, `StateImageStore::ReadLease`, `ContextSnapshot`, `KVHistoryStore` and native sequence mapping/commit helpers. Full StateImage includes the DFlash2 ring; settled ring frontier is required.
- G1 is a bounded experiment: one BF16/DFlash2 lane, text-only full-history Chat continuation, authenticated disk payload, real process restart, fresh native ownership. No Responses-store, multi-point, Host-only, concurrent, Windows or performance claim follows from this probe.
- Input: fork-owned tracer at `49bc4219`; community checkpoint codec read as design reference only, no community source copied.
- Engine hooks honor `allow_prefix_reuse`, so startup warmup never attempts import. Other architecture Programs are not given a fake persistence implementation.
- Launcher confines containers to holder `A2` and loopback port `18120`; receipts contain names, hashes and counters, not host paths, model text or credentials.
- G1 candidate `e442df4d` compiled successfully: 627 Ninja steps, lease `20261025T010431Z-A2-tracer-build-e442df4d-210506`, exit 0, 192 seconds, zero leftover/unowned containers. Binary SHA-256 `da54f5a633527515f78eef98f9c967f99b176a9036cd2097098aa9bfd9f362cd`; source archive SHA-256 `ac994248f3396b13a86411d656520752f0329c01c4827d0061ab31000d7f8bd6`.
- Initial restart job `20261025T011218Z-A2-tracer-restart-e442df4d-218677` exited before health with `cudaErrorStubLibrary`; this is an invalid environment, not an engine G1 failure. Corrected image `ov-build-env:master-64d10fbf-r2`, SHA-256 `e7f34ca2f41ecea14da5cce0b2a30cae6b0877e3b3eb371bd92d103bca1121f1`, is GPU-qualified by Infra.
- G1 **PASS**, `G1.json`: real restart, **22,010 checkpoint-hit tokens**, **93 byte-identical continuation bytes**, 10 completion tokens. Priority-1 job `20261025T015558Z-A2-tracer-restart-e442df4d-r2-332480` exited 0 in 64 seconds, zero leftover/unowned containers. Export/import took 6.67914/4.2713 seconds; checkpoint payload 1,638,737,920 bytes. The earlier queued priority-2 proof was canceled before execution to prioritize this lead gate.
- Phase 2 uses frozen Contracts `746b29d2`, streaming/crypto prerequisites `0789709d` and `cb89c89a`. Native CBOR is field-wise and bounded; typed canonical round-trip rejects unknown fields, narrowing, duplicates and noncanonical containers.
- Native set export retains checkpoint, state and page leases, resolves views while serialized, then copies/hashes/writes through a dedicated stream and bounded pinned staging outside Engine serialization. Padding and uncommitted KV columns are zeroed. K2 layout derives from immutable geometry, not allocation capacity.
- Host-only import reserves StateImages, logical pages, extents, inactive histories and checkpoint slots; payload descriptors and delivered-byte verification precede native publication. Prepared reservations abort through RAII; capacity refusal is separate from corruption. Endpoint/input/anchor records preserve exact text/media identity and DFlash2 ring/frontier metadata.
- Phase 2 source `0e64cc77` built successfully on the approved CPU-only lane: **642 Ninja steps**, serving binary plus all five native/codec test targets linked; `ninfer_checkpoint_codec_test` **PASS**, 0.01 seconds (CTest total 0.04 seconds). CPU image SHA-256 `62a4feee8f308cde61095355ff11977986ecce388ad4f057c23014950b681326`. The queued NYC native job `...333221` was canceled before execution to move compilation off the GPU queue. GPU ownership/restart/regression tests were compiled but not executed; this is not native runtime qualification.
- Release-blocking upstream [#419](https://github.com/Neroued/ninfer/issues/419): `write_pages` quoted growth against pre-rewind membership, while `prepare_write` first trims/forks to `begin`. The fix quotes post-trim membership and retains the strict write-side reservation guard. Regression: rewind a 65-token history to 8, then write through 65; one new page is required, plus one tail-COW page when the old endpoint remains retained. A separate fix refreshes page reservations for unchanged execution permits: readers acquired after admission can add COW demand. Its real-model regression pins the shared tail after admission, then re-reserves and executes the same unit. Both source-derived regressions await execution; resolution of [#414](https://github.com/Neroued/ninfer/issues/414) is not yet established.

## Commands

Host directory arguments below use the RUNBOOK variables `A2_SCRIPT_ROOT`, `A2_WORK_ROOT` and `V3_MODEL`; this keeps host paths out of committed evidence.
`CPU_COMMON`, `CPU_SOURCE` and `CPU_BUILD` denote the approved CPU lane's shared wrappers and holder-A2 Git source/build directories. Its source was detached at `0e64cc77`; the build wrapper held one of the two CPU slots and used 12 workers. No GPU model test or production-service change was performed on that lane.

```sh
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
  "$CPU_COMMON/run-in-env-wsl.sh" --gpu --env CUDA_VISIBLE_DEVICES= \
  --mount "$CPU_SOURCE" --mount "$CPU_BUILD" -- \
  ctest --test-dir "$CPU_BUILD" --output-on-failure -R '^ninfer_checkpoint_codec_test$'
```
