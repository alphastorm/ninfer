# Native architecture enablement: C2 final report

## Outcome and source boundary

**All requested C2 technical gates pass on both native profiles, including real-model server decode at 131,072 context/KV, INT8, MTP3 and C1. The approved admission-headroom amendment resolves the measured startup refusal: 640 MiB on the 4090 and 768 MiB on the 3090, with calibration checks preserved. Decode measured 137.3510 / 78.3319 tokens/s respectively. These are C2 diagnostic proofs, not integrated native release qualification.**

- Branch: `rebase/arch-enable`, continued from `6063e9d9`; final implementation revision: `caa110f7456c2f0d28de6e5ed3f9068c0e3c5706`. Subsequent report/receipt commits do not change compiled source.
- Linux sm_86 and sm_89 both link the final implementation: server, real-model loader, architecture contract and all 92 ops-inventory targets. No GPU execution occurred on the Linux CPU build lane.
- Windows ops evidence is bound separately: sm_86 `47ebc3824b8c32112458eb013e5d4bb20c8b98f6`; sm_89 `a709f895db658c86b210efff3a56af31bfb28ef1`. Final Windows servers are both `caa110f7456c2f0d28de6e5ed3f9068c0e3c5706`. The last change affects startup admission and its serve option, not kernel arithmetic or selected-weight loading. Old manifests have not been relabeled.
- Both GPU windows are released. Final observation found no owned server or probe listener. No installed native root, shipped model, console session, firewall or production port was changed.

## Final gating table

| Gate | RTX 4090 / sm_89 | RTX 3090 / sm_86 | Evidence |
|---|---|---|---|
| Final-source Linux links | PASS | PASS | `linux-sm89-build.json`, `linux-sm86-build.json` |
| MSVC ops/test binary set | PASS: 97 binaries, 92 ops targets | PASS: 97 binaries, 92 ops targets | `windows-sm89-a709-build.json`, `windows-sm86-build.json` |
| Final-source MSVC server link | PASS | PASS | `windows-sm89-headroom-server.json`, `windows-sm86-headroom-server.json` |
| Admission-headroom option unit test | PASS: 1/1 | PASS: 1/1 | Final Windows build receipts; only `ninfer_serve_options_test` executed |
| DFlash2 link, included in server model libraries | PASS | PASS | Final Linux and MSVC server links; no DFlash2 execution claim |
| Ops inventory execution | 91 PASS + 1 authorized exclusion | 91 PASS + 1 authorized exclusion | `sm89-tests.json`, `sm86-tests.json` |
| Architecture contract | PASS, included in the 92-target inventory | PASS, included in the 92-target inventory | Exact refusal checks in test receipts |
| Artifact materialization | PASS | PASS | Per-GPU test receipts |
| Expert cache | PASS | PASS | Per-GPU test receipts |
| PLE prepare GPU harness | PASS | PASS after correcting the operator's Python launcher quoting | `sm89-tests.json`, `sm86-ple-r3.json` |
| Selected-MTP materialization / no DFlash2 residency | PASS | PASS | 17,901,798,400 device bytes; draft selected 0, draft weights 0 |
| Required real-model server decode | **PASS**, 640 MiB admission floor | **PASS**, 768 MiB admission floor | `sm89-decode.json`, `sm86-decode.json` |
| Metadata-only budget diagnosis | PASS: exact admission inequality reproduced | PASS: exact admission inequality reproduced | `planner-sm89.json`, `planner-sm86.json`; no model upload or kernels |
| Server UUID / effective startup profile / residency equality | PASS | PASS | Startup attestation matched each requested physical GPU; server weight bytes equal selected-MTP loader |
| Decode tokens/second / actual prompt and completion tokens | 137.3510 tok/s; 86 / 256 tokens | 78.3319 tok/s; 86 / 256 tokens | One bounded real-model decode per card; not a 131,072-token prompt |
| Optional DFlash2 load-only check | Not run | Not run | Not a C2 acceptance requirement |

All receipt filenames above are under [`receipts/native-architecture/`](receipts/native-architecture/).

## Execution families and explicit exclusions

