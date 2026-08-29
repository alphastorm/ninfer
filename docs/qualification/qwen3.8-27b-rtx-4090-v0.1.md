# Qwen3.8 27B RTX 4090 qualification

## Verdict

**Qualified for beta support as a source-bound release candidate.** Gates G, L, and R passed on the final binary and finalized asset set. No stable promotion, public release, or permanent route change was performed.

The unavailable historical private corpus was not reused, read, copied, hashed, or transmitted. Golden-equivalent coverage uses only the source-controlled synthetic typed-tool contract and deterministic final-answer oracle.

## Exact identities

| Item | Value |
|---|---|
| Runtime source | `6fd9e4507d00331a29c20fe4bed8ace11c0b3a0f` |
| Runtime source archive | `83ed4cc9fff62929942186176ab7fcc360ac8e99926fb955a3e971b2c8b543f5` / `5254811` bytes |
| Package source | `2fb84928e29da7b7c6be708cade723121457fa1f` |
| Package source archive | `fdc08c36d8eecf70fff1cd2c6add2898c831b00d7c80167ab75a8e51ea6eb3de` / `5255750` bytes |
| Server | `8e63c6a90b54913d8aa0c4d660f67ff1fab036e5435a144d450ff7dc1ce664a3` / `230262272` bytes |
| Config | `ec5e4cdb167ac26fc7cc762f9e3d188b965c4e2a20ecfe3cbb1968f084e872db` |
| Model | `eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e` |
| Package | `7548307420faab9f6cf82294c3aa0b6a6ea82168d89c8f5ac4900a683665b418` / `227402593` bytes |
| SPDX 2.3 SBOM | `d37ab787f9f2ccc0212f56c1beb1cffb010a058ae15c2c89853b84a0824b3eaf` / `22373` bytes / `27` files |
| Qualification sidecar | `30d8d741d1957ab13a6139f0d673b43ee0258b049cefa3b26d927f043a7b1656` / `8705` bytes |
| SHA256SUMS | `dcd4bfbdc0c029fbd02c3345abdc4f5d4cc1212d845e6d22e76d1a474013ddce` / `680` bytes / LF only |
| Deployment profile | `qwen38-4090-v0.1` |
| CUDA target/toolkit | `sm_89` / `13.3.73` |

## Transactional DELETE correction

The response DELETE path now holds the response-store transaction boundary while it builds a replacement live store and post-delete session checkpoint, commits the checkpoint generation, and performs no-throw live swaps. Foreign-session LRU publication cannot interleave with the durable commit. Publication failure or quota rejection returns conflict while preserving both the live records and the durable `current` generation.

The deterministic native regression covers failed publication, a foreign-session LRU race, quota rejection, restart durability, and surviving-descendant engine-tag semantics. DELETE removes response addressability and the session-key checkpoint rooted at the latest surviving session key; it does not claim cryptographic erasure of ancestor tokens still reachable through a surviving descendant checkpoint.

Because runtime inputs changed, every hardware-dependent gate was rerun instead of inherited.

## Windows release-boundary hardening

The shipped installer contains no callable test mode, ambient test activation, or fault-injection hook. Synthetic failure instrumentation is generated only in the unshipped lifecycle harness. Its receipt enumerates every substituted component and function and separately identifies the exact unmodified shipped components.

Real coverage is separate:

- each missing managed directory is created atomically with its final SYSTEM/Administrators-only descriptor;
- NULL DACLs, raced or precreated roots, unowned roots, and reparse points fail closed without deleting an untrusted tree;
- a real low-privilege principal was denied protected reads and writes, while the deliberately created NULL-DACL probe was observed readable before rejection;
- the generic GPU-owner controller invokes only the trusted absolute system query, rejects malformed or ambiguous output, and uses the release-bound owner state root on every action;
- the default owner-state root remained unchanged during nondefault-root tests;
- the installer snapshots and restores both lifecycle helpers during rollback;
- two exact shipped-installer full installs exercised upgrade, rollback, candidate return, retained-secret ACLs, and owner-state-root binding;
- GPU power-limit mutations were exactly zero.

Status on a protected root populated with 2,048 sparse files and 2 GiB of logical checkpoint data completed in `3.4954474` seconds, below the 30-second gate.

## Qualification authority and publication safety

The in-package status is `candidate-only-not-release-eligible`. The passed external sidecar is the sole release-eligibility authority; it supersedes `candidate_ready` only after G, L, and R pass and is bound by package-owned `SHA256SUMS`.

The finalizer first reverified the candidate checksums for every immutable asset, then replaced only the external qualification sidecar and LF checksum manifest. The ZIP and SPDX hashes are unchanged from candidate assembly. All four standalone lifecycle scripts are byte-identical to their ZIP members. The ZIP has 27 unique file members and no duplicate names.

A final scan covered all eight finalized assets and all 27 ZIP members. A separate scan covered all 1,093 intended tracked files. Both found zero private identifiers.

## Live rerun results

