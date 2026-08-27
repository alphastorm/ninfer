# Qwen3.8 27B RTX 4090 qualification

## Verdict

**Not qualified; no RTX 4090 support claim.** A fresh native Windows `sm_89` Release build and package installed through the external-pinned-model lifecycle and restarted successfully on loopback. The required direct Golden-equivalent gate then failed before protocol, 100K+ context/persistence, or bounded performance evidence could run.

## Fresh identities

| Item | Value |
|---|---|
| Runtime source | `ea265776254a62ab5184454ba0163cdf04aad1e5` |
| Package-producing lifecycle source | `09f38db0d506d09e7c381c862aeea3e243e09669` |
| Focused-regression lifecycle source | `669b729af635b55c69bfe8b5e76fd45614c96883` |
| CUDA target/toolkit | `sm_89` / `13.3.73` |
| Fresh server SHA-256 | `ea10b9a540722d1ac3f8832a8856e103c4040c599550d59dc4250eb9b5094e86` |
| Model SHA-256 | `eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e` |
| Config SHA-256 | `d613fc71ebe30b799af4936af8f73b0f25ebd1a1486b55fdb59aaab5d884bb96` |
| Package SHA-256 | `b2081fecd5f6ef3a9fcab3d58a0bb3129af16cca0099bd085b554d7d84ebb530` |
| Package bytes | `227626612` |

The package checksum set passed. Its embedded installer (`6bd36fa790…`) and controller (`b01254f14…`) came from package-producing lifecycle commit `09f38db0d506d09e7c381c862aeea3e243e09669`; those scripts are superseded by the focused-regression lifecycle source `669b729af635b55c69bfe8b5e76fd45614c96883` and do not inherit its fixes. The installer retained the 18,210,531,328-byte model as an external, pinned, read-only reference; it did not copy the model into candidate-owned state.
Machine-readable focused receipts are bound at lifecycle `669b729af635b55c69bfe8b5e76fd45614c96883`: lifecycle `cfdb852343d765ecf3a1fe54e8c0ae70325e2b658f9708cc391e21cfe310d277`, assets `e3d732687ecec40435497b04efc08ff413d51d0fc8cf03f877f119ad76c60b57`, and MTP decision `6aebf34e142b3f5f79130fc7d0bf0e37ea1c1bd0813ea599776f523a27ab6c98`.

Focused lifecycle regressions passed, including ten injected transaction failures, zero restart model re-hashes, zero candidate model copies, interrupted-install repair, dead-start rejection, and release-asset identity checks.

## Live result

| Gate | Result |
|---|---|
| Fresh package install | Passed |
| Packaged controller restart | Passed |
| Authenticated loopback identity | Passed: clean build, exact binary/model/config identities, 131,072-token configured context, MTP0 |
| Golden-equivalent t01 | **Failed** |
| Protocol pack | Not run after Golden red |
| 100K+ context and process-restart persistence | Not run after Golden red |
| Bounded performance request | Not run after Golden red |

The Golden-equivalent harness used the exact pinned t01 task, fixture, and scorer over the direct loopback endpoint. The model emitted textual `<tool_call>` markup instead of a typed tool call, so the copied fixture remained unchanged. Executing the unchanged fixture with an unknown arm did not reject before side effects and hit the 30-second bound. Output SHA-256: `8e7847b76261f4ec359853261ceff5cf46093c8525481426f57441cdc9a6329e`.

A local deterministic reproduction found two `<tool_call>` and two `<function=...>` openings, but zero corresponding closing tags. This is already represented by `test_malformed_falls_back_to_text`: the parser must preserve incomplete markup as text. Treating it as a typed call would fabricate missing arguments and could execute truncated code, so there is no safe project-owned parser fix and no same-identity GPU rerun is justified. Reproduction receipt SHA-256: `537697b43cb33cdcb85e164d83f8923eba2830e1aabf0aed73d6c7673715a150`.

Per the stop-on-first-red rule, no protocol, long-context, persistence, performance, MTP3, route, publication, or support decision followed.

## Platform blockers resolved before the gate

1. Package receipt construction attempted to assign an optional `latest_attempt_utc` property directly on a strict `PSCustomObject`. The red path exited 1. The fixed path uses add-or-update property semantics and produced the fresh package above.
2. A second `Expand-Archive` in one PowerShell runspace stalled after partial extraction. Running the installer in a fresh `pwsh` process closed the class: both 22-file expansions completed and produced the exact server binary hash.
3. The fixed installer returns `ninfer_windows_release_install_receipt` with nested lifecycle status. The runner was corrected to validate that actual terminal contract rather than the superseded direct-status shape.

## Restoration

The candidate task, state root, operation root, lease, and loopback listener are absent. The predecessor controller and binary were restored exactly, its endpoint returned `qwen38-long`, Docker remains paused with its pause marker present and service stopped, and the GPU power limit remains 500 W.

- Window receipt SHA-256: `3f92867c89cf2248b5baa01aab2bdf93f0222520b2070da6fcae2e69d0bd5dc2`
- Independent restore receipt SHA-256: `0ab4eaf421750547a83d69c89abb5b7d9861b5a96cdcb23827ae0246ae34c7c2`

## Next decision

Keep RTX 4090 support blocked. The next bounded attempt must first reproduce and fix typed tool-call emission for the exact direct Golden-equivalent task, then rerun the fresh-package protocol, 100K+ restart-persistence, and bounded-performance gates. Do not publish or promote this package.

Final bounded affected-class rereview closure passed at `dca808b53c8ab13362aa2dce16a57751959000f7`; self-check receipt SHA-256: `ea7b99fc60bf9a452d3ada67785c0ac6d6135d11a38fcb4a77a94638ea5cbae7`. Qualification remains failed and the package remains non-release.
