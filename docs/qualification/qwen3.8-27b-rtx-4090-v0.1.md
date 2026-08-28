# Qwen3.8 27B RTX 4090 qualification

## Verdict

**Beta-qualified for RTX 4090 support.** The final clean-source `sm_89` package passed the source-controlled Golden-equivalent through OMP: one typed tool invocation, exact string/integer/boolean arguments, tool-result continuation, and the exact visible final answer.

No stable release or permanent route change was performed. The historical private corpus is unavailable and was not reused, read, copied, hashed, or transmitted; this qualification used only the committed synthetic fixture.

## Exact identities

| Item | Value |
|---|---|
| Runtime source | `4e67e6f96f108a17abac7a324c23c5a96e71de32` |
| Package source | `69f03550d5c068ec5b89a75b1380bdeb96f45cc6` |
| Source archive SHA-256 | `54d6ef81c134e2405a00e5a2a60885fde3d5431f206a2be9fef6986c8c27f0da` |
| Server SHA-256 | `bb77a5be24fcd2b8f78d598887bfe7c7516d78c4b7d33086c28deb639ac76388` |
| Config SHA-256 | `e7f86a4da23d17bc50b3c261263a636df2cf317a82f84b22bcae5d6a186e7623` |
| Model SHA-256 | `eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e` |
| Package SHA-256 | `24b18e8949e08e049b87e60adb5c18eff3110cffb8db61c1e4743cde4005bbf7` |
| Package bytes | `227410510` |
| CUDA target/toolkit | `sm_89` / `13.3.73` |

The fixed build retained the original empty-build-directory lineage and rebuilt the exact new source identity. Runtime C++ was unchanged. Native Responses, the seven-case Python Golden regression, and package asset checks passed.

## Sequential unmasking and nearest cause

The first live attempt reached the required tool but failed the typed argument oracle. A bounded sanitized diagnostic projected the actual calls without retaining prompts, secrets, or raw transcripts:

- OMP default intent tracing injected required extra argument `i`;
- `z.literal(...)` emitted `const`-only parameter schemas;
- NInfer intentionally preserves values as strings when JSON Schema `type` is absent, so `days` arrived as string `"3"` and `metric` as string `"true"`;
- the failed tool execution caused a second identical tool attempt.

The nearest cause was the qualification adapter, not the NInfer runtime. The fix disables OMP intent tracing in the isolated profile and exposes explicit `string`, `integer`, and `boolean` schemas while retaining the same exact-value scorer and final-answer contract. The captured bad argument object is a permanent red fixture; the corrected domain object is the matching green fixture. A local OMP wire regression confirmed exactly two provider turns, no injected `i`, and explicit primitive types before the single authorized full live rerun.

## Final Golden-equivalent

Contract `qwen38-4090-omp-golden-equivalent-v1` passed through standard OMP `18.0.6` in `6.282964` seconds:

| Gate | Result |
|---|---|
| Typed tool invocation | Passed |
| Typed argument oracle (`city="Paris"`, integer `days=3`, boolean `metric=true`) | Passed |
| Tool-result continuation | Passed |
| Exact visible final answer | Passed |

Exact visible answer: `NINFER_GOLDEN_EQUIVALENT_OK|Paris|3|metric=true|18C|clear`.

The final transcript projection SHA-256 is `77f581081492b5a0b15a9667c557c20532e5461f998d854347e3019a19552a5a`; raw transcript content is not retained.

Previously completed protocol, 102K process-restart persistence, and uncontaminated performance receipts remain inherited evidence for the unchanged runtime lineage.

## Restoration

Every GPU-owner stop/start effect went through the hash-pinned adapter and tracked incumbent controller. Final restoration passed:

- the incumbent route is healthy;
- the candidate listener, state root, task, and lease are absent;
- the private fleet identity is intentionally omitted from this public receipt;
- no stable promotion or permanent route mutation occurred.

## Receipts

- final Golden receipt: `receipts/qwen3.8-27b-rtx-4090-golden-equivalent.json`, SHA-256 `e51f351dead50f9d2d3254c9c6542cf7baf9e7b678af4a546e5aba1883f38076`;
- sanitized argument diagnostic: `receipts/qwen3.8-27b-rtx-4090-golden-equivalent-argument-diagnostic.json`, SHA-256 `8702fcd4aefccef8c35ee220a6a626cfdd4a48b1a9499e46efc5328157ac10d0`;
- fixed build receipt: `3690afc6e65ea0ab653fb48409ba0882b646d06d34dbb34b163babcaac8343b7`;
- fixed package receipt: `924920cf7cb3421ba6f30c6e7e0940a0828e37dce388f4e8330077d18b02d8c1`;
- final lease-ready receipt: `dc733a4f638260b6d28d6d96d2ec834a65aa8ff7f54ed285158c86cedd8442f5`;
- final restore receipt: `32a8e9c7da48e4dd046c9706c0539e3cc41b572eee21d3ed40b7bbfcc7876057`.
