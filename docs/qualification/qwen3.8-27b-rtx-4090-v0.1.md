# Qwen3.8 27B RTX 4090 qualification

## Verdict

**Not beta-qualified; no RTX 4090 support claim.** The reproducible, source-controlled Golden-equivalent reached an exact typed OMP tool call on the final clean `sm_89` candidate, but its typed argument oracle rejected the model output. Per the stop-on-first-red rule, tool-result continuation and the exact visible final-answer oracle were not credited.

The historical private corpus is unavailable and was not reused, read, copied, hashed, or transmitted. This qualification used only the synthetic fixture committed as `golden_equivalent_contract.json`.

## Final candidate identities

| Item | Value |
|---|---|
| Runtime source | `40580cc703b03197573789923b5866007aba0a68` |
| Package source | `7a425890a10fe31c207816697b4bd89d8f5b319c` |
| Source archive SHA-256 | `9c595ca0aa4d932c9fc60f7dc5a484ad8c56dd3e1d85dae3d99ae3f08bc0d020` |
| Server SHA-256 | `66fb8187e70df2e842b6cb23a8e7ee0392ecec868c5265ac737c98084bb0b9f2` |
| Config SHA-256 | `e7f86a4da23d17bc50b3c261263a636df2cf317a82f84b22bcae5d6a186e7623` |
| Model SHA-256 | `eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e` |
| Package SHA-256 | `f9c5aa0f5a005c70474b4b4ee5f8fa4bc4ed61d91283543e176a2683dfd5d885` |
| Package bytes | `227425120` |
| CUDA target/toolkit | `sm_89` / `13.3.73` |

The build started from an empty build directory and embedded `patch_stack_sha=40580cc703b03197573789923b5866007aba0a68`, `source_dirty=false`, `build_type=Release`, and `cuda_architecture=89`. The focused native Responses test, Python Golden-equivalent tests, Python compile checks, MTP decision regression, and package asset regression passed. The package contains the runner, extension, and contract under `smoke/`, covered by package checksums.

## Source-controlled Golden-equivalent

Contract `qwen38-4090-omp-golden-equivalent-v1` defines one fixed tool invocation:

- tool: `ninfer_golden_weather_lookup`;
- arguments: string `city="Paris"`, integer `days=3`, boolean `metric=true`;
- one fixed tool result;
- one exact visible final answer: `NINFER_GOLDEN_EQUIVALENT_OK|Paris|3|metric=true|18C|clear`.

The gate ran through standard OMP `18.0.6` over the final package's OpenAI-compatible endpoint. Ambient rules, skills, extensions, MCP discovery, sessions, and built-in tools were disabled; only the source-controlled tool schema was exposed.

## Live result

| Gate | Result |
|---|---|
| Exact clean source/build/package identity | Passed |
| Authenticated loopback status identity | Passed |
| Typed OMP tool invocation | Passed |
| Typed argument oracle | **Failed: `typed argument oracle rejected OMP output`** |
| Tool-result continuation | Not evaluated after first red |
| Exact visible final-answer oracle | Not evaluated after first red |
| Beta support decision | Blocked |

The sanitized no-session runner deliberately retained no raw transcript, so it did not retain the rejected actual argument object. It did retain the exact first failing stage and message. No private prompt or output exists in the receipt.

Previously completed protocol, 102K process-restart persistence, and bounded performance receipts remain valid inherited engineering evidence for the unchanged runtime lineage, but they cannot override a red required Golden-equivalent gate on this final candidate.

## Restoration

The bounded lease used a hash-pinned adapter whose only stop/start effects invoked the existing tracked `sf-long-persistent-control.ps1` controller (`79f14e4b…`). Restoration passed:

- `sf-long-persistent-candidate` is running again on `100.116.135.24:18081` as `qwen38-long`;
- the predecessor runtime/model/controller identities match the pre-lease state;
- the candidate listener, state root, scheduled task, and GPU-owner lease are absent;
- the base controller remains paused/Ready and Docker remains paused;
- no stable promotion or permanent route mutation occurred.

## Receipts

The source-bound sanitized Golden-equivalent failure receipt is `receipts/qwen3.8-27b-rtx-4090-golden-equivalent.json`, SHA-256 `5c6a1cdb0fe14d3bac582b9151ca26717d962caa917ccca8b5fe287b28f3a5cc`.

Supporting on-host receipt SHA-256 values:

- clean build: `e9a6b0deb0ee2179ba38d4c20e370f7795109057b8403b1e5bfaccc496f6d452`;
- final package: `5607f6c7f79286c9041c8515ab3d2bcd70de9645c4a76d01fb99858818493d75`;
- lease ready: `ca37013f50fe8046bd94c86928c67419308e8e41b247191e0c3a71d14510f6fb`;
- final restore: `79d0f4be4684c96ca735bc743e0e3d2002f547a4a7aa4afe81eac45f3afe330b`.
