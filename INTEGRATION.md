# Upstream integration

## Invariants and decisions

- Branch: `rebase/upstream-64d10fbf`; sole integration owner: Integrator2 (successor to Integrator). Frozen upstream base: `64d10fbf9`. All three release lanes target this base; older scout recommendations to retain native fork binaries are superseded.
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
| Base | upstream `64d10fbf9` | Infra reports apps/tests ON, benches OFF, `120a`, 285.323 s | 45/45 aggregate passed after the three missing-driver cases passed their changed-environment follow-up | Corrected `r2` image: seven passed, six failed, two skipped across all real labels; 189.272 s. Explicit dense v3 loading passed, 5.728 s | [Final Infra baseline](https://github.com/alphastorm/ninfer/blob/8526f23ad/REPORT.md) |
| Shared prerequisites | `dc3140b23`: native-profile gate and scope carriers | Apps/tests `120a` build passed; lease job elapsed 289 s, exit 0 | `ninfer_qwen_frontend_test` passed, 0.71 s | Not run; no GPU-ready environment yet | Integration-owned prerequisite, not a slice checkpoint |
| Contracts candidate | merge `a2fcef5c`, proof/prose `bc551eefb`; shared declarations through `42efad145`; K6 clarifications through `96585c84e` | Changed-source integrated proof pending; slice's three standalone C++20 header compiles passed | Slice: C++ five canonical vectors, Python six canonical/MAC tests passed; integrated selector contains 47 tests | Nine applicable real cases, KV regression and explicit loading selected; prefix golden remains blocked | [Contracts proof](https://github.com/alphastorm/ninfer/blob/cf9ba7db/REPORT.md) |
| Baseline fixture correction | `1bcaffa4d`, cherry-pick of EngineCache `091c6a815` | Included in next integrated proof | No production-policy change | Five no-EOS cache helpers explicitly select non-strict Automatic tools; dedicated agent-continuation proof pending | [EngineCache report](https://github.com/alphastorm/ninfer/blob/091c6a815/REPORT.md) |
| Baseline reservation correction | `36a42facf`, cherry-pick of Persistence `117a166a`, isolated from its later native-import changes | Included in next integrated proof | No host-only import APIs pulled into this boundary | Quotes replay growth after rewind; retained/unretained tail regression and grammar proof pending | [Persistence diagnosis](https://github.com/alphastorm/ninfer/blob/117a166a/REPORT.md) |

Baseline inventory must distinguish failures, skips and missing prerequisites. `tests/models/qwen3_5/tests.cmake` registers 14 `gpu;real` cases. `loading_real` needs explicit `--artifact` (the environment variable alone skips); MoE/offload cases need a matching MoE artifact. The supplied dense v3 artifact does not by itself prove every registered model/component route.

Observed prerequisite command sequence: `NINFER_BUILD_HOLDER=int` exported, then `build.sh <source> <build>`, then `run-in-env.sh --mount <source> --mount <build> -- ctest --test-dir <build> -R '^ninfer_qwen_frontend_test$' --output-on-failure --timeout 180`. The private absolute holder roots are intentionally omitted. This proves compilation and the scope-carrier host regression, not runtime isolation or GPU behavior. Infra's initial GPU baseline is environment-invalid: its driver stub overrode the real driver (`cudaErrorStubLibrary`); Python oracle dependencies were also absent. The corrected environment must pass Infra's device/oracle smoke before GPU work resumes.

Infra reports corrected `ov-build-env:master-64d10fbf-r2`, image `e7f34ca2f41e`, passed the real device test (1.298 s) and three Python-backed template/schema oracles (2.709 s). Old integration job `20261025T013006Z-int-checkpoint-7c6ba673-250067` was cancelled while queued, before execution; it is neither proof nor a product failure. Replacement proof pins the changed source and corrected image, builds apps/tests, runs Infra's 45 host tests plus two contract tests, then nine applicable real cases, the targeted KV-history regression and explicit v3 loading with Vision/DFlash2/optimized proposal. Host tests mount the real driver with `--gpu --env CUDA_VISIBLE_DEVICES=`; model/server tests leave that variable absent. No green checkpoint is claimed.

Replacement lease job: `20261025T015729Z-int-checkpoint-36a42fac-336877`, exact source `36a42facfec7280265fa929fa78044136797d70d`, priority 2, 15-minute cap, one batched incremental build/test iteration. It is the blocked integration proof, not a lead G1/G2 priority-1 gate.

### Vanilla baseline disposition

The three host loader failures were `ninfer_openai_schema_test`, `ninfer_request_log_test` and `ninfer_http_error_handler_test`: `libcuda.so.1` was unavailable after the build stub was correctly isolated. Their changed-environment follow-up passed in lease `20261025T014632Z-infra-host-baseline-closeout-304281`, giving 45/45 aggregate host coverage; the integration host run uses the corrected driver mount. The separate 124-test non-real GPU scan completed only 94 cases before its 900-second deadline: 90 passed, four timed out, 30 uncompleted. The four 90-second timeouts were `ninfer_softmax_attention_test`, `ninfer_context_kv_materialize_test`, `ninfer_linear_bf16_a16_test` and `ninfer_linear_split_multi_output_test`; no assertion failure is claimed. This is incomplete evidence, not a green suite; KernelRoutes has the exact inventory for the later full-suite gate.

| Applicable release blocker | Observed failure / root cause | Owner and accepted action |
|---|---|---|
| `ninfer_qwen3_5_grammar_real_test` | `KV write exceeds its page reservation`; replay growth was quoted against pre-rewind membership | Persistence's isolated [#419](https://github.com/Neroued/ninfer/issues/419) fix preserves the reservation guard and adds a 65→8→65 retained/unretained regression. Changed-source GPU proof pending; related [#414](https://github.com/Neroued/ninfer/issues/414) is not claimed fixed. |
| `ninfer_qwen3_5_prefix_real_test` | Thinking-prompt golden differs from the supplied artifact | Frontend must record the artifact/template epoch cause before correcting a golden, or fix rendering if wrong. Deferred, **not excluded**. |
| `ninfer_qwen3_5_agent_continuation_real_test` | Basic constraints reject the fixture's intentional no-default-EOS policy | Main approved explicit Automatic constraints in five non-strict cache helpers. Production defaults/guard and token/frontier/role assertions are unchanged; changed-source proof pending. |

Main explicitly approved these **v3-dense subset exclusions**, not blanket test suppression:

| Excluded test | Artifact-specific reason |
|---|---|
| `ninfer_qwen3_5_moe_real_test` | Requires the 35B MoE fixture; the supplied 27B dense model has a different architecture/load summary. |
| `ninfer_qwen3_5_moe_offload_real_test` | MoE/offload fixture's 16 GiB device budget cannot hold the supplied dense artifact's weights/resources. |
| `ninfer_qwen3_5_dflash_real_test` | Requires the legacy `dflash` component absent from this artifact; applicable DFlash2 and DFlash-prefill tests remain selected. |

The seven vanilla dense passes are native transactions, preemption, tools, score, vision workspace, DFlash2 and DFlash prefill. Bare Qwen3.5 loading skips without its explicit argument; the explicit invocation passed. Qwen4 loading is outside this subset. Infra also proved the published v3 artifact `81f924d440c27261d820c19a9f8d45794c5aee410f8a68bd358133fa8c0375da` preserves all 1,184 unique tensor payload hashes, geometry/reference sets and all six frontend resources from the production artifact's v3 upgrade; format/provenance differences are not evidence of a changed production template.

### Mechanical G1 receipt

Persistence's [G1 receipt](https://github.com/alphastorm/ninfer/blob/0e64cc77/G1.json) records a real restart on tracer source `e442df4d`, published v3 and the corrected image: 22,010 checkpoint-hit tokens, 93 byte-identical continuation bytes, 10 completion tokens, lease `20261025T015558Z-A2-tracer-restart-e442df4d-r2-332480`, exit 0 in 64 s, no leftover/unowned containers. Native export/import measured 6.67914/4.2713 s for 1,638,737,920 payload bytes. Scope is C1, 32,768 context, BF16/DFlash2 K7, text Chat feasibility; this is not integrated production checkpoint-set, Responses, C2 or Windows qualification. The tracer branch is not merged by this receipt.

## Merge queue and conflict watchlist

Queue is provider-first, not a fixed file-owner list. Contracts precedes every durable provider. Hardened DurableStore backends additionally require PortableCore's host-only K6 I/O. ServePort's status schema 2 requires EngineCache's real accounting, while its pure identity/snapshot provider precedes the snapshot-aware store. Frontend, KernelRoutes, Harness, ArchEnable and Persistence enter when their providers and proof are ready; dependent A3/B2/C3/E2 follow. Main approved this dependency rule: every merged boundary must be independently green; a source-only merge is not a green checkpoint.

Preflight after a pushed slice is announced: `git fetch origin`, then `git merge-tree --write-tree rebase/upstream-64d10fbf origin/rebase/<slice>`. Preflight writes Git objects only, never the integration working tree. The first fetch found no current-program slice branches beyond the integration base; `rebase/durable-4090-v041` is unrelated and excluded. DurableStore standalone `0789709d` preflight against `7fccec947` was clean (exit 0); not yet merged or built.

Latest read-only preflights use `36a42facf`; these are conflict inventories, not merge qualification:

| Candidate | Observed conflicts / resolution boundary |
|---|---|
| PortableCore `b0f184124` | Copied `INTEGRATION.md` only; production sources auto-merge. |
| Frontend `5104b523` and ServePort `8233bd95` | Logs/reports plus older K0 contract prose, adapter header and header test. Header/test differences are exactly the two approved startup identity getters and their signature assertions; keep the integration declarations. Production implementations auto-merge. |
| EngineCache `091c6a815` | Logs/reports plus `RuntimeTests.cmake` and `test_serve_options.cpp` insertion collisions; keep both test groups. Production implementations auto-merge. |
| DurableStore `5d0350a5` | Logs/reports, older K0 surfaces, and `serve_options.{cpp,h}`. Its transitive older serving provider lacks the current K5 durable/stop union; retain the integration options, not those deletions. Provider-first order remains necessary. |
| Harness `abdb7d115` and Infra `0d754712` | Root `REPORT.md` only; preserve immutable source report links. |
| ArchEnable `940b000c` | Copied integration log and root report only. |
| KernelRoutes `7bd48e11` | Clean read-only merge tree, exit 0; source readiness/performance proof is separate. |
| Persistence `0130b930` | Report, older K0 surfaces and KV-history test insertion. Preserve the isolated replay regression and, only with Phase 2 providers, its host-only import regression. Production sources auto-merge. |

Recheck newer source boundaries before merging. No native-import APIs or tests were pulled into the isolated `36a42facf` correction.

Main approved the next isolated checkpoint additions: Persistence `d1f4832e` refreshes an already-admitted unit's reservation when a new reader requires tail COW; Contracts `c30e5746` centralizes frozen K7 reason spellings with compile-time assertions. Read-only cherry-pick preflights against `b54b56a43` found only the root report conflict for `d1f4832e`, and no conflict for `c30e5746`. Neither changes the currently queued `36a42facf` proof input, and neither establishes that upstream #414 is resolved.
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


K2 identity accessors: Main approved internal `checkpoint_layout(const Engine&)` and `frontend_identity(const Engine&)`, returning the existing typed identity structures. Shared declarations and compile-time signature assertions are present. Persistence owns actual native geometry; Frontend owns one-time hashing of loaded resource bytes and exact selected template source. Serving queries once at startup and caches its fingerprint; provider implementation and integration proof remain separate from these declarations.
Custom-tool boundary: Main confirmed shipped raw custom calls and Responses replay remain in scope. Shared `ToolKind { Function, Custom }` defaults to Function on history/output tool calls; Custom input remains opaque text. Master carries tool declarations as JSON, so Frontend owns kind propagation into its internal definition contract, templates and parser; ServePort owns wire objects/replay. Main also approved `SchemaMismatch` / `schema_mismatch` for declared-type atomic fallback, without changing inference for unknown parameters. Declaration → Frontend → ServePort custom-tool commit is distinct from the identity/publication merge.

## Shared-file requests

| Requester | Requested surface | Ownership / current state |
|---|---|---|

| Contracts | New `runtime/contract/{durable_checkpoint_set,durable_engine_api,runtime_fingerprint}.h`; contract test registration | Frozen provider `746b29d2` merged with standalone C++/Python CTest registration. Internal adapter friendship and typed identity getter declarations are present; no public persistence methods. |
| DurableStore | Store/backend sources and host-only test targets | Authorized standalone SHA/backend registration consumed as `dc5ffcbb`; snapshot-aware store remains dependent on ServePort and K6. |
| ServePort | Serving source/test registration; persistence and managed-stop CLI union | Authorized source/test registration supplied. Identity, bounded API-key-file input, K5 persistence budgets and managed-stop options ported with parser regressions. Actual restore/final-save/shutdown orchestration awaits providers and must reject enabled launches without them. |
| Persistence | `program/storage/durable_tracer.cpp` adjacent to `checkpoints.cpp` in `program_sources.cmake` | Exact slice-local insertion permitted for G1 build; integration owns merge disposition. Prefix-identity hook remains slice-owned and must become typed for final K1. |
| EngineCache | `gpu_keep_warm_ms`, `--gpu-keep-warm-ms`, core source and test registration | Exact shared patch supplied and consumed on slice; runtime role/occupancy declarations pushed separately after Main approval. |
| PortableCore | Root/core portability wiring; exact `Uint128` migrations in shared resources and serve options | Authorized platform registration consumed as `bb5bf4cc1`; shared arithmetic patch supplied. Minimal Qwen4 read-completion category migration authorized. |
| ArchEnable | Single `120a`, `89` or `86` architecture gate; propagated `NINFER_SM86`/`NINFER_SM89` definitions; ops CTest label | Root gate/definitions applied. Existing `ninfer_add_op_test` appends `ops` for `ctest -L ops`, preserving skip semantics; native support still awaits family-local gating and qualification. |
| Harness | BuildInfo header/module, compile-owner verification, source registration and version consumers | Standalone provider `f4f42173` ready; exact compile-owner/core/app registration patch supplied for slice consumption. |

## Open prerequisites

- Valid vanilla model and 45/45 aggregate host evidence is consumed above. The first changed-source integration proof remains pending.
- Contracts' first candidate includes frozen declarations, fixtures and host CTest registration. Integrated build/host/model proof must pass before a checkpoint tag or the next implementation merge.
- Frontend is active on grammar whitespace, parser/custom-tool preservation and the approved loaded-identity provider; its source inspection found upstream already inserts stable leading/tool boundaries, with regression coverage being added.
- Exact requested declarations/source lists before shared edits; do not register nonexistent sources on the integration branch.
- PortableCore and ArchEnable must land before adding native Windows architecture builds to the routine.
- GPU selection remains numeric `--device`; no new engine UUID flag/field. Controller sets child `CUDA_DEVICE_ORDER=PCI_BUS_ID`, requires absent `CUDA_VISIBLE_DEVICES`, and attests the request-log GPU UUID. Tooling UUID arguments remain unchanged.
