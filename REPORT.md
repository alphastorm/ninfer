# Native architecture enablement

## Scope and invariants

- Base: upstream master `64d10fbf`; branch `rebase/arch-enable`. Single-profile builds: `86`, `89`, or `120a`; the default remains `120a`. Shared root profile wiring is Integrator commit `15a4f2a78` (applied here as `83b1b683`).
- Native qualification target: dense Qwen3.8 27B text, INT8 KV, 131,072-token capacity, MTP with three draft tokens, one request. The lead clarified that DFlash2 acceptance is **link-only** on both native profiles; fitting its weights belongs to S2. A load-only check using the public v3 artifact is optional and cannot block C2.
- The lead's final artifact decision binds **all three lanes** to public v3
  `81f924d440c27261d820c19a9f8d45794c5aee410f8a68bd358133fa8c0375da`
  (20,437,521,664 bytes), repository `neroued/Qwen3.8-27B-NInfer`,
  revision `1cbd84e7221e51186bd7f093a149912d2489625b`.
  The older native `5476dfd6` conversion is no longer the qualification input.
  Native MTP selection must not materialize unused DFlash2 weights.
- Program, scheduler, context ownership, and native packaging are unchanged by this slice. Windows host portability is supplied by PortableCore. Release review is the lead-owned G4 gate.

## Gating table

| Execution family | sm_120a | sm_89 route / exclusion | sm_86 route / exclusion | Native proof status / evidence |
|---|---|---|---|---|
| BF16/F16/INT8 MMA and Q4/Q5/Q6/Q8 A16 | Existing routes | Retained | Retained | L1 linked both profiles; numerical proof pending |
| Programmatic dependent launch | Existing behavior | Ordinary stream ordering | Ordinary stream ordering | L1 linked both; GPU ordering coverage pending |
| Dense BF16 TMA-selected tiles | Existing TMA | cp.async staging of the same MMA tiles | cp.async staging of the same MMA tiles | W1 passed corrected TMA/launcher boundary; numerical proof pending |
| FP8 activation MMA | Block-scaled unit-scale opcode | Plain Ada E4M3 MMA | Explicit refusal | L1 linked both with explicit row policy; numerical/refusal proof pending |
| FP8 TMA-selected tiles | Existing TMA/split-K | Ordinary full-K MMA; no split-K partial allocation | Explicit refusal | L1 linked both; numerical/refusal proof pending |
| FP8 and NVFP4 A16 weight dequantization | Existing routes | Retained; portable widening | Retained; portable widening | L1 linked both; A16 oracle runs pending |
| NVFP4 A4 | Existing non-RDC/TMA and MMA | Explicit refusal | Explicit refusal | L1 linked contract/exclusions; execution pending |
| Dense BF16/INT8 KV attention | Existing routes | Retained | Retained | L1 linked both; W1 linked corrected sm_89 tests; numerical proof pending |
| Dense FP8 KV attention | Existing routes | Retained | Explicit refusal | L1 linked both; sm_89 numerical / sm_86 refusal execution pending |
| Dense NVFP4/K8V4 KV attention | Existing routes | Explicit refusal | Explicit refusal | L1 linked contract/exclusions; execution pending |
| Block-scaled FP8/NVFP4 MoE | Existing non-RDC routes | Explicit refusal | Explicit refusal | L1 linked contract; only exact named block-scaled exit 77 permitted |
| Groupwise-integer MoE and CPU experts | Existing routes | Retained | Retained | L1 linked both; W1 passed former MSVC CPU blockers; numerical proof pending |
| Striped gated-residual projection | Existing TMA selection | Existing folded cp.async route | Existing folded cp.async route | W1 passed corrected launcher boundary; numerical proof pending |

Statuses above describe route admission, not numerical qualification.
**L1:** Linux `9ee8a29cb87524410238b3d964dd84af546d8f2b` linked `ninfer-serve`,
loading-real, architecture contract and all 92 ops-inventory targets for both
profiles; combined driver and per-profile receipts exited 0. This supersedes the
earlier successful `53e847dd` links but predates the later launcher/test corrections.
**W1:** PortableCore2 reports successful combined MSVC sm_89
`47ebc3824b8c32112458eb013e5d4bb20c8b98f6` (C1 `6661b7d03` plus C2 `363ccccc`):
97 binary targets, including all 92 ops-inventory targets and `ninfer-serve`.
Its earlier `2a4266e30` attempt failed in the now-corrected ops test sources.
Final-tip Linux links, Windows sm_86 links and all per-GPU oracle runs remain pending.
The explicit exclusion authority is `tests/ops/excluded_paths.h`; its refusal messages,
not arbitrary failures or skips, determine unsupported cases.

