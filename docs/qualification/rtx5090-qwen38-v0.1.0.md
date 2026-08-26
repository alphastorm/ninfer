# RTX 5090 × Qwen3.8 v0.1.0 qualification

**Decision as of 2026-08-25: BLOCKED — not qualified and not authorized for publication or promotion.**

This packet freezes the public, release-head-bound acceptance contract for the Qwen3.8-27B
`groupwise-int` NInfer release on one RTX 5090. It does not convert prior production, dirty-tree, or
pre-release measurements into release evidence. A gate changes from `not_run` only when a real
candidate receipt identifies the exact clean release HEAD and compiled binary.

The machine-readable decision is
[`rtx5090-qwen38-v0.1.0.json`](rtx5090-qwen38-v0.1.0.json). The frozen gate manifest, isolated
candidate contract, and public-safe measurement schema are under
[`fixtures/`](fixtures/rtx5090-qwen38-v0.1.0-gates.json).

## Immutable release identity

A qualifying candidate must report all of the following through `--version`, authenticated status,
and its build receipt:

| Field | Required value |
|---|---|
| upstream base | `4eef14a7560d87a3ba717898e1d488a4c4c7246d` |
| patch stack | exact clean HEAD used for this candidate and every recorded gate |
| build profile | `qwen38-5090-v0.1.0` |
| build type | `Release` |
| source dirty | `false` |
| compiler | non-empty compiled C++ and CUDA compiler identities |
| CUDA toolkit | non-empty compiled toolkit identity |
| model artifact | `qwen3_8_27b.ninfer`, 18,210,531,328 bytes |
| model SHA-256 | `eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e` |

`tools/lifecycle/ninfer_container.py build` refuses tracked or untracked changes, proves that the
upstream commit is an ancestor, and passes Docker a Git archive of the exact measured HEAD. A direct
Docker build without that clean-source attestation remains dirty by construction. Run the final
configure and build only after the last release commit; otherwise the embedded patch-stack SHA is
not the tested release HEAD.

## Isolated candidate and incumbent protection

The only allowed release candidate is:

- image and container: `ninfer-5090-v010-rc`
- loopback port: `18089`
- restart policy: `no`
- build profile: `qwen38-5090-v0.1.0`
- distinct candidate configuration and request log

Port `18088` and container `ninfer-5090` are protected. The candidate workflow must not stop,
replace, rebuild, reconfigure, relabel, or route traffic to that incumbent. Cleanup uses the owned
lifecycle stop operation only; it retains the stopped candidate and image as rollback evidence.
Promotion is external to this repository, is never automatic, and is forbidden while this record is
blocked.

## Required gates

### 1. Exact 130,048-token context

Use the committed `examples/cli/messages/long_niah_128k.json` fixture. Its SHA-256 is
`8dcece5215805293490baa0d8204d59a899d8bc3f9d010b0c142ed6624260a9a`, its declared prompt length is
130,048 tokens, and the exact expected output is:

```text
ORCHID=493817; COLOR=COBALT
```

Run with maximum context 131,072, the frozen candidate configuration, thinking disabled, and at
most 128 new tokens.
The receipt must record the fixture hash, measured prompt-token count, exact-match result, candidate
binary hash, model hash, and clean release identity.

### 2. Golden t01

The approved Golden t01 output must match exactly and total elapsed time must be no more than
`120.9117` seconds. Its prompt and approved fixture digest are not present in this public repository,
so this gate cannot be executed or independently audited from this worktree. Do not substitute a
similar prompt, reconstruct the fixture, or reuse an older timing. The external runner must supply
its approved fixture SHA-256 in the public-safe measurement receipt.

### 3. Decode throughput

Measured decode throughput must be at least `200.58921` committed tokens per second. The receipt
must describe the fixed request count and output-token denominator and must not mix prefill time,
concurrent aggregate throughput, or a different speculative configuration into this value.

### 4. MTP acceptance

Aggregate MTP acceptance must be in the inclusive interval `[0.72319, 0.78319]`. Record drafted and
accepted token totals as well as the reported rate so the aggregate can be checked. A value outside
the interval fails even when decode throughput passes.

### 5. Process-exit policy

This probe applies only to `ninfer-5090-v010-rc`. With Docker restart policy `no`, terminate the
candidate process, wait the fixed observation grace period, and prove that the container remains
`exited` with restart-count delta zero. Then explicitly start the same owned candidate and prove
authenticated health with the same binary/model/config identities. Never direct this probe at the
incumbent.

### 6. Live Responses HTTP corpus

Run `tools/smoke/agent_protocol.py` against candidate port 18089 with the candidate API-key file. The
live-server corpus must prove a first Responses turn, continuation, two forks from one parent,
cross-session continuation rejection with content-safe 404, parent deletion, failed parent
retrieval, and retrieval/continuation of surviving descendants. The configured API key is the sole
stored-response tenant. `ninfer_session` scopes POST continuation and Engine cache lineage; the
standard retrieve/delete/input-Item/cancel routes use the API key plus opaque Response ID and do not
claim a second tenant boundary.

## Evidence and release assets

A measurement must validate against
[`rtx5090-qwen38-v0.1.0-measurement.schema.json`](fixtures/rtx5090-qwen38-v0.1.0-measurement.schema.json)
and must contain no hostname, filesystem path, secret, prompt text, generated text other than the
fixed exact oracle, GPU UUID, or process arguments. Raw private logs stay outside the repository.
Only bounded public-safe receipts belong in this record.

After all gates pass on one exact clean HEAD, create the local binary asset, checksum file, and SPDX
2.3 SBOM with:

```bash
python3 tools/release/package.py \
  --ninfer /path/to/release/ninfer \
  --ninfer-serve /path/to/release/ninfer-serve \
  --output-dir /path/to/local/release-output \
  --release-version v0.1.0 \
  --platform linux-x86_64-cuda13.1 \
  --upstream-base-sha 4eef14a7560d87a3ba717898e1d488a4c4c7246d \
  --release-head-sha "$(git rev-parse HEAD)" \
  --build-profile qwen38-5090-v0.1.0
```

The packager executes both binaries' `--version`, requires matching exact identities with
`source_dirty=false`, reads `LICENSE` from the release commit, normalizes archive ownership, modes,
ordering, and timestamps, and emits no model weights. It refuses to overwrite an existing local
release packet. Publishing, uploading, tagging, and registry mutation are deliberately out of scope.

## Current blockers and limitation

No authorized isolated RTX 5090 execution mechanism was exposed to this release worktree, and the
approved external Golden t01 fixture identity is unavailable here. Consequently no candidate build,
HTTP corpus, performance, process-exit, checksum, or SBOM receipt is claimed. Existing production
state was not inspected or changed.

Even after the gates pass, this packet establishes behavior on one RTX 5090 machine only. It does
not prove cross-machine reproducibility, RTX 4090 parity, cloud portability, or production-route
fitness. Those statements require their own independently bound evidence.
