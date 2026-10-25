# Persistence slice

- Base: upstream `64d10fbf`; branch `rebase/persistence`.
- RepoPrompt preparation: `prep-v0-13-0-upstream-re-DF08C8`; export `oracle-plan-2026-10-24-174946-prep-v0-13-0-upstrea-1d64.md` (private, not committed).
- G1 tracer re-expressed on `CheckpointStore::Lease`, `StateImageStore::ReadLease`, `ContextSnapshot`, `KVHistoryStore` and native sequence mapping/commit helpers. Full StateImage includes the DFlash2 ring; settled ring frontier is required.
- G1 is a bounded experiment: one BF16/DFlash2 lane, text-only full-history Chat continuation, authenticated disk payload, real process restart, fresh native ownership. No Responses-store, multi-point, Host-only, concurrent, Windows or performance claim follows from this probe.
- Input: fork-owned tracer at `49bc4219`; community checkpoint codec read as design reference only, no community source copied.
- Engine hooks honor `allow_prefix_reuse`, so startup warmup never attempts import. Other architecture Programs are not given a fake persistence implementation.
- Launcher confines containers to holder `A2` and loopback port `18120`; receipts contain names, hashes and counters, not host paths, model text or credentials.
- G1 candidate `e442df4d` compiled successfully: 627 Ninja steps, lease `20261025T010431Z-A2-tracer-build-e442df4d-210506`, exit 0, 192 seconds, zero leftover/unowned containers. Binary SHA-256 `da54f5a633527515f78eef98f9c967f99b176a9036cd2097098aa9bfd9f362cd`; source archive SHA-256 `ac994248f3396b13a86411d656520752f0329c01c4827d0061ab31000d7f8bd6`.
- Initial restart job `20261025T011218Z-A2-tracer-restart-e442df4d-218677` exited before health with `cudaErrorStubLibrary`; this is an invalid environment, not an engine G1 failure. Corrected image `ov-build-env:master-64d10fbf-r2`, SHA-256 `e7f34ca2f41ecea14da5cce0b2a30cae6b0877e3b3eb371bd92d103bca1121f1`, is GPU-qualified by Infra.
- Corrected-image G1 job: `20261025T012810Z-A2-tracer-restart-e442df4d-r2-245426`; verdict pending.
- Phase 2 uses frozen Contracts `746b29d2`, streaming/crypto prerequisites `0789709d` and `cb89c89a`. Native CBOR is field-wise and bounded; typed canonical round-trip rejects unknown fields, narrowing, duplicates and noncanonical containers.
- Native set export retains checkpoint, state and page leases, resolves views while serialized, then copies/hashes/writes through a dedicated stream and bounded pinned staging outside Engine serialization. Padding and uncommitted KV columns are zeroed. K2 layout derives from immutable geometry, not allocation capacity.
- Host-only import reserves StateImages, logical pages, extents, inactive histories and checkpoint slots; payload descriptors and delivered-byte verification precede native publication. Prepared reservations abort through RAII; capacity refusal is separate from corruption. Endpoint/input/anchor records preserve exact text/media identity and DFlash2 ring/frontier metadata.
- Added synthetic backing ownership/rollback tests, canonical codec failure tests and an actual separate-process real-model export/import test. Phase 2 candidate `19043a49` was submitted as lease job `20261025T014145Z-A2-native-19043a49-287234`; results are not yet available, so this branch is not merge-qualified. That job does not contain the later isolated replay-reservation fix.
- Release-blocking upstream [#419](https://github.com/Neroued/ninfer/issues/419): `write_pages` quoted growth against pre-rewind membership, while `prepare_write` first trims/forks to `begin`. The fix quotes post-trim membership and retains the strict write-side reservation guard. Regression: rewind a 65-token history to 8, then write through 65; one new page is required, plus one tail-COW page when the old endpoint remains retained. The previous quote omits that growth page. Regression execution remains pending; this does not yet establish resolution of the separate concurrent-prefix report [#414](https://github.com/Neroued/ninfer/issues/414).

## Commands

Host directory arguments below use the RUNBOOK variables `A2_SCRIPT_ROOT`, `A2_WORK_ROOT` and `V3_MODEL`; this keeps host paths out of committed evidence.

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
```
