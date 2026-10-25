# ServePort: retained Responses, identity and publication boundaries

## Source and ownership

- Branch `rebase/serve`; upstream base `64d10fbf9f8e6bfc79ab0570b94674c5b9997929`; verified source `7fa1c952`. Holder `B1`.
- Identity/publication/name port is separate from custom tools: initial serving commits `70dc649f`, `85278519`, `06f5842b`; custom wire/replay commit `3ff7321d`; native submission-epoch fixes `2e3dde75`, `c4e7e90f`. Shared prerequisites are explicit cherry-picks, not a fork-file transplant.
- Owned changes are serving implementation and focused tests. Shared source registration, host-state target factoring, field-only keep-warm declarations and the app stop-watcher composition were supplied or expressly authorized by Integrator. The app composition is `8233bd95`.
- Design follows frozen K1–K7 and approved RepoPrompt prep `prep-v0-13-0-upstream-re-DF08C8`. Shipped S1 serving behavior was adapted to upstream's immutable context DAG. No community-fork code was copied. No release, tag, image publication or production-runtime change was performed.

## Implemented behavior

- `src/serve/client_identity.cpp:65`: strict authenticated explicit session/request digests; constant-time credential/capability comparison; domain-separated prompt-cache-key naming and API-principal scope. Both SHA-256 domains include one terminating NUL. Valid unauthenticated cache keys do not establish private ownership; conflicting key/session inputs are rejected. Raw keys are not logged.
- `src/serve/openai_responses_store.h:98`: private reads/deletes require the matching capability; foreign and absent capabilities are indistinguishable from missing private records. Removed unscoped store accessors and migrated callers. Retained lineage includes immutable context, previous response, optional thinking policy, explicit latest endpoint and normalized persisted order.
- `src/serve/openai_responses_store.h:115`: move-only guards pin the revision before disk loading; atomic restore prepares replacement state before committing Engine state. False/throwing commit, stale revision, active publication, foreign collision, malformed lineage or capacity refusal expose no partial DAG and do not evict unrelated sessions. Revision bookkeeping is retired when no records/publications/guards need it.
- Publication is registered before submission and ended by insertion or RAII abandonment. `generation_service.cpp:450` captures the real native publication epoch immediately after submission, before consuming the handle. `openai_responses_http.cpp:72` permits request-ticket fallback only with persistence disabled; tagged durable records require a nonzero native epoch. New live records outrank restored relative orders after restart.
- Responses creation restores a scoped missing parent, or restores on the first stored keyed request when no live endpoint exists. `store:false` does not advance named state. Inherited/current input private boundaries are carried to the shared frontend without replacing upstream cache markers, thinking rules or call normalization.
- `src/serve/request.h:25`: one `kMaxToolNameLength = 128` supplies request defaults and Chat, Responses and Anthropic validation. ASCII naming rules are preserved; 71-character MCP-style names and the 128-character boundary are covered. JSON-schema format names intentionally retain their unrelated 64-character limit.
- `src/serve/openai_responses_response.cpp:469`: Responses supports top-level custom declarations, `custom_tool_call`, `custom_tool_call_output`, aggregate/SSE input events and opaque raw replay. JSON-looking input, quotes, newlines and markup remain byte-preserved raw text. Function/custom result-kind mismatches are rejected. `tests/test_openai_responses.cpp:1198` is the custom wire/replay corpus.
- `src/serve/checkpoint_identity.cpp:99`: strict typed K2 fingerprint-v2 serialization/validation, canonical effective-config projection and explicit rejection of legacy/missing/extra/dirty/unknown or inconsistent identity. Cache/host admission budgets, secrets, paths and ordinals are excluded. Template/tokenizer/native-layout identities have no fabricated filename/default fallback. Diagnostic startup `weights_id` now means verified whole-artifact SHA-256.
- Secure opaque IDs use OS entropy, never a PRNG fallback. HTTP auth/header handling and endpoint-specific errors preserve upstream error envelopes while adding identity isolation. Request-log schema 30 adds startup identity/correlation; status schema 2 preserves required non-null observations and lists eight explicitly unavailable diagnostics rather than manufacturing zeroes.
- `src/serve/stop_event.h:12`: protected Windows named-event watcher, retry-until-accepted callback and joined destruction; non-Windows rejects the unsupported facility. Early HTTP stop is remembered before listen. The app installs the watcher before startup work. This is not a durable-save acknowledgement.

## Exact remaining K3/K4 integration boundaries