The shared non-RDC archive exists only for `120a`. Each excluded family supplies current-signature throwing entrypoints. Unsupported activation plans never silently substitute A16 arithmetic. The public Engine-options rejection of unsupported native KV formats is EngineCache-owned.

The upstream Q8 family already selects dynamic shared memory above 48 KiB and calls `cudaFuncSetAttribute`; it is retained rather than transplanting the fork-local W8 kernels. Dense BF16/INT8 attention also already has explicit dynamic-shared-memory launch support.

## Test coverage and exclusions

- `tests/ops/excluded_paths.h` lists exact permitted architecture refusal messages; arbitrary CUDA/runtime errors are not exclusions.
- `ninfer_architecture_contract_test` exercises the native activation, attention, and block-scaled MoE refusal entrypoints. It must fail if an excluded entrypoint returns normally.
- Linear, LinearAdd, SwiGLU, segmented projection, GDN record/snapshot, and dense attention retain supported numerical cases. Unsupported A4/A8 arms are named explicitly. Existing accumulated failures are preserved. A16 cases cannot be accepted as architecture refusals.
- Native GDN snapshot coverage includes large-T A16 cases instead of asserting an unsupported activation mode executes. Block-scaled MoE is reported as excluded, not numerically qualified.
- No numerical tolerance was relaxed. No GPU correctness or performance pass is claimed until the native binaries are exercised.

## Commands and observed evidence

Working-directory prefixes are omitted for privacy; `common/build.sh` is the program-provided build helper. Initial builds used holder `C2`, priority 2, a 20-minute lease, and persistent architecture-specific build directories. After the lead restricted the primary lane to critical work, the next queued compile was cancelled and moved to the secondary CPU-only lane.

```sh
common/build.sh "$SOURCE" "$BUILD/native-sm86" \
  -DCMAKE_CUDA_ARCHITECTURES=86 -DBUILD_TESTING=ON -- \
  ninfer-serve ninfer_architecture_contract_test
common/build.sh "$SOURCE" "$BUILD/native-sm89" \
  -DCMAKE_CUDA_ARCHITECTURES=89 -DBUILD_TESTING=ON -- \
  ninfer-serve ninfer_architecture_contract_test
```

- Initial compile attempt (`20261025T010734Z-C2-arch-86-89-compile-212431`) failed during CMake configuration after 4 seconds: split projection and GDN input-convolution still registered sources against the omitted non-RDC archive. Both family manifests and their current-signature rejection entrypoints were corrected in `980b4966`.
- The next lease (`20261025T011530Z-C2-arch-86-89-compile2-223772`) ended after 1 second before configuration: a shallow branch clone did not contain the pinned parent revision. The driver now explicitly fetches the full 40-character commit before detaching; this was a source-staging failure, not compiler evidence.
- Compile lease `20261025T012416Z-C2-arch-86-89-compile3-239028` used candidate `980b4966ff0b002632c1f74857fa510be23ecc64`. **sm_86 compiled and linked `ninfer-serve` and `ninfer_architecture_contract_test`**, completing all 619 build steps. The build compiled the upstream DFlash2 loader and linked the server with its model libraries. No GPU execution was performed.
- The same lease then failed the sm_89 build: the native FP8 forward declaration did not carry the default row policy, and NVCC's host compilation rejected four-argument callers. The first correction placed defaults on the first declaration. Combined lease runtime was 259 seconds.
- Windows links, sm_89 completion, and native GPU qualification are not yet proven.
- Native v3 artifact supplied by Infra: SHA-256 `5476dfd6b243332f1754a494469ba4a1c8206edba6518b61dba1e0f4338664e7`, 18,210,749,936 bytes, 1,118 tensors. Infra reports v3 framing/directory validation; this is not a model-decode proof.
- The secondary CPU build uses source `940b000c365887a8f4c3ad03a4a039e92039f396`, image `ov-build-env:master-64d10fbf-r2-cpu-local`, 12 workers, and the shared two-slot lock. Its driver builds `ninfer-serve` and the architecture-contract target, obtains the exact ops target names with `ctest -L ops --show-only=json-v1`, and builds those targets for `89`, then `86`. It does not execute GPU tests in that CPU-only image.
- That secondary attempt **failed** during sm_89 compilation: NVCC host emission reported a repeated `Rows` default despite the source definition having none. The final correction removes both launcher defaults entirely and explicitly passes the unchanged `Fp8IdentityRows{}` policy at every formerly four-argument caller (projection, residual add, both split projections, and GDN). Custom-row callers were already explicit. This correction has **not been rebuilt**; the user requested handoff after the current step. No ops target batch or sm_86 rebuild ran after this failure.