| Execution family | sm_89 | sm_86 |
|---|---|---|
| BF16/F16/INT8 MMA; Q4/Q5/Q6/Q8 A16 | Numerical suites PASS | Numerical suites PASS |
| Dense BF16 TMA-selected tiles | Native cp.async route PASS | Native cp.async route PASS |
| FP8 activation MMA and TMA-selected tiles | Ada ordinary/full-K MMA PASS, including corrected FP8 SwiGLU | Explicit unsupported A8 refusal PASS |
| FP8 and NVFP4 A16 weight dequantization | Numerical suites PASS | Numerical suites PASS |
| Dense BF16/INT8 KV attention | Numerical suites PASS | Numerical suites PASS |
| Dense FP8 KV attention | Numerical suites PASS | Explicit unsupported refusal PASS |
| NVFP4 A4 / NVFP4 KV / K8V4 attention | Explicit unsupported refusals PASS | Explicit unsupported refusals PASS |
| Block-scaled FP8/NVFP4 MoE | Explicit exclusion | Explicit exclusion |
| Groupwise-integer MoE and CPU experts | Numerical suites PASS | Numerical suites PASS |
| Striped gated-residual projection | Folded cp.async route PASS | Folded cp.async route PASS |
| Programmatic dependent launch | Native ordinary-stream route exercised by supported suites | Native ordinary-stream route exercised by supported suites |

The only whole-test exclusion is `ninfer_moe_block_scaled_test`, exit 77 with the exact message `EXCLUDED block-scaled MoE (requires sm_120a; refusal checked by architecture_contract)`. No arbitrary skip, nonzero exit or CUDA failure is accepted. Supported cases within mixed-format tests still run. Both profiles exclude NVFP4 A4, NVFP4 KV, K8V4 KV and block-scaled MoE paths; sm_86 additionally excludes FP8 A8 and FP8 KV. Every observed case-level exclusion is retained in the per-test JSON. `tests/ops/excluded_paths.h` remains the exact-message authority. No oracle tolerance or supported case was removed.

## Artifact and selected-component residency

The only qualification artifact is published v3 `81f924d440c27261d820c19a9f8d45794c5aee410f8a68bd358133fa8c0375da`, **20,437,521,664 bytes**, from `neroued/Qwen3.8-27B-NInfer` revision `1cbd84e7221e51186bd7f093a149912d2489625b`. The failed partial download was removed. A complete verified source was copied over the local network; the destination's full SHA-256 and length matched before promotion. See `artifact-copy.json`.

On each physical GPU the real loading test passed with:

| Selected-MTP observation | Bytes / count |
|---|---:|
| Device capacity | 17,901,798,400 B |
| Host capacity | 0 B |
| Host-to-device transfer | 17,901,792,256 B |
| Resident parents | 785 |
| MTP selected | 1 |
| Draft selected / draft-named weights | 0 / 0 |

The loader checks all resident parents are reachable from selected weights. Server `Startup` uses `load_options(options)` for both `plan_load` and materialization; its resident-weight estimate is the selected plan's `placement(device, device).device_capacity_bytes` (`src/models/qwen3_5/startup.cpp:9-30`). Both amended server startups reported exactly **17,901,798,400 resident weight bytes**, **17,901,792,256 H2D bytes** and **785 device objects**. The decode runner asserted server weight-byte equality with the selected-MTP loader before generation. Together with the loader's zero selected draft tensors, this satisfies the no-DFlash2 residency assertion.

## Baseline refusal and measured budget attribution

Before the approved headroom amendment, both `5e2210ae6` server attempts emitted:

```text
ERROR startup failed | planning runtime | 836 ms
FATAL server failed during startup | Device budget cannot hold model weights and required execution resources
```

The baseline per-card driver exited 1. Its PowerShell server object did not retain a native exit code, so the native exit code is recorded as unknown, not invented. The stderr establishes the actual failure. Both baseline attempts retained INT8 KV, MTP3, optimized proposal head, C1 and 131,072 configured context/KV capacity. Their original blocker receipts remain historical evidence; neither baseline attempt became ready. Successful amended startup and decode evidence is recorded separately below.

Source attribution at `5e2210ae6`:

