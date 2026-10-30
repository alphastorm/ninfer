# N15 upstream fix dispositions

N15 takes three bounded fixes: failure-atomic workspace layout planning, Linux listener address
reuse after shutdown, and agent-scratch exclusion from container contexts. Five candidates are
already represented in the fork; seven remain
excluded for P16. No new prompt, tokenizer, grammar, model, kernel, native fit, or checkpoint
contract change is admitted by this lane.

## Frozen inputs and method

- Fork baseline `N`: `752dd887be6a9cdb2da6217f3b664b3c03ed33ae`.
- Upstream snapshot `U`: `1882c6adbc98043fcf89f3589aa5addb3742777a`
  (`refs/remotes/neroued/master`).
- Merge-base `B`: `64d10fbf9f8e6bfc79ab0570b94674c5b9997929`, established with
  `git merge-base 752dd887 refs/remotes/neroued/master`.
- Candidate subjects and full identities were checked with `git log` and `git show`;
  the reviewed range is `B..U`, not a rebase onto `U`.
- The P15 preparation's section 3.5 owns the lower-layer-first policy. Source review followed
  artifact, grammar, core, runtime, and serving boundaries; the optional streaming feature
  was dispositioned last. No new pick was started after the lane's 04:45Z cutoff.
- Each candidate received a read-only
  `git merge-tree --write-tree --merge-base=<candidate>^ 752dd887 <candidate>` preflight.
  Every preflight returned 1 because at least one path conflicts; this is not a test failure.
  The output trees below identify those preflights, not deployable candidates.
- All three admitted fixes were applied with `git cherry-pick -x`. The workspace and listener
  implementations/tests merged without a code conflict; their sole resolution retained the
  fork's deletion of obsolete `docs/issue-checklist.md`. The build pick adds only the missing
  scratch exclusion; existing dependencies, provenance, native headers, and isolated driver
  stub win over conflicting upstream context. No upstream scratch ledger was resurrected.
- Fork contracts win: stop event, keep-warm, API-key file, origin authentication, fit fields,
  DirectStorage checkpoint backend, and the native model-load seam are unchanged.

Classes follow section 3.5: **clean** means unchanged fork code, **textual** means a path
changed in `B..N` (including deletion), **semantic** means an affected fork API/contract,
and **prompt-rendering** conservatively includes constrained-output whitespace changes.
A textual documentation conflict does not imply a semantic implementation conflict.

## Candidate ledger

The tests column names relevant executable coverage, not a claim that every listed test ran.
`merge-tree` ran for all rows. Exact-head CI run IDs, conclusions, and remaining GPU work are
reported in the L5 delivery report; GPU-required tests were not run by this off-fleet lane.

