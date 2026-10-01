# NVFP4 paged-KV selective port

Donor: Neroued/ninfer `neroued/master` at `d44ab58408aa389728cd8b1ee50179527e1f3e0d`.
Fork baseline: `52f38907af265501c3a3c079e099a5746e40f0b7`.

The provenance gate inspected each commit with `git show --stat` and read the final
NVFP4 files, their shared dependency closure, and the append/attention tests using
`git show neroued/master:<path>`. Recorded stat totals:

| Commit | Files / insertions / deletions | Selected provenance |
| --- | --- | --- |
| 4ac73c47 | 85 / 6149 / 1151 | storage schema, group16 codec, standalone and batched append |
| a7818988 | 19 / 1062 / 890 | masked physical width may exceed the live envelope (NVFP4 only) |
| 1192ad76 | 33 / 1477 / 1198 | Grouped, ParallelGrouped, Tiled attention, multibatch append, workspace |
| 1737ca11 | 75 / 890 / 2027 | only causal primitives referenced by the final NVFP4 family |
| d44ab584 | 7 / 93 / 45 | NVFP4 ParallelGrouped width-192 planner and workspace enumeration |

## Representation and adaptations

Both NVFP4 K and V use U8 code planes of leading extent 128 and U8 scale
planes of leading extent 16 for a logical D256 vector: 288 bytes per token/head
across K, V, K-scale, V-scale. Page size remains 64, with 256-byte plane alignment.
The final donor codec uses the D256 sign/Hadamard transform, E2M1 packed values,
and UE4M3 group16 scales. This representation is separate from weight NVFP4.

The product enum is appended as `Nvfp4` (donor `Nvfp4Group16`). Legacy dtype/
quant-group views remain valid; an explicit storage identity selects NVFP4.
The final shared causal helpers are new files in this fork and used only by
NVFP4. Existing BF16/INT8/FP8 routers and kernel implementations are retained.
The tiled NVFP4 translation unit is non-RDC in the existing SM120a archive;
other builds receive rejecting entry points, not unresolved device symbols.
Checkpoint payload transfer remains format-agnostic and copies every plane.

## Exclusions

No wholesale commit cherry-picks. K8V4, `21a0e85f` (paged BF16 V changed to FP16),
BF16/FP8/INT8 retunes, upstream BF16 attention, `23b0997d`, `98ba2dac`,
`c71795e1`, artifact-v3/runtime cutovers, and weight projection refactors are
excluded. The fork BF16 K and V remain BF16; DFlash2 cyclic draft FP16 V and
Q5 4/8/16 projection routes are unchanged. Upstream qualification additions are
adapted without replacing the fork BF16 oracle or graph scheduling policy.

## Qualification boundary

The intended appliance profile is two requests, DFlash2 K=7,
`--max-context 131072 --kv-capacity auto --device-state-slots 4` with NVFP4 KV.
262,144 tokens is the desired **aggregate** capacity, not per-request context.
Builds and CPU checks run in the GPU-less NVFP4 build container. Append bytes,
attention numerics, graph replay, real-model checkpoint resume, memory fit and
the existing 89-case MTP3 BF16 byte-identity corpus require the lead GPU window.