- `src/runtime/engine/model_startup.h:216-228` rejects the selected weights plus initial execution candidate before model materialization, graph calibration or graph preparation.
- `src/models/qwen3_5/startup.cpp:32-49` returns no candidate when the requested capacity curve's reservation exceeds available runtime bytes.
- The reservation is persistent state plus execution workspace (`program/planning/startup.cpp:839-850`). The upstream startup budget withholds **1,073,741,824 B** of headroom even for explicit KV capacity (`include/ninfer/types.h:47`, `runtime/engine/model_construction.cpp:112-114`). No `NINFER_VRAM_BUDGET_MIB` override was found in the queried process, user or machine environment.
- The generic server error does not emit its budget breakdown. A subsequently authorized metadata-only diagnostic executed the existing selected-weight plan and `make_sequence_planner_impl` at the same implementation revision on each physical GPU. At C1 the minimum and maximum page count both equal the requested 131,072-token capacity. These later measurements reproduce the admission inequality; they are not invented telemetry from the original server attempts.

| Planner term | RTX 4090 | RTX 3090 |
|---|---:|---:|
| CUDA free before planning | 24,159,191,040 B | 24,468,520,960 B |
| Selected model weights | 17,901,798,400 B | 17,901,798,400 B |
| Persistent runtime | 5,200,372,480 B | 5,497,795,328 B |
| — text KV / MTP KV | 4,429,185,024 / 276,959,232 B | 4,429,185,024 / 276,959,232 B |
| — device StateImage pool | 461,862,912 B, 3 total slots | 769,771,520 B, 5 total slots |
| — replay records | 7,151,616 B | 7,151,616 B |
| — prefill hidden / other persistent | 20,971,520 / 4,242,176 B | 10,485,760 / 4,242,176 B |
| Execution workspace, prefill peak | 207,896,576 B | 103,948,288 B |
| Total runtime reservation | 5,408,269,056 B | 5,601,743,616 B |
| Weights plus runtime | 23,310,067,456 B | 23,503,542,016 B |
| Slack before mandatory headroom | 849,123,584 B | 964,978,944 B |
| Mandatory startup headroom | 1,073,741,824 B | 1,073,741,824 B |
| Available runtime after weights and headroom | 5,183,650,816 B | 5,492,980,736 B |
| **Admission shortfall** | **224,618,240 B** | **108,762,880 B** |

**Attribution: a genuine upstream admission-policy change, not K5 translation.** Shipped `cad19767:src/runtime/engine/kv_capacity.cpp:85-98` subtracts headroom only for automatic capacity; explicit capacity carries zero automatic headroom. At `5e2210ae6`, `model_construction.cpp:112-114` imposed the 1 GiB floor even for explicit capacity. The unchanged native profiles' selected weights plus runtime fit the measured free bytes, but their remaining slack is less than this newly mandatory floor. The selected planner has zero DFlash2 persistent state and zero DFlash2 workspace; selected weights match both real-loader measurements. The ordinary execution workspace is only about 198/99 MiB, and the baseline refusal occurred before CUDA graph calibration.

The two metadata probes exited 0 in 1.343/1.342 seconds, sequentially, without model materialization or kernel execution. Their source/hook hashes, binary hashes, successful target-only builds, CMake injection removal and complete component breakdown are retained in `planner-sm89.json` and `planner-sm86.json`. Both cards were released afterward. No profile was changed during diagnosis. The lead accepted this cause and subsequently authorized the explicit K5 floor amendment; the successful amended runs below retain the original context, KV precision, MTP window, concurrency, state-slot and prefill settings.

The slot-translation suspicion was checked against both sources. Shipped `targets/qwen3_6/impl/runtime/layouts_impl.h:112-114` and current `models/qwen3_5/program/planning/startup.cpp:83-85` both add concurrency to configured state slots. Changing 2 to 1 or 4 to 3 would therefore reduce the agreed capacity, not correct a translation error.

| Profile | Configured / total device StateImages | StateImage pool bytes | Prefill | Former host KV MiB | Host StateImages | Unified host bytes / exact MiB |
|---|---:|---:|---:|---:|---:|---|
| sm_89 | 2 / 3 | 461,862,912 | 2048 | 11264 | 24 | 15,506,063,360 / 14787.734375 |
| sm_86 | 4 / 5 | 769,771,520 | 1024 | 8192 | 24 | 12,284,837,888 / 11715.734375 |

