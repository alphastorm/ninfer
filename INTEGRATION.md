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
| Base | upstream `64d10fbf9` | Infra reports apps/tests ON, benches OFF, `120a`, 285.323 s; image `502ca8b3465c` | Infra evidence pending | Infra evidence pending | [Infra environment](https://github.com/alphastorm/ninfer/blob/ee39c4626/REPORT.md) |
| Shared prerequisites | `dc3140b23`: native-profile gate and scope carriers | Apps/tests `120a` build passed; lease job elapsed 289 s, exit 0 | `ninfer_qwen_frontend_test` passed, 0.71 s | Not run; no GPU-ready environment yet | Integration-owned prerequisite, not a slice checkpoint |

Baseline inventory must distinguish failures, skips and missing prerequisites. `tests/models/qwen3_5/tests.cmake` registers 14 `gpu;real` cases. `loading_real` needs explicit `--artifact` (the environment variable alone skips); MoE/offload cases need a matching MoE artifact. The supplied dense v3 artifact does not by itself prove every registered model/component route.

Observed prerequisite command sequence: `NINFER_BUILD_HOLDER=int` exported, then `build.sh <source> <build>`, then `run-in-env.sh --mount <source> --mount <build> -- ctest --test-dir <build> -R '^ninfer_qwen_frontend_test$' --output-on-failure --timeout 180`. The private absolute holder roots are intentionally omitted. This proves compilation and the scope-carrier host regression, not runtime isolation or GPU behavior. Infra's initial GPU baseline is environment-invalid: its driver stub overrode the real driver (`cudaErrorStubLibrary`); Python oracle dependencies were also absent. The corrected environment must pass Infra's device/oracle smoke before GPU work resumes.

## Merge queue and conflict watchlist

Queue: Contracts → DurableStore standalone backend/I/O → ServePort snapshot/identity → DurableStore snapshot-aware store → Frontend → KernelRoutes → EngineCache → Harness → PortableCore → ArchEnable → Persistence → dependent A3/B2/C3/E2 integration. Main approved provider-first ordering: every merged boundary must be independently green; a source-only merge is not a green checkpoint.

Preflight after a pushed slice is announced: `git fetch origin`, then `git merge-tree --write-tree rebase/upstream-64d10fbf origin/rebase/<slice>`. Preflight writes Git objects only, never the integration working tree. The first fetch found no current-program slice branches beyond the integration base; `rebase/durable-4090-v041` is unrelated and excluded. DurableStore standalone `0789709d` preflight against `7fccec947` was clean (exit 0); not yet merged or built.

| Boundary | Expected overlap / dependency | Disposition rule |
|---|---|---|
| Slice reports | Every slice adds root `REPORT.md` | Link each immutable slice commit's report from the merge log; do not silently overwrite another slice's evidence. |
| Contracts ↔ DurableStore | New durable types, codecs and checkpoint I/O headers | Contracts declarations are authoritative; escalate contract deltas. |
| DurableStore ↔ ServePort | Store tests require the new Responses snapshot/codec | Owners must identify the minimum prerequisite or split standalone backend work before queue admission. |
| ServePort ↔ Frontend | Tool-name limit, validator and prompt-boundary behavior | ServePort owns serving files; Frontend owns shared Qwen parser/grammar; keep one authoritative limit. |
| ServePort ↔ EngineCache | Private session isolation versus shared principal fanout | Main approved immutable private session reuse scope; shared stable/tool prefixes retain bearer-principal scope across sessions. No blanket shared-cache disable. |
| EngineCache ↔ Persistence | Engine hooks, Program checkpoint APIs and pressure protection | Keep native API sole-owned by Persistence; merge dependent controls atomically. |
| PortableCore ↔ shared callers | Wide arithmetic, file handles and read-completion error categories | Owner supplies exact caller migrations; preserve Linux behavior and error taxonomy. |
| PortableCore ↔ ArchEnable | Platform/architecture gates, PDL and dependency targets | One compile-profile gate; family-local rejection paths; no fat binary. |
| Harness ↔ Infra | Build provenance, archive manifest and Docker CMake inputs | Preserve clean-source attestation; no `.git` means neither clean nor dirty by inference. |
| All sources/tests | Shared CMake manifests and CLI/config union | Integrator owns final wiring; never register missing files or advertise unsupported consumers. |

Scope-carrier prerequisite: public `ContextCacheHints` and internal fixed-capacity `PreparedContextCache` now carry separate private `reuse_scope` and principal `shared_reuse_scope`. Frontend preparation preserves both, including `update_session_index=false`, and rejects empty/oversized values. Existing whole-struct forwarding already carries them through Qwen3.5 (`planning/request_plan.cpp`) and Qwen4 (`program/request.cpp`); no duplicate native copying was added. Runtime filtering and fanout regressions remain EngineCache-owned; the carrier alone is not isolation proof.

K7 telemetry prerequisite: Main approved `PrivateEndpoint`, `PrivateInputReplay`, `PrivateLongAnchor` and `SharedStablePrefix` reuse paths plus `RuntimeStats::last_selected_reuse_path`. EngineCache classifies the actual selected checkpoint role at successful binding; native paths without a semantic role retain `Root`/`Checkpoint`. No `PrivateTurnClosure` is synthesized. Main also approved `private_catalog_occupied` and `shared_catalog_occupied` as actual post-prune catalog-entry counts, not residency or reader counts. Serving status schema 2 retains every shipped field name; required consumer fields remain real/non-null. Harness's consumer audit governs unavailable-field nulls and downstream schema updates.

K3/K4 publication rule: Main approved a mandatory Engine-supplied live-service guard around final epoch recheck, atomic `current` replacement and sync. Payload export, hashing and bulk writes remain outside the Engine lock; only tests may inject a fake publication guard. Contracts owns the exact interface freeze.

## Shared-file requests

| Requester | Requested surface | Ownership / current state |
|---|---|---|
| Contracts | New `runtime/contract/{durable_checkpoint_set,durable_engine_api,runtime_fingerprint}.h`; contract test registration | Frozen provider `746b29d2` preflight clean. Integration adds internal adapter friendship to Engine, PreparedPrompt and GenerationHandle; no public persistence methods. Contract CTest registration is staged for first merge. |
| DurableStore | Store/backend sources and host-only test targets | Authorized standalone SHA/backend registration consumed as `dc5ffcbb`; snapshot-aware store remains dependent on ServePort and K6. |
| ServePort | Serving source/test registration; persistence and managed-stop CLI union | Authorized source/test registration supplied. Identity, bounded API-key-file input, K5 persistence budgets and managed-stop options ported with parser regressions. Actual restore/final-save/shutdown orchestration awaits providers and must reject enabled launches without them. |
| Persistence | `program/storage/durable_tracer.cpp` adjacent to `checkpoints.cpp` in `program_sources.cmake` | Exact slice-local insertion permitted for G1 build; integration owns merge disposition. Prefix-identity hook remains slice-owned and must become typed for final K1. |
| EngineCache | `gpu_keep_warm_ms`, `--gpu-keep-warm-ms`, core source and test registration | Exact shared patch supplied and consumed on slice; runtime role/occupancy declarations pushed separately after Main approval. |
| PortableCore | Root/core portability wiring; exact `Uint128` migrations in shared resources and serve options | Authorized platform registration consumed as `bb5bf4cc1`; shared arithmetic patch supplied. Minimal Qwen4 read-completion category migration authorized. |
| ArchEnable | Single `120a`, `89` or `86` architecture gate; propagated `NINFER_SM86`/`NINFER_SM89` definitions; ops CTest label | Root gate/definitions applied. Existing `ninfer_add_op_test` appends `ops` for `ctest -L ops`, preserving skip semantics; native support still awaits family-local gating and qualification. |
| Harness | BuildInfo header/module, compile-owner verification, source registration and version consumers | Standalone provider `f4f42173` ready; exact compile-owner/core/app registration patch supplied for slice consumption. |

## Open prerequisites

- Infra valid vanilla host/model baseline evidence remains pending; image refresh isolates the link-only driver stub and adds Git plus Python oracle dependencies. GPU work is blocked until Infra broadcasts observed readiness.
- Contracts declared the K1–K7 freeze; mergeable commit/report and standalone proof are pending.
- Frontend starts when a worker slot becomes available; grammar-whitespace handoff is queued.
- Exact requested declarations/source lists before shared edits; do not register nonexistent sources on the integration branch.
- PortableCore and ArchEnable must land before adding native Windows architecture builds to the routine.
- GPU selection remains numeric `--device`; no new engine UUID flag/field. Controller sets child `CUDA_DEVICE_ORDER=PCI_BUS_ID`, requires absent `CUDA_VISIBLE_DEVICES`, and attests the request-log GPU UUID. Tooling UUID arguments remain unchanged.