| Area | Upstream SHA and verified subject | Class | Disposition and fork-faithful resolution | Tests / evidence |
| --- | --- | --- | --- | --- |
| Artifact | `4ec903a0f571638ffe39555e8bdc59a86a65b077` — `fix(artifact): honor direct I/O alignment during materialization` | semantic | **Already present; do not replay.** `b3b3b73a8` ports native direct-I/O geometry. Per-file windows, memory/offset alignment, original pinned owners, Host in-place reads, short-read checks, and staging-slack accounting are in the fork. Upstream additionally assumes newer encoded-slice/range machinery; importing it would replace rather than fix the native materializer. | `src/artifact/materializer.cpp:59-80,134-190,367-426`; `tests/artifact/materialization_io.cpp:187-287`; materialization CPU and CUDA cases are retained. Preflight: materializer/test content conflicts plus obsolete-doc deletion. |
| Grammar | `9822c38d7dba8d7f9d1bd5382d314ef24a4b34e8` — `fix(grammar): allow whitespace in JSON output` | prompt-rendering | **Already present; do not replay.** P14's `928f83e0a` / `8c3102905` port admits structural whitespace, including const/enum containers and reasoning framing. A second application would duplicate tests and conflict with fork hardening. No new whitespace language is introduced. | `src/text/json_schema.cpp:407-414`; `third_party/xgrammar/cpp/json_schema_converter.cc:3181-3265`; `tests/text/test_json_schema.cpp:105-194,250-251`. Existing bounded grammar/JSON-schema/oracle tests cover the retained behavior. |
| Grammar | `1407c62f6b9c4cb1dec9447114174899e6f80e75` — `perf(grammar): compile tool strings as terminal automata` | semantic | **Declined; P16.** This is a compiler/performance redesign, not an isolated defect correction. Its tool-grammar side imports the native-streaming `tool_format.h` / `tool_name_constraint.h` interface from `bc70d7c8`, replaces the fork's parameter-language construction, and touches the excluded GLM frontend. Equivalent token masks on the shipped tool/reasoning grammar are unproven. A dependency on the declined streaming feature is not admitted. | Read upstream grammar, Qwen tool grammar and converter changes. Preflight conflicts in Qwen tool grammar and deleted GLM source/tests. No new automata tests or GPU mask claims. |
| Core | `d602894abcd071c273003f021258f46608aab447` — `fix(core): preserve workspace layout on allocation failure` | textual (code clean) | **Applied as `e69cecd14`.** Compute aligned offset locally; update the cursor only after checked addition succeeds. Preserve the upstream implementation and overflow/scope regressions exactly. Keep obsolete issue-checklist deleted. Successful layout geometry and allocation sizes are unchanged. | `src/core/layout.cpp:107-123`; `tests/test_memory.cpp` overflow, subsequent-allocation, scope restoration, and tensor-overflow cases; target `ninfer_memory_test`. Preflight's sole conflict was the deleted checklist. |
| Runtime/cache | `77241ffb1015448d67737f21be60a306cdfcc370` — `fix(cache): honor host placement before context migration` | semantic | **Declined; P16.** The fix is implemented against new `SessionImpl`, `ContextDirectory`, `HostReservations`, executor reservations, and physical release-inventory APIs absent from the fork. It is not the later unified-pool refactor itself, but taking it still requires the separately excluded executor/session restructuring. Do not transplant those ownership APIs into the durable-checkpoint fork. | Read upstream pause/host-placement, resource-manager, and scheduler changes. Preflight reports deleted session/directory files and conflicts in the fork resource manager. No migration behavior changed or claimed equivalent. |
| Runtime | `2cf044529002ea1a854adc3cb3d61626ca4c3e6b` — `fix(runtime): separate planning and allocation budgets` | semantic | **Declined; P16.** It consumes unified `ContextCapacity`, prepared/distributed startup, rank preparation, and shared-physical-memory Host-reservation contracts. The fork retains token-based `KvCapacityPolicy`, `Program`, and a three-argument startup budget. Re-expressing only selected arithmetic would silently change native headroom/fit meaning. | `src/runtime/engine/startup_memory.cpp:8-29`; `tests/runtime/test_kv_capacity.cpp:54-75`; upstream startup/budget diffs reviewed. Preflight has missing prepared/executor/context-capacity interfaces and startup content conflicts. |
| Runtime | `704fa68da9addc5e353ba14f10004d4c9829a1f0` — `fix(runtime): publish terminal responses after request cleanup` | semantic | **Already present; do not replay.** `199fa88dd`, `9aeb3185d`, and baseline `752dd887b` retire successful, cancelled, and failed rows before publishing the corresponding runtime snapshot and delivering responses. Preserve this fork-specific implementation and durable publication ordering instead of importing `engine_*.inl` and Session ownership. | `src/runtime/engine/engine_core.h:798-983`; `tests/runtime/test_engine_core.cpp:102-103,1249-1253` plus success/error/cancellation publication regressions. Existing bounded `ninfer_engine_core_test` exercises the fake Program boundary. |
| Frontend | `419d637db97b8213eab5e5f67887f340d601fdc9` — `fix(frontend): count thinking tokens without a budget` | semantic | **Declined; P16.** The fork deliberately leaves its independent semantic tracker dormant for unlimited thinking (`output_session.cpp:367-371`) and separately counts presentation reasoning tokens. This pick would activate per-token semantic-string tracking on the default path and change default `model_thinking_tokens` receipt/log bytes. It is not needed for capped-thinking correctness; preserve the shipped fast-path and observational contract rather than silently changing it. | `src/models/common/qwen/frontend/output_session.cpp:157-184,360-371,534-541,592-601,756-763`; `src/serve/request_log.cpp:795`; existing thinking-budget tests remain unchanged. Preflight conflicts at that intentional fork fast-path and deleted shared-frontend/GLM interfaces. |
| Frontend | `2b13afa58d8973f7dfa4f1a8b873bb4071557290` — `fix(frontend): report token counts in context errors` | semantic | **Declined; P16.** It intentionally changes externally emitted context-overflow error text on every lane, including lower-bound tokenization messages, without an opt-in. That violates this lane's conservative output-byte boundary. The current overflow rejection remains intact; no claim is made that the diagnostic enhancement already exists. | Read upstream frontend/processor, request-contract, and Engine error-message diffs. Current `frontend.cpp:314-316` and `processor.cpp:940-943,1070-1073` retain rejection. Preflight also conflicts with absent GLM/shared-frontend organization. |
| Serve | `45f742877550d550d4359ecdb370a06cdaaf0aac` — `fix(serve): wait for admission before starting streams` | semantic | **Already present; do not replay.** `92d56958d` adds the fork's repeatable, non-consuming `wait_until_started` and invokes it during streaming preparation, before HTTP adapters commit SSE. Errors/cancellation are still HTTP errors and output events remain queued for `run()`. Do not rename the existing public contract or replace durable submit hooks. | `include/ninfer/engine.h:51-56`; `src/runtime/engine/engine_core.h:466-496`; `src/serve/generation_service.cpp:487-495`. Existing engine and serving contracts are retained. |
| Serve | `a062e0165ef2c26e433b92c639b5484539a48765` — `fix(serve): allow port reuse after listener shutdown` | textual (code clean) | **Applied as `6cd762e7d`.** Linux explicitly sets `SO_REUSEADDR` rather than httplib's default `SO_REUSEPORT`; retain active-listener exclusivity and all keepalive/user-timeout settings. Other platforms keep `httplib::default_socket_options`. Keep obsolete issue-checklist deleted. | `src/serve/http_transport.cpp:108-122`; `tests/test_http_transport.cpp` TIME_WAIT predecessor/successor matrix and active-port exclusion; target `ninfer_http_transport_test`. The new Linux cases use ephemeral loopback ports, never a fixed port. Preflight's sole conflict was the deleted checklist. |
| Config | `6dd2b1b4e3db715b5f68bd4163a86f1c16875a11` — `feat(config): make startup memory headroom configurable` | semantic | **Declined; P16.** Upstream replaces the 1-GiB context-policy allowance with a 512-MiB default, a TOML/config-schema option, and new distributed preparation wiring. The fork already has an explicit `admission_headroom_bytes` floor override, which is not equivalent to this planning allowance. Preserve 640/768-MiB native admission contracts; do not import excluded TOML, unified context, or rank-executor APIs. | `include/ninfer/types.h:47-62,165-166`; upstream options/schema and worker changes reviewed. Preflight shows deleted TOML/config/preparation paths and type/startup conflicts. No startup budget or default changed. |
| Media | `58733a86f1696282aec8e96be1b7c77370afc46b` — `fix(media): align JPEG decoding with Pillow` | semantic | **Already present; do not replay.** P14's `66301968c` ports Pillow-compatible JPEG decoding with preprocessing-identity versioning. The decoder source/header and retained JPEG fixture bytes have zero diff from this upstream fix. Preserve later fork media hardening. | `src/media/decode/jpeg.cpp`; `tests/test_media_decode.cpp:160-234`; `git diff --stat 58733a86 752dd887 -- src/media/decode/jpeg.cpp src/media/decode/jpeg.h tests/fixtures/media/jpeg.h` produced no differences. Existing bounded media tests exercise RGB references, orientations, rejection and recovery. |
| Build | `1e902680d6b845534c09500553706fda75f225a3` — `fix(build): complete container dependencies` | textual | **Applied as `7ebc8e4dc`; missing scratch exclusion only.** JPEG/io_uring build/runtime dependencies and explicit serving standard-library includes already exist. Add `.agent-tmp` to `.dockerignore`, which was still missing. Keep Git provenance context, native headers, and fork `ea74e391d`'s build-only driver stub isolation. Reject upstream's `/build/lib` copy, which requires its excluded shared-ops/core build refactor. README, Dockerfile and serving header resolve entirely to their existing fork versions. | `.dockerignore:1`; retained `Dockerfile:16-18,26-31,64-66` and `src/serve/generation_service.h:18-20`; existing container tooling tests cover Docker contracts. Preflight conflicts in all four candidate files. Container execution was not performed. |
| Tools (last/stretch) | `bc70d7c872df188183265d2016ac23244a6911ec` — `feat(tools): stream native tool call arguments` | semantic | **Declined; P16.** A 60-file feature changes tool parser transactions, Engine events, CLI, and all three public streaming protocols. It changes successful wire/event bytes without a per-lane opt-in and crosses the fork's Responses/durable-session contracts. Not a fixes-only cherry-pick. No parser/streaming subset was silently imported. | Upstream full file/stat inventory and grammar/tool dependencies reviewed; preflight conflicts in public types, tool parser/grammar, Responses/Anthropic rendering, and tests. No streaming feature proof claimed. |

