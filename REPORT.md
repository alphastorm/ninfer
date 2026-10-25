# Native architecture enablement

## Scope and invariants

- Base: upstream master `64d10fbf`; branch `rebase/arch-enable`. Single-profile builds: `86`, `89`, or `120a`; the default remains `120a`. Shared root profile wiring is Integrator commit `15a4f2a78` (applied here as `83b1b683`).
- Native qualification target: dense Qwen3.8 27B text, INT8 KV, 131,072-token capacity, MTP with three draft tokens, one request. DFlash2 must link; fitting its weights is not claimed by this slice.
- Program, scheduler, context ownership, and native packaging are unchanged by this slice. Windows host portability is supplied by PortableCore. Release review is the lead-owned G4 gate.

## Gating table

| Execution family | sm_120a | sm_89 | sm_86 |
|---|---|---|---|
| BF16/F16/INT8 MMA and Q4/Q5/Q6/Q8 A16 | Existing routes | Retained | Retained |
| Programmatic dependent launch | Existing behavior | Ordinary stream ordering | Ordinary stream ordering |
| Dense BF16 TMA-selected tiles | Existing TMA | cp.async staging of the same MMA tiles | cp.async staging of the same MMA tiles |
| FP8 activation MMA | Block-scaled unit-scale opcode | Plain Ada E4M3 MMA | Explicit refusal |
| FP8 TMA-selected tiles | Existing TMA/split-K | Ordinary full-K MMA; no split-K partial allocation | Explicit refusal |
| FP8 and NVFP4 A16 weight dequantization | Existing routes | Retained; portable widening | Retained; portable widening |
| NVFP4 A4 | Existing non-RDC/TMA and MMA | Explicit refusal | Explicit refusal |
| Dense BF16/INT8 KV attention | Existing routes | Retained | Retained |
| Dense FP8 KV attention | Existing routes | Retained | Explicit refusal |
| Dense NVFP4/K8V4 KV attention | Existing routes | Explicit refusal | Explicit refusal |
| Block-scaled FP8/NVFP4 MoE | Existing non-RDC routes | Explicit refusal | Explicit refusal |
| Groupwise-integer MoE and CPU experts | Existing routes | Retained | Retained |
| Striped gated-residual projection | Existing TMA selection | Existing folded cp.async route | Existing folded cp.async route |

The shared non-RDC archive exists only for `120a`. Each excluded family supplies current-signature throwing entrypoints. Unsupported activation plans never silently substitute A16 arithmetic. The public Engine-options rejection of unsupported native KV formats is EngineCache-owned.

The upstream Q8 family already selects dynamic shared memory above 48 KiB and calls `cudaFuncSetAttribute`; it is retained rather than transplanting the fork-local W8 kernels. Dense BF16/INT8 attention also already has explicit dynamic-shared-memory launch support.

## Test coverage and exclusions

- `tests/ops/excluded_paths.h` lists exact permitted architecture refusal messages; arbitrary CUDA/runtime errors are not exclusions.
- `ninfer_architecture_contract_test` exercises the native activation, attention, and block-scaled MoE refusal entrypoints. It must fail if an excluded entrypoint returns normally.
- Linear, LinearAdd, SwiGLU, segmented projection, GDN record/snapshot, and dense attention retain supported numerical cases. Unsupported A4/A8 arms are named explicitly. Existing accumulated failures are preserved. A16 cases cannot be accepted as architecture refusals.
- Native GDN snapshot coverage includes large-T A16 cases instead of asserting an unsupported activation mode executes. Block-scaled MoE is reported as excluded, not numerically qualified.
- No numerical tolerance was relaxed. No GPU correctness or performance pass is claimed until the native binaries are exercised.

## Commands and observed evidence

Working-directory prefixes are omitted for privacy; `common/build.sh` is the program-provided build helper. Builds use holder `C2`, priority 2, a 20-minute lease, and persistent architecture-specific build directories.

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
- The same lease then failed the sm_89 build: the native FP8 forward declaration did not carry the default row policy, and NVCC's host compilation rejected four-argument callers. Defaults now live only on the first declaration, with a direct row-policy header dependency; no warning suppression or caller arithmetic change. Combined lease runtime was 259 seconds.
- Windows links, sm_89 completion, and native GPU qualification are not yet proven.
- Native v3 artifact supplied by Infra: SHA-256 `5476dfd6b243332f1754a494469ba4a1c8206edba6518b61dba1e0f4338664e7`, 18,210,749,936 bytes, 1,118 tensors. Infra reports v3 framing/directory validation; this is not a model-decode proof.

## Provenance and remaining gates

- Behavioral references: shipped native fork and the upstream architecture/kernel contracts. Community ports were design references; no community source file was copied.
- Required remaining proof: Linux compilation for both native profiles, MSVC link for both profiles, ops suites on their respective physical GPUs with the explicit exclusions, and one server-path model decode on each GPU with tokens/second and configured/measured context recorded.
- Native runs use isolated work directories and loopback ports `18200–18209`, never installed release roots or the shipped port.
