# Native architecture enablement

## Scope and invariants

- Base: upstream master `64d10fbf`; branch `rebase/arch-enable`. Single-profile builds: `86`, `89`, or `120a`; the default remains `120a`. Shared root profile wiring is Integrator commit `15a4f2a78` (applied here as `83b1b683`).
- Native qualification target: dense Qwen3.8 27B text, INT8 KV, 131,072-token capacity, MTP with three draft tokens, one request. DFlash2 must link; fitting its weights is not claimed by this slice.
- Program, scheduler, context ownership, and native packaging are unchanged by this slice. Windows host portability is supplied by PortableCore. Release review is the lead-owned G4 gate.

## Gating table

| Execution family | sm_120a | sm_89 route / exclusion | sm_86 route / exclusion | Native proof status / evidence |
|---|---|---|---|---|
| BF16/F16/INT8 MMA and Q4/Q5/Q6/Q8 A16 | Existing routes | Retained | Retained | sm_86 server link `980b4966`; final ops numerical proof pending |
| Programmatic dependent launch | Existing behavior | Ordinary stream ordering | Ordinary stream ordering | sm_86 server link `980b4966`; GPU ordering coverage pending |
| Dense BF16 TMA-selected tiles | Existing TMA | cp.async staging of the same MMA tiles | cp.async staging of the same MMA tiles | MSVC unused-definition failure; `ca85f257` gate awaiting rebuild |
| FP8 activation MMA | Block-scaled unit-scale opcode | Plain Ada E4M3 MMA | Explicit refusal | `b5bd4926` explicit-row-policy rebuild in flight; refusal test pending |
| FP8 TMA-selected tiles | Existing TMA/split-K | Ordinary full-K MMA; no split-K partial allocation | Explicit refusal | `b5bd4926` rebuild in flight; numerical/refusal proof pending |
| FP8 and NVFP4 A16 weight dequantization | Existing routes | Retained; portable widening | Retained; portable widening | sm_86 server link `980b4966`; A16 mathematical-oracle runs pending |
| NVFP4 A4 | Existing non-RDC/TMA and MMA | Explicit refusal | Explicit refusal | `architecture_contract` and explicit A4 suite exclusions; execution pending |
| Dense BF16/INT8 KV attention | Existing routes | Retained | Retained | sm_86 server link `980b4966`; softmax-attention numerical runs pending |
| Dense FP8 KV attention | Existing routes | Retained | Explicit refusal | sm_89 numerical and sm_86 `architecture_contract` runs pending |
| Dense NVFP4/K8V4 KV attention | Existing routes | Explicit refusal | Explicit refusal | `architecture_contract` and exact attention refusal messages; execution pending |
| Block-scaled FP8/NVFP4 MoE | Existing non-RDC routes | Explicit refusal | Explicit refusal | `architecture_contract`; only named block-scaled test exit 77 permitted |
| Groupwise-integer MoE and CPU experts | Existing routes | Retained | Retained | PortableCore `ed99ff3f` passed former MSVC compilation blockers; numerical proof pending |
| Striped gated-residual projection | Existing TMA selection | Existing folded cp.async route | Existing folded cp.async route | sm_86 server link `980b4966`; gated-residual numerical runs pending |

Statuses above describe route admission, not numerical qualification. Current evidence
for every retained family is the sm_86 server/contract link at `980b4966`; final-tip
Linux ops links, native MSVC links and per-GPU mathematical-oracle runs remain pending.
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
  The two-profile Linux server/contract/ops build at this revision is in flight.
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

## Provenance and remaining gates

- Behavioral references: shipped native fork and the upstream architecture/kernel contracts. Community ports were design references; no community source file was copied.
- Required remaining proof: final-revision Linux compilation for both native profiles, MSVC link for both profiles, ops suites on their respective physical GPUs with the explicit exclusions, and one server-path model decode on each GPU with tokens/second and configured/measured context recorded. The assignment is not acceptance-complete.
- Native runs use isolated work directories and loopback ports `18200–18209`, never installed release roots or the shipped port.
- The lead additionally authorized `ninfer_artifact_materialization_test`, `ninfer_expert_cache_test`, and the PLE prepare GPU Python harness on each native GPU. PortableCore supplies the executables, DLLs, stdlib-only Python inputs, and the ops manifest. Python 3.13.15 was observed on the native runtime host.
- PortableCore reports its native candidate (including architecture source `940b000c`) failed compilation in CPU expert code on GNU-only unreachable/F16C intrinsics and ISA flags. The lead assigned those minimal `src/ops/moe` MSVC fixes to PortableCore; this slice leaves those files unchanged. The immutable native artifact's Windows export is also still awaiting its queued job receipt.
- Test-only full-UUID `CUDA_VISIBLE_DEVICES` masking is explicitly authorized because upstream tests use CUDA ordinal zero. Receipts record the mask; skip 77 is accepted only for the named block-scaled MoE test with its exact exclusion message. Server probes instead leave that variable absent, use `CUDA_DEVICE_ORDER=PCI_BUS_ID` plus numeric `--device`, and require the server-start GPU UUID to match.
- No native ops execution, real-model decode, throughput measurement, or native release qualification is claimed in this report.