## Continued final-candidate qualification

- Imported PortableCore's `ed99ff3f` compiler-neutral CPU expert changes as
  `53e847dd`: unreachable intrinsic, F16C scalar conversion and isolated MSVC ISA flags.
  At this revision, Linux sm_89 linked the server and architecture contract
  (427/427 incremental tasks), then completed its ops target batch. Linux sm_86
  also linked the server and architecture contract (619/619 tasks). Both ops
  target batches completed all 228 tasks; the driver reported
  `C2_NATIVE_LINUX_COMPILE_PASS` and exit zero for both profiles.
  The one-hour observer expired during the sm_86 ops links; the detached
  build remained alive and the reattached observer obtained its actual completion.
  This is intermediate-source evidence, not the final combined-candidate proof.
- PortableCore's combined MSVC sm_89 build at `ed99ff3f` passed the former CPU
  compiler blockers, then failed at step 98/1289 after 101.566 seconds:
  `bf16_a16_tma_mma.cuh:116`, C2719 on the 128-byte-aligned descriptor parameter.
  No native executable bundle was produced. The unused native-profile TMA
  descriptor/kernel definitions are now gated like the existing FP8 TMA definitions;
  the cp.async launcher and Blackwell implementation are unchanged. This correction
  still requires Linux and MSVC rebuild evidence.
- The real Qwen loader retains its complete artifact/resident-byte/input checks.
  Its peak-RSS observation now uses the native Windows process working-set query
  on Windows and the unchanged `getrusage` path on Linux, allowing the direct
  DFlash2 loading check to compile without a POSIX-only header.
  Its MTP loading arm also rejects materialized draft-named weights, reports
  selected MTP/draft components and draft-weight count, and retains the assertion
  that every resident parent is reachable from selected model weights. This
  covers both device and host parent placements; the decode receipt separately
  records actual startup weight, transfer, VRAM and host-capacity observations.
- Combined MSVC sm_89 candidate `3d2a3df4` passed the unused-TMA declaration
  boundary, then failed at 26/1161 tasks after 47.261 seconds: C2326 rejected
  outer `constexpr` kernel aliases referenced inside token-slicing lambdas.
  The observed instances were BF16 sliced-K and folded gated-residual Up.
  These launch sites and the analogous native FP8/NVFP4 A16 sliced-K sites now
  name the same kernel specialization directly inside the lambda; shared-memory
  setup stays outside the loop. Schedules, arguments and arithmetic are unchanged.
  The Blackwell-only NVFP4 A4 TMA launcher is outside the native build and unchanged.
- The preceding MSVC attempt exposed C++20 test portability errors: `constexpr std::sqrt` in attention
  and GDN references, and GCC-only `_Float16` in sparse-attention fixtures.
  These now retain the same square-root expressions as `const` values and reuse
  the existing independent round-to-nearest-even binary16 fixture conversions.
  No oracle tolerance or supported case was removed. W1 compiled these corrections.
  Integrator's isolated `252bb209d` correction for missing `<array>`
  in public `ninfer/types.h` and `<span>` in the generation-service header was
  imported; no test include-order workaround was added.

### Native host-pool translation

