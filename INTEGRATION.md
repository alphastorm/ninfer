# Upstream integration

## Invariants and decisions

- Branch: `rebase/upstream-64d10fbf`; sole integration owner: Integrator. Frozen upstream base: `64d10fbf9`. All three release lanes target this base; older scout recommendations to retain native fork binaries are superseded.
- Preserve upstream scheduler, DFlash2, grammar and context ownership. No NVFP4 KV port, standby-pinning retries, old monolithic Program, or unshipped `3a2fadbd` route.
- Contracts K1–K7 require lead approval before changes. Contracts merges first; dependent implementations consume its frozen declarations.
- Preparation: RepoPrompt chat `prep-v0-13-0-upstream-re-DF08C8`, export `oracle-plan-2026-10-24-174946-prep-v0-13-0-upstrea-1d64.md` (private, not committed).
- Release assurance: production/hard-to-reverse; Main owns the final G4 council over the frozen candidate. This lane owns integration proof, not publication or review dispatch.
- Infra owns the single vanilla-master build/test baseline; Integrator consumes its evidence and runs subsequent integrated checkpoints. No duplicate vanilla run.
- All heavy work uses the assigned GPU lease; no production-container or existing-package changes. Checkpoint tags are created only after the required checks pass.

## Base cherry-pick evaluation

Read-only command for each candidate: `git merge-tree --write-tree --merge-base=<commit>^ HEAD <commit>` at vanilla `64d10fbf9`. Exit status was 1 for each; no cherry-pick or working-tree conflict was created.

| Candidate | Observed conflicts / semantic boundary | Disposition |
|---|---|---|
| `9822c38d7` grammar whitespace | `tests/models/common/qwen/test_tool_constraints.cpp` conflicts; tests target the newer dev frontend. Includes vendored xgrammar const/enum structural-whitespace changes. | Not applied; Main confirmed Frontend is queued and its brief includes re-expression on master. |
| `4ec903a0f` direct-I/O alignment | `docs/issue-checklist.md` modify/delete; `src/artifact/materializer.cpp`, `tests/artifact/materialization_io.cpp`, `test_materialization.cpp`, `tests.cmake` content conflicts. Dev materializer/test machinery differs. | Not applied; PortableCore owns re-expression of per-file windows, aligned staging and original-buffer ownership. |
| `1e902680d` container dependencies | `README.md` and `src/serve/generation_service.h` conflicts. Dockerfile auto-merges but also imports dev JPEG/shared-library packaging assumptions. | Not applied; Infra owns master-appropriate dependencies/container changes. |

## Merge and verification log

| Checkpoint | Source / commit | Build | Host tests | Real-model tests | Slice report |
|---|---|---|---|---|---|
| Base | upstream `64d10fbf9` | Awaiting Infra baseline | Not run by integration | Not run by integration | Infra report pending |

Baseline inventory must distinguish failures, skips and missing prerequisites. `tests/models/qwen3_5/tests.cmake` registers 14 `gpu;real` cases. `loading_real` needs explicit `--artifact` (the environment variable alone skips); MoE/offload cases need a matching MoE artifact. The supplied dense v3 artifact does not by itself prove every registered model/component route.

## Shared-file requests

| Requester | Requested surface | Ownership / current state |
|---|---|---|
| Contracts | New `runtime/contract/{durable_checkpoint_set,durable_engine_api,runtime_fingerprint}.h`; contract test registration | New headers reserved to Contracts; integration owns CMake; merge first. |
| DurableStore | Store/backend sources and host-only test targets | Exact entries awaited; use existing test helpers, no duplicate convention. |
| ServePort | Serving source/test registration; persistence and managed-stop CLI union | Integration owns shared CMake/options; ServePort owns request declarations and shutdown orchestration. |
| Persistence | `program/storage/durable_tracer.cpp` adjacent to `checkpoints.cpp` in `program_sources.cmake` | Exact slice-local insertion permitted for G1 build; integration owns merge disposition. Prefix-identity hook remains slice-owned and must become typed for final K1. |
| EngineCache | `gpu_keep_warm_ms`, `--gpu-keep-warm-ms`, core source and test registration | Integration owns public option/CLI/CMake; EngineCache supplies exact entries. |
| PortableCore | Root/core portability wiring; exact `Uint128` migrations in shared resources and serve options | `src/core/uint128.h` is PortableCore-owned; EngineCache owns engine arithmetic callers; integration owns shared callers. |
| ArchEnable | Single `120a`, `89` or `86` architecture gate; propagated `NINFER_SM86`/`NINFER_SM89` definitions | Integration owns root gate; family-local source/stub gating remains ArchEnable-owned. |
| Harness | BuildInfo header/module, compile-owner verification, source registration and version consumers | New provenance files reserved to Harness; integration owns shared wiring/consumers. |

## Open prerequisites

- Infra build image/helper and vanilla baseline evidence.
- Contracts freeze and mergeable branch/report.
- Frontend starts when a worker slot becomes available; grammar-whitespace handoff is queued.
- Exact requested declarations/source lists before shared edits; do not register nonexistent sources on the integration branch.
- PortableCore and ArchEnable must land before adding native Windows architecture builds to the routine.