One native StateImage is 153,954,304 B: `48 * (10240 * 3 * 2 + 128 * 128 * 48 * 4) + 5120 * 2`. The amended startup receipts confirm the translated slot counts and exact unified host-byte budgets.

## Approved amendment and successful real-model decodes

`caa110f74` adds `--admission-headroom-mib N`, an integer floor of at least 256 MiB. Its optional `EngineOptions::admission_headroom_bytes` replaces only the default operand of the existing headroom maximum. Omission retains 1 GiB, including the unchanged sm_120a profile. Larger automatic-capacity headroom, graph calibration, measured rebuild reservations and allocation-fit checks remain enforced. The floor is resource policy, not K2 identity.

The option unit test covers omission, exact 256/640/768/1024 MiB conversion, invalid values, missing value, overflow, help text and preservation of the independent automatic-capacity lower bound. Both native builds ran only this focused test, 1/1 PASS each. All 26 executable/DLL payloads in each transferred amended package matched their SHA-256 and byte length before execution.

Exactly one amended server decode ran per card, sequentially. Both startup receipts attest the requested physical GPU with numeric device selection, `CUDA_DEVICE_ORDER=PCI_BUS_ID` and no visibility mask. Both report **131,072 context and KV capacity, INT8-group64, MTP3, optimized proposal head and C1**. The 86-token prompt generated 256 completion tokens on each card; this is not a full-context prompt or a throughput benchmark corpus.

| Amended startup / decode measurement | RTX 4090 | RTX 3090 |
|---|---:|---:|
| Configured / effective admission floor | 640 MiB / 671,088,640 B | 768 MiB / 805,306,368 B |
| Runtime reservation | 5,408,269,056 B | 5,601,743,616 B |
| Planned slack | 168,058,112 B | 149,695,744 B |
| CUDA free after startup | 742,322,176 B | 953,155,584 B |
| **CUDA spare after admission floor** | **71,233,536 B** | **147,849,216 B** |
| Graph calibration increment / retained | 8,388,608 / 8,388,608 B | 8,388,608 / 8,388,608 B |
| Graph preparation increment / rebuild estimate | 0 / 0 B | 0 / 0 B |
| Decode throughput | **137.351036 tok/s** | **78.331888 tok/s** |
| Prompt / completion tokens | 86 / 256 | 86 / 256 |
| Draft proposals / accepted | 231 / 177 | 231 / 177 |

`sm89-decode.json` and `sm86-decode.json` retain the full startup memory breakdown, configured/observed profile and attestation-match boolean without physical UUIDs. Each owned server was stopped after its decode. Final observation found no server process or isolated-port listener; idle used memory was 939 MiB / 322 MiB. These idle readings are separate from the CUDA startup budget measurements above.

## Corrections and verification provenance

1. Imported PortableCore's final static-linkage, host-compiler and UTF-8 boundary fixes (`b1f99a76b`, `679c063f4`, `6661b7d03`) as `370fd69df`, `190aedc54`, `f390abf20`. Before subsequent C2 fixes, the compiled-source difference between `f390abf20` and combined Windows `47ebc382` was only `REPORT.md`.
2. **`a709f895d`: native FP8 producer-warp correction.** The ordinary-MMA adapter inherited 32 TMA producer threads although its launch has no producer warp. The SwiGLU collective epilogue consequently subtracted nonexistent threads and accessed invalid indices. Explicit zero producer threads fixes the adapter. The original complete FP8 SwiGLU oracle now passes, including negative-gate and graph cases. The initial invalid-exit-capture run and its real kernel error remain in `sm89-initial-diagnostic.json`.
3. **`5e2210ae6`: runtime capability correction.** Qwen startup still required compute capability 12.0. It now requires exactly the compiled single-profile capability, 8.6, 8.9 or 12.0; mismatched devices still fail. Both baseline servers passed this boundary and reached the separate memory-budget rejection.
4. The operator runner now retains a directly constructed .NET process rather than relying on PowerShell's null `Start-Process.ExitCode`. Host-only probes captured exact exits 0 and 77. The corrected sm_86 PLE invocation removed inappropriate quoting of the Python launcher version switch; its separate successful receipt supersedes only that failed harness invocation.
5. Final Linux proof at `caa110f74` used the CPU-only CUDA 13.1.2 development image, 12 workers and the shared two-slot lock: both profiles and all 92 ops targets linked, with `C2_NATIVE_COMPILE_EXIT=0`. An earlier disk-full link failure was not treated as a compiler defect; redundant owned transfer artifacts were reclaimed. Final Windows server and focused options-test links used CUDA 13.3 / MSVC 14.44.35112; the options test passed on both profiles.
6. **`caa110f74`: approved admission floor.** Public/serve option storage, parser/help, engine forwarding, minimum validation and floor selection are the only implementation boundaries changed. Existing ops evidence retains its original source identity; amended server and focused unit evidence is separate.