## Preflight output-tree identities

All preflights used the unchanged `N` baseline. The three admitted changes touch disjoint code/test
paths, so their preflight resolutions compose without changing one another's implementation.
A conflict-tree ID is only an audit handle for Git's simulated merge, not a passing source tree.

| Candidate | `merge-tree` output tree | Exit |
| --- | --- | --- |
| `4ec903a0` | `8000c00db5445978d1129addfeed8a7e360dcf8f` | 1 |
| `9822c38d` | `381e7a2fa003477b90714a5934e0cf7bca08e038` | 1 |
| `1407c62f` | `09eaffd9acc07b3b458e453530d248b62bd03044` | 1 |
| `d602894a` | `cbd680bfd0c3c918c014cfd69623cb0368c728f7` | 1 |
| `77241ffb` | `6e477aa36f88576df82e25d6ce79d7aac41c6862` | 1 |
| `2cf04452` | `a31898075e25cb7913c75ce4669f291ed7524549` | 1 |
| `704fa68d` | `2d199d4330299348d27c9ca9f5009d121431a33a` | 1 |
| `419d637d` | `e9a8473b031035ae33c66007472a9e31ca943277` | 1 |
| `2b13afa5` | `03a697a1cd783222ad67fa88b7073c3111002c2f` | 1 |
| `45f74287` | `70d49d6bafaba3e9bd9273f5f40f0762faa5f8ef` | 1 |
| `a062e016` | `0c3e06aabb9a3f01deac17942b02d6787a0182e9` | 1 |
| `6dd2b1b4` | `e06d1d116e92224421764b3f7c8b005942df55f0` | 1 |
| `58733a86` | `845b1ae9c5d34a3706904c4cd78098cff2f1a4dc` | 1 |
| `1e902680` | `b1d6ec8b43d0e606c08fa61438e9722e9ea2f61b` | 1 |
| `bc70d7c8` | `5427b4f7fd88362878907a63ad8bb7f2c24b01c4` | 1 |

