# Qwen3.8 27B RTX 4090 qualification

## Verdict

**Qualified for beta support as a source-bound release candidate. All runtime, Windows trust, disclosure, and closed asset-manifest gates passed on the final clean package.** No stable promotion, public release, or permanent route change was performed.

The unavailable historical private corpus was not reused, read, copied, hashed, or transmitted. Golden-equivalent coverage uses only the source-controlled synthetic typed-tool contract and deterministic final-answer oracle.

## Exact identities

| Item | Value |
|---|---|
| Runtime source | `e4654b5aed87e7385bdf33f8b4365a7a550d4ad9` |
| Runtime source archive | `0c8b7afd73ae1354d82f7f01b84e7b42b7fab203df5484fe4d954c6eaaf20d6d` / `5257807` bytes |
| Package source | `19ebcc09ebbcc79061badf78068e1ad70062485f` |
| Package source archive | `c0d9c294f986834ea177842bf336e9c38d28200fcfeb325c9b218df70ddae39c` / `5261801` bytes |
| Server | `41fcc1803e4a057de24a485776e71737a31cb39fdd9358350b975f6b11b9c869` / `230305280` bytes |
| Config | `ec5e4cdb167ac26fc7cc762f9e3d188b965c4e2a20ecfe3cbb1968f084e872db` |
| Model | `eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e` |
| Package | `1cced9e1f16ad0d1b4a5f9c5a28d00cb2b4449137f1652696799be70a74baf7d` / `227438715` bytes |
| SPDX 2.3 SBOM | `474f0c0e83903bec64744d9cd12b71a6b46b25647d218726d2ab7f61c9873e9a` / `22373` bytes / `27` files |
| Qualification sidecar | `543e7ff9d174b6a7896acb1ccbd8332a3af3a9dc4c248794d3f073678c7a4497` / `8705` bytes |
| SHA256SUMS | `e1d74d03094454c7c3f95452919e5f5e0b71164932e5d9256e54be03090a8c3a` / `680` bytes / LF only |
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

The fixed status regression uses 2,048 sparse files and 2 GiB of logical lengths. It is a bounded synthetic file-walk measurement, not a realized quota-full generation topology and not a worst-case quota file-count claim. The prior run completed in `3.8825062` seconds below its 30-second gate; the final clean package run passed.

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

Long-restart receipt: `8587c8617d3ee46a32861f8e919e25beda641f1c61154ea0d7a6a5c48aac6979`.

### Bounded performance

The exact public `long_decode_aime26_01` fixture ran once after one warmup:

| Metric | Result |
|---|---:|
| Prompt tokens | 228 |
| Completion tokens | 1168 |
| Prefill | 1410.691 tokens/s |
| Decode | 52.330 tokens/s |
| Wall time | 22.512 s |
| Prefix cache hits | 0 |
| Reuse path | `full_reset` |

No builder or compressor ran before or after measurement. The candidate was the sole measured NInfer compute owner. Fourteen unavoidable Windows display-model rows are disclosed separately; no other row reported measurable compute memory.

Performance receipt: `35917c7df793c82db51f696addd0a5b21763e7b25d041c4ffd7b578682e23358`.

### Final Golden-equivalent

Contract `qwen38-4090-omp-golden-equivalent-v1` passed through standard OMP `18.0.6` in `6.786655` seconds:

| Gate | Result |
|---|---|
| Typed tool invocation | Passed |
| Typed argument oracle (`city="Paris"`, integer `days=3`, boolean `metric=true`) | Passed |
| Tool-result continuation | Passed |
| Exact visible final answer | Passed |

Exact visible answer: `NINFER_GOLDEN_EQUIVALENT_OK|Paris|3|metric=true|18C|clear`.

Golden receipt: `bf4675664058418a2c64a4f4b8ed59b9abd0db57f927087256dafb0477ff13ba`. Raw transcript content is not retained.

## Restoration

Every live GPU-owner stop/start effect used the hash-pinned tracked operator controller. The exact incumbent route is healthy again. Candidate listener, state root, scheduled task, processes, probe account, and credential material are absent. No production route was promoted or permanently mutated.

Final restore receipt: `45b64577cac78691f7077c586e99c8bc653381c18e15069f74b30ed27044e293`.

## Receipts

- public asset receipt: `e224ce431b3cc7e56a6c7c60f0578cb892de453d81175e8fa3bf2b531d940475`;
- neutral clean build/package: `9d74814848b74830b1886a66b91e381df7c9ebdff31d53afd8ca64fc403281fb`;
- native transactional DELETE: `fa6fc50dc311611a723e071dc2faa16efcecb662b305739bff106cf0fa83e447`;
- package assembly: `9b3e9dfe8659cdca128884eaf14be54c5658100909ba2ed3a20870934a56e653`;
- immutable package finalization: `0fed3f247db0624164c4b1bf3b62a16879694e29989af76da7172184b28477d3`;
- final private-identifier scan: `b8cc1cd9de6cc9b775635cb7f7621f0664fc8e30d3c6324c10dd4914aa7841fc`;
- committed-script byte binding: `5aa4657f2809f39918ad12eeb3bddb350ec44f117756f3b6cdac2e581ebf1e6c`;
- real state security: `02a2d1b531241189fd7754d477ddaa4abe21a33705d10e0898dc538a8badba81`;
- instrumented lifecycle: `a9d2b71c0704be94b528e4cc59c87c40f7754dcdcbb2b69d429248f626dc7946`;
- live GPU-owner/state ACL: `a8ed789ea1a7197de43bd74b7c4b76ca578431ba2a27ffddbb308722f46a7216`;
- protected request log: `a6d2ce12c6149606e98831cef1d16fe24d2f7d1b945e9b9ff966aa3c9add2973`;
- populated-root status timing: `f71d7f38488126372a9b6fe9c0659ca1b747aecac6e7bc7fbf64a91885732c2c`;
- protocol: `bfc0ae064e1ff7e1c421df0dd51ebf17de416cd545676dd142e2c3bb378127ac`;
- 102K restart: `8587c8617d3ee46a32861f8e919e25beda641f1c61154ea0d7a6a5c48aac6979`;
- performance: `35917c7df793c82db51f696addd0a5b21763e7b25d041c4ffd7b578682e23358`;
- final Golden: `bf4675664058418a2c64a4f4b8ed59b9abd0db57f927087256dafb0477ff13ba`;
- final restore: `45b64577cac78691f7077c586e99c8bc653381c18e15069f74b30ed27044e293`.