The shipped `cad19767` MTP StateImage layout contains 48 BF16 convolution histories,
48 FP32 recurrent states and one BF16 continuation-hidden vector, with no DFlash
local ring. Its size is
`48 * (10240 * 3 * 2 + 128 * 128 * 48 * 4) + 5120 * 2 = 153954304` bytes;
each region is already 256-byte aligned. Source authorities are
`qwen3_6_27b/impl/config.h`, `qwen3_6/impl/runtime/layouts_impl.h` and
`qwen3_6/impl/state/state_image.cpp` under the shipped native target tree.
The inspected copies have no differences from those paths at `cad19767`.

| Native profile | Former Host KV MiB | Host StateImages | Unified host bytes | Exact `--host-context-mib` | Extra / total device StateImages at C1 | Prefill chunk |
|---|---:|---:|---:|---:|---:|---:|
| sm_89 | 11264 | 24 | 15506063360 | 14787.734375 | 2 / 3 | 2048 |
| sm_86 | 8192 | 24 | 12284837888 | 11715.734375 | 4 / 5 | 1024 |

Both the shipped and upstream implementations add active concurrency to the
configured device-state slots; the values above preserve that behavior. These are
source-derived byte-equivalent budgets, not yet measured startup receipts. The
decode driver accepts exact decimal MiB and checks the observed startup fields.
Context 131072, INT8 KV, MTP3 and C1 are not tuned.
The native controller's existing `--lm-head-draft` flag is also preserved: the
selected-MTP loading probe and server use the optimized proposal head, not the
upstream default full head.

## Provenance and remaining gates

- Behavioral references: shipped native fork and the upstream architecture/kernel contracts. Community ports were design references; no community source file was copied.
- Required remaining proof: final-revision Linux compilation for both native profiles, MSVC sm_86 link, ops suites on their respective physical GPUs with the explicit exclusions, and one server-path model decode on each GPU with tokens/second and configured/measured context recorded. The assignment is not acceptance-complete.
- Native runs use isolated work directories and loopback ports `18200–18209`, never installed release roots or the shipped port.
- The lead additionally authorized `ninfer_artifact_materialization_test`, `ninfer_expert_cache_test`, and the PLE prepare GPU Python harness on each native GPU. PortableCore supplies the executables, DLLs, stdlib-only Python inputs, and the ops manifest. Python 3.13.15 was observed on the native runtime host.
- The earlier GNU-only unreachable/F16C and ISA-flag errors were corrected by
  PortableCore and imported here. The public v3 direct download timed out after
  1800 seconds with curl exit 28; its partial file is not qualified and has no
  whole-file SHA receipt. Infra2 independently reported a verified build-host copy.
  No native artifact upgrade is part of the current path.
- Test-only full-UUID `CUDA_VISIBLE_DEVICES` masking is explicitly authorized because upstream tests use CUDA ordinal zero. Receipts record the mask; skip 77 is accepted only for the named block-scaled MoE test with its exact exclusion message. Server probes instead leave that variable absent, use `CUDA_DEVICE_ORDER=PCI_BUS_ID` plus numeric `--device`, and require the server-start GPU UUID to match.
- No native ops execution, real-model decode, throughput measurement, or native release qualification is claimed in this report.

## Portable-core prerequisite evidence

- C1 history through `c796fcb7` is merged; subsequent core fixes and tests through
  `c7f127c7` were imported. Its earlier immutable report is
  [`REPORT.md` at `c796fcb762e1b6aaeb438d1ee9ad3f400379a304`](https://github.com/alphastorm/ninfer/blob/c796fcb762e1b6aaeb438d1ee9ad3f400379a304/REPORT.md).
- The earlier C1 `f689e6bde` result was 8/9. PortableCore2 subsequently reported
  **9/9 at combined `2a4266e30`, 2.45 seconds**, including anchored nonempty-directory
  publication/tombstone, junction refusal and retained-reader cases. This is
  native core evidence, not C2 numerical, model, or complete release qualification.
- For W1, PortableCore2 reported native core **9/9 (10.00 seconds)** and remaining
  host tests **43/43 (6.48 seconds)**, no failures/skips and no GPU execution.
  The server is 322383360 bytes, SHA-256
  `f7ec4d3426457bc5f9ccb706df369dfe032b94cd4a5e00a5dcfc8bcde8634c1d`.