## Output-byte impact by shipped lane

These are source-based impact assessments, **not GPU parity results**. No GPU was accessed.
For the admitted workspace fix, every non-throwing offset and byte count is unchanged; only
recovery after checked overflow changes. The listener fix affects Linux socket lifecycle,
not request rendering, tokenization, sampling, math, or SSE payload construction. Excluding
untracked agent scratch changes only container input selection, not product source or runtime.

| Candidate / disposition | RTX 5090 / sm_120a | RTX 4090 / sm_89 | RTX 3090 / sm_86 |
| --- | --- | --- | --- |
| `d602894a` applied | [INFERENCE] No generated/prompt byte change; failure-atomic host layout | [INFERENCE] Same | [INFERENCE] Same |
| `a062e016` applied | [INFERENCE] No payload/token change; Linux port reuse/exclusivity changes | No implementation change: non-Linux socket branch retained | No implementation change: non-Linux socket branch retained |
| `1e902680` applied subset | [INFERENCE] No runtime byte change; excludes agent scratch from container build context | No native packaging/source behavior change | Same |
| `4ec903a0` already present | No new source or byte change | Same | Same |
| `9822c38d` already present | No new grammar or byte change; relaxed JSON whitespace already shipped | Same | Same |
| `704fa68d` already present | No new publication or byte change | Same | Same |
| `45f74287` already present | No new admission or byte change | Same | Same |
| `58733a86` already present | No new pixel/prompt byte change; Pillow decode already shipped | Same | Same |
| `1407c62f` declined | No shipped change; compiler-mask equivalence unproven if taken | Same | Same |
| `77241ffb` declined | No shipped change; migration/ownership port would need qualification | Same | Same |
| `2cf04452` declined | No shipped change; capacity/admission effects if taken unproven | Same | Same |
| `419d637d` declined | No shipped change; would change thinking-counter receipt bytes and activate default semantic tracking | Same | Same |
| `2b13afa5` declined | No shipped change; would change default HTTP context-error bytes | Same | Same |
| `6dd2b1b4` declined | No shipped change; would change default planning headroom and fit semantics | Same | Same |
| `bc70d7c8` declined | No shipped change; would change tool streaming/event bytes | Same | Same |

## Qualification ownership and limits

- `build.yml` push CI provides bounded sm_120a compilation and the existing GPU-free contracts.
  Its unchanged bounded target/CTest list does not run the new memory or HTTP transport cases.
- One manual `build.yml` run with `full_build=true` must succeed at the final L5 head. It builds
  the complete sm_120a apps/test closure, including both changed test binaries; it does not
  run model/GPU tests or claim native sm_89/sm_86 compilation.
- The separately owned native-compile workflow includes `ninfer_memory_test` and
  `ninfer_http_transport_test` in both sm_89/sm_86 matrices. Per integration ownership, that
  workflow lands first and this branch second; its integration-head run supplies native proof
  for `d602894a` and native no-regression compilation for `a062e016`'s non-Linux branch.
- Final N15 still requires the RTX 5090 T1 output-byte parity gate, **84/84**, against the shipped
  v0.14.0 reference and the unchanged RC union on the exact candidate binary. Host CI cannot
  establish those GPU claims. Native quality/fit gates remain with their release lanes.
- No unified context pool (`2705a728`, `049d5fb4`, `1fcc6dc5`, `bea5a04e`), TOML startup
  configuration (`4126ecf4`), executor/session restructuring, Spark, GLM5Next, TP2, MoE, or
  qwen4 expansion is imported. No local fixed-port smoke or fleet job was run.