Principal executed commands, with private locations and physical identities replaced by variables:

```sh
common/build-wsl.sh "$SOURCE" "$BUILD" -DCMAKE_CUDA_ARCHITECTURES=89 -DBUILD_TESTING=ON -- ninfer-serve ninfer_architecture_contract_test ninfer_qwen3_5_loading_real_test
# Same command for 86; both then build every target enumerated by:
ctest --test-dir "$BUILD" -L ops --show-only=json-v1
```

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File run-gpu-chain.ps1 -Arch 86 -Attempt r2
powershell.exe -NoProfile -ExecutionPolicy Bypass -File run-gpu-chain.ps1 -Arch 89 -Attempt r3 -SourceCommit a709f895db658c86b210efff3a56af31bfb28ef1 -TestsOnly
py.exe -3 tests/models/qwen4_exp/ple/disk/test_prepare.py $PleExecutable --gpu
powershell.exe -NoProfile -ExecutionPolicy Bypass -File decode-profile.ps1 -Arch 89
powershell.exe -NoProfile -ExecutionPolicy Bypass -File decode-profile.ps1 -Arch 86
```

The two `decode-profile.ps1` commands above are the retained baseline attempts. Final amended decodes used:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File decode-native.ps1 -Binary $SERVER89 -Artifact $MODEL -GpuUuid $GPU89 -Device 0 -Port 18200 -PrefillChunk 2048 -DeviceStateSlots 2 -HostContextMiB 14787.734375 -AdmissionHeadroomMiB 640 -MtpLoadEvidence $MTP89 -Evidence $EVIDENCE89
powershell.exe -NoProfile -ExecutionPolicy Bypass -File decode-native.ps1 -Binary $SERVER86 -Artifact $MODEL -GpuUuid $GPU86 -Device 1 -Port 18201 -PrefillChunk 1024 -DeviceStateSlots 4 -HostContextMiB 11715.734375 -AdmissionHeadroomMiB 768 -MtpLoadEvidence $MTP86 -Evidence $EVIDENCE86
```

Ops executables used an authorized full-UUID visibility mask. Servers used numeric devices, `CUDA_DEVICE_ORDER=PCI_BUS_ID`, no visibility mask and only the assigned isolated ports. Startup UUID attestation was a required readiness assertion and passed in both amended runs.

## Delivery and containment

- Content-safe receipts preserve separate build, numerical, residency, historical refusal and successful amended-decode scopes. Both cards satisfy the requested C2 technical gates. Integrated native release qualification is not claimed.
- Current artifact staging was moved to the approved data drive. The relocation's verified archive and removed extracted duplicate reclaimed **54,018,305,491 B** from the system drive; `staging-relocation.json` records the result. Earlier superseded owned packaging copies and the failed partial archive were also removed. Referenced build binaries, receipts, verified models and all other holders' scratch were preserved.
- The integration owner must include the NativeOps DirectStorage root correction `87ae8085a` and the full provider union in its three-binary native package, then run `ckpt-native` and integrated qualification. C1+C2 diagnostic proof is not a substitute. The founder-approved release split remains unchanged: v0.13.0 carries the v0.12.2 native packages; rebased natives follow in v0.13.1 with the explicit approved headroom settings.