- `GenerationCheckpointHooks` (`generation_service.h:30`): bind `set_checkpoint_tag` to `runtime::CheckpointEngineAccess::set_checkpoint_tag`, and `publication_epoch` to `CheckpointEngineAccess::publication_epoch`. Both are mandatory for enabled persistence. E1/native implementation must supply the actual session scopes, private capture and native epoch, not an HTTP request counter.
- `SessionCheckpointCallbacks::restore` (`http_server.h:43`, callback type `openai_responses_store.h:169`) returns the complete `DurableImportResult`. The provider captures Engine session epoch and `OpenAIResponseRestoreGuard` before reading; verifies the A1 checkpoint and actual K2 identities; uses `CheckpointEngineAccess::prepare_import`; then calls `restore_session` with the prepared transaction's no-fail `commit`. Store-side refusal reasons remain typed; an external Engine refusal retains its original K7 reason. HTTP performs a scoped re-read, never publishes unverified records.
- The export provider must stage via `CheckpointEngineAccess::export_session`, wait for the matching Responses publication outside the Engine execution lock, take `snapshot_session`, and use A1's mandatory `publish_export` gate with `CheckpointEngineAccess::publish_export`. Hashing/writing stays off-lock; final current-pointer publication is gated by tag/epoch. These native/file implementations are not substituted with serving no-ops.
- `SessionCheckpointCallbacks::final_save` is called synchronously after HTTP draining, including the remembered-stop path; false fails listen. Its provider must save every relevant session and retain detailed K7 receipts/controller shutdown-report evidence. A queued save is not an acknowledgement. Native shutdown-report emission/controller behavior is not proven here.
- `http_server.cpp:550` fails closed before listening when a checkpoint root is configured without restore, tag, epoch and final-save providers. Default-disabled persistence remains usable. Tests use explicit in-memory fake providers; no fake production provider is installed.
- Frontend consumes shared `ninfer::ToolKind` and `kMaxToolNameLength`: serving passes raw Custom `arguments_json` and public-history kind, and canonical declarations carry outer `x-ninfer-tool-kind: custom`. Frontend alone strips the marker, wraps raw text once at rendering and propagates parsed kinds. Merge `3ff7321d` atomically with its matching Frontend implementation. No `src/serve/tool_call_parser.*` was carried.
- DurableStore's `ninfer-responses-cbor/1` codec must retain both call and result kinds and optional thinking. Actual loaded frontend/native-layout authorities must populate the strict K2 value before enabling persistence. Real restart, corruption/MAC/identity failure matrix, native scope isolation and durable publication acknowledgement remain integration gates, not claims of these fake-based host tests.

## Verification and reproducible commands

- The initial secondary CPU/WSL dependency build was interrupted by an added 900-second wrapper deadline at 426/659 Ninja edges; no CTest ran. Its EXIT trap printed zero on TERM, which is not success evidence. The sole owned orphan container was explicitly stopped; the incremental proof uses the same persistent build directory, no such deadline and nonzero TERM/INT handling. Queued primary-host attempts were cancelled before execution.
- The resumed compiler attempt exposed an unqualified `ContextCacheHints` in the identity fixture (77 seconds, exit 1); `7fa1c952` qualifies the existing type as `ninfer::ContextCacheHints`. No production behavior was weakened and that failed attempt is not test evidence.
- **PASS at `7fa1c952`:** `ninfer-serve` plus all eleven focused host-test executables built; CTest reported **11/11 passed, zero failures, 0.65 seconds**. The complete incremental batch recorded exit 0 and **163 seconds**, using observed `build_slot=0` of the secondary two-slot/12-worker CPU helper. Image: `ov-build-env:master-64d10fbf-r2-cpu-local` (ID `sha256:62a4feee8f308cde61095355ff11977986ecce388ad4f057c23014950b681326`). Driver-linked CPU tests loaded the real driver with empty device selection; this is not GPU/model qualification or selected-UUID attestation.

Paths below are role variables, deliberately not machine-specific receipts: `$SOURCE` is a real detached Git checkout of `7fa1c952`; `$BUILD` is its persistent build directory; `$COMMON` is Infra's helper directory. Source/build/cache mounts use the Windows drive, not an unintegrated WSL/UNC filesystem.

```sh
export NINFER_BUILD_HOLDER=B1
export NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2-cpu-local
"$COMMON/build-wsl.sh" "$SOURCE" "$BUILD" \
  -DNINFER_BUILD_PROFILE=rtx5090 \
  -DNINFER_UPSTREAM_BASE_SHA=64d10fbf9f8e6bfc79ab0570b94674c5b9997929 \
  -- ninfer-serve ninfer_openai_schema_test ninfer_openai_responses_test \
  ninfer_openai_responses_store_test ninfer_anthropic_schema_test \
  ninfer_http_server_stop_test ninfer_stop_event_test ninfer_http_error_handler_test \
  ninfer_request_log_test ninfer_serve_options_test ninfer_http_transport_test ninfer_metrics_test
"$COMMON/run-in-env-wsl.sh" --gpu --env CUDA_VISIBLE_DEVICES= \
  --name serve-host-tests --mount "$SOURCE" --mount "$BUILD" -- \
  ctest --test-dir "$BUILD" --output-on-failure \
  -R '^(ninfer_openai_schema_test|ninfer_openai_responses_test|ninfer_openai_responses_store_test|ninfer_anthropic_schema_test|ninfer_http_server_stop_test|ninfer_stop_event_test|ninfer_http_error_handler_test|ninfer_request_log_test|ninfer_serve_options_test|ninfer_http_transport_test|ninfer_metrics_test)$'
```

## Deliberately changed, dropped and unproven

- No old unscoped access aliases, process-PRNG IDs, legacy K2 fingerprint acceptance, request-counter durable-order fallback or fabricated status capacity values. Old fork persistence formats are not silently migrated. Whole-artifact diagnostic identity replaces the removed upstream logical weights ID.
- Upstream context DAG/scheduler/DFlash2/grammar/frontend architecture stays authoritative. No fork monolithic Program/resource accounting, NVFP4 KV cache, standby-pinning retries or serving-local parser was ported.
- Namespaced custom tools and Chat/Anthropic custom wire types remain unsupported as before. Custom text/Lark/regex format declarations remain prompt guidance, not a claim of new grammar enforcement.
- No GPU model run, restart proof, C2 pressure test, decode benchmark, Windows/MSVC build, Windows ACL execution, controller-package qualification, G1/G2/G3 gate or formatter/linter/project-wide suite was run by this slice. On Linux, the stop-event test checks unsupported-platform refusal, not Windows ACL behavior.