### Protocol

The authenticated 15-check pack passed in full, including typed tool arguments, tool-result arrays, stream parity, private-session cache isolation, stored Responses isolation, continuation, streaming checkpointing, parent deletion, descendant survival, and Anthropic token counting.

Protocol receipt: `43360c9f78e6e48b75c3676344095d5a16c6015ab713059900f2a6113d2a7918`.

### Exact 102K process checkpoint restart

The first stored Responses turn contained exactly `102060` input tokens. A controller restart replaced the process; the next turn restored `102075` tokens and appended through `append_frontier`. The post-restart prompt contained `102097` tokens. No raw prompt or output is retained.

Long-restart receipt: `6ca95941272e0d4eb2397bbb3dc41a14bb96ad89f52e2944fbb0ce085ca377d4`.

### Bounded performance

The exact public `long_decode_aime26_01` fixture ran once after one warmup:

| Metric | Result |
|---|---:|
| Prompt tokens | 228 |
| Completion tokens | 1485 |
| Prefill | 1408.259 tokens/s |
| Decode | 52.263 tokens/s |
| Wall time | 28.612 s |
| Prefix cache hits | 0 |
| Reuse path | `full_reset` |

No builder or compressor ran before or after measurement. The candidate was the sole measured NInfer compute owner. Fourteen unavoidable Windows display-model rows are disclosed separately; no other row reported measurable compute memory.

Performance receipt: `9bf85003102f89a523812498bbe992827d2ee139c0367dce02e6191b615205ac`.

### Final Golden-equivalent

Contract `qwen38-4090-omp-golden-equivalent-v1` passed through standard OMP `18.0.6` in `6.786655` seconds:

| Gate | Result |
|---|---|
| Typed tool invocation | Passed |
| Typed argument oracle (`city="Paris"`, integer `days=3`, boolean `metric=true`) | Passed |
| Tool-result continuation | Passed |
| Exact visible final answer | Passed |

Exact visible answer: `NINFER_GOLDEN_EQUIVALENT_OK|Paris|3|metric=true|18C|clear`.

Golden receipt: `8331118c6f84e2c695d11f519e529cbec871e67d9951a62b822622c9d47f62f4`. Raw transcript content is not retained.

## Restoration

Every live GPU-owner stop/start effect used the hash-pinned tracked operator controller. The exact incumbent route is healthy again. Candidate listener, state root, scheduled task, processes, probe account, and credential material are absent. No production route was promoted or permanently mutated.

Final restore receipt: `a0e6665a98e4a9bb734d2ea4dee23d2be7d31a94656c7e09cd6f36ca9ebfe1bb`.

## Receipts

- public asset receipt: `e05ad2e750e6856227910509d9d20aeb0949c751dff3a68d7f2c9bcecbf69c6b`;
- neutral clean build: `56834a34871cdd6d8f8903ede947de543cb859ea97d076310a94aff595c0ab11`;
- native transactional DELETE: `6a33ec4b68cd7bed6f49af50c821574b8a3397d65192cfc2055c4aa3cbf56ea2`;
- package assembly: `bf22794202bb2da25cf4275861f4ec72f9d76537bc631d09bedbe2a93ac90d5b`;
- immutable package finalization: `33f381fcce753c969cfe16ea52082a448ac71463846e5d93c8c697b99edd3420`;
- final private-identifier scan: `b8cc1cd9de6cc9b775635cb7f7621f0664fc8e30d3c6324c10dd4914aa7841fc`;
- released-script byte binding: `9fd4f24de65c3afea1abcce327525d3a7f5246eca150257c23ac2df425fa0d94`;
- real state security: `e1c42dced5ba78eeeb1857362d38cbbc73851500b2927e8455e7e98f4884e5bf`;
- instrumented lifecycle: `a1a7ea14d6032984faf811ac48e1f37af2352a6889897afd1efdbdc040d5e03d`;
- real upgrade/rollback secret ACL: `4c8f6510cf5ad919a156749b58b1626bef428b0993dff2746bcc1d3d951f3105`;
- protected request log: `a374ad04d7426cacfa074648130e5736d1e0066ddc2e1d7f944bbd285ccaf4f5`;
- populated-root status timing: `5e63ca267ccebcefb204d6328d4afb5f91b07ed42f0e3feaa36b949b24f3f278`;
- protocol: `43360c9f78e6e48b75c3676344095d5a16c6015ab713059900f2a6113d2a7918`;
- 102K restart: `6ca95941272e0d4eb2397bbb3dc41a14bb96ad89f52e2944fbb0ce085ca377d4`;
- performance: `9bf85003102f89a523812498bbe992827d2ee139c0367dce02e6191b615205ac`;
- final Golden: `8331118c6f84e2c695d11f519e529cbec871e67d9951a62b822622c9d47f62f4`;
- final restore: `a0e6665a98e4a9bb734d2ea4dee23d2be7d31a94656c7e09cd6f36ca9ebfe1bb`.
