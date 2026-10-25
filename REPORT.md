# Frontend parity slice

- Branch: `rebase/frontend`; base: `f90a3fb`; shared declarations consumed from `7c6ba673a` as `8c9f44a1d`.
- Scope: Qwen shared frontend, its corpus, and the explicitly delegated `9822c38d` JSON/xgrammar whitespace port. No serving-layer parser, scope-carrier changes, scheduler changes, or GPU/model changes.
- Source preparation: supplied RepoPrompt export `oracle-plan-2026-10-24-174946-prep-v0-13-0-upstrea-1d64.md`, frontend contract §3.5; no second preparation. Program G4 review remains lead-owned.

## Implementation

- Declared non-string type mismatches now restore the complete tool region as text, including preceding otherwise-valid calls. Unknown parameter names and unsupported schemas retain upstream legacy inference and diagnostics; empty non-string arguments retain upstream omission.
- Basic/constrained non-strict duplicates no longer use last-wins. They return atomic text fallback with `duplicate_parameter`; semantic mismatches report `schema_mismatch`, not a grammar disagreement. Strict schema validation, no-tools selection, and completed-call retention across genuine interruption remain upstream-owned.
- Type-union compilation is iterative; non-strict schemas are neither copied nor serialized merely to derive scalar types. Parameter-delimiter search bounds scans to the next close and never repeats an impossible malformed suffix search.
- Custom declarations use the agreed canonical function schema plus outer `x-ninfer-tool-kind:"custom"`. Template rendering strips that metadata and wraps typed raw custom history as `{input: raw}` exactly once. Shared `ToolKind` propagates through history and generated calls. Custom parser input is opaque, including standalone closing-tag text; the constrained custom grammar requires the sole `input` parameter and preserves its bytes.
- History-only automatic output retains known tool kinds; conflicting historical kinds are not callable. Explicit `tool_choice: none` still selects upstream's no-tools grammar (lead-approved divergence from the old parser's ability to emit historical calls under None).
- Automatic system/developer-frontier and tool-boundary `SharedStablePrefix` markers already exist upstream. No insertion was ported. Added coverage proves leading-role sequences, coincident explicit/automatic deduplication, shared-principal scope retention, and the automatic-disable policy.
- Ported the behavioral portion of upstream `Neroued/ninfer@9822c38d7dba8d7f9d1bd5382d314ef24a4b34e8`: JSON structural/outer whitespace and const/enum container punctuation, with mask preview/commit/discard and combined tool/body tests. The vendor helper is attributed in place. Did not carry the unrelated upstream issue checklist or dev-only fixture changes. No community-fork code copied.
- Added the separately approved K2 loaded-frontend provider: `Frontend::durable_identity()` returns the immutable cached typed identity. It hashes the effective selected template while the exact loaded source is still owned, hashes tokenizer bytes, and hashes a six-resource inventory once during construction. Each inventory record frames name length/name, original byte length, and SHA length/SHA with unsigned 64-bit big-endian lengths after a domain/NUL prefix; raw resource payloads are not recopied into the inventory. Vision uses a separately domain-separated inventory of its two loaded configs or `disabled`. Codec spellings come from source-owned `kParserCodec` and Persistence's `kIdentityCodec`. Getter calls do not reopen paths, inspect resource views, or hash data.
- Added an explicit CPU diagnostic mode, `ninfer_qwen_frontend_test --resource-probe DIR [TEMPLATE]`, for the lead-assigned artifact/template golden investigation. It loads real frontend resources, requires count/prepare token-count agreement, and emits the exact rendered prompts/token IDs and template/tokenizer SHA-256 values for the two existing Chinese prompt cases. No model weights or GPU execution are required.

## Fork corpus disposition

Reference: shipped `test_tool_call_parser.cpp` (17 cases) and custom replay expectations from `test_responses_schema.cpp`.

| Fork case | Disposition | Upstream test / retained behavior |
|---|---|---|
| `test_single_call` | upstream-already | `test_untyped_parameters`; prefix trimming, name, legacy numeric/string inference. Call-ID generation assertion intentionally moved out of frontend scope: protocol adapters own wire IDs. |
| `test_multiple_calls_and_json_values` | upstream-already | `test_multiple_calls`; ordered calls, objects/arrays/booleans, ordinary text. |
| `test_malformed_falls_back_to_text` | upstream-already | strict-structure and incremental fallback cases; all bytes restored. |
| `test_suffix_after_tool_falls_back_to_text` | upstream-already | strict-structure and fork atomic corpus; non-tool suffix rejects the whole region. |
| `test_configured_name_limit` | upstream-already | explicit 64/128/129 parser limits retained; protocol default unification belongs to shared/serve owners. |
| `test_declared_strings_are_not_json_sniffed` | upstream-already | declared-string corpus; raw numeric/boolean/null/quoted text, CRLF framing, meaningful spaces, string-admitting unions. |
| `test_declared_non_string_values_are_json_decoded` | upstream-already | declared JSON types; integer-as-number, booleans, objects, arrays, null, scalar unions. |
| `test_declared_types_normalize_or_fall_back` | fixed | ASCII-case booleans and exact numeric/integer classification already existed; mismatch publication fixed. Bytewise and every two-chunk boundary covered. |
| `test_composed_type_contracts` | upstream-already | nested anyOf/oneOf scalar union, raw-string preference, unsupported-branch legacy inference. Rejection on mismatch fixed with the preceding class. |
| `test_deep_supported_union_without_recursive_compilation` | fixed | 8,192-level supported anyOf compilation; original definition depth retained, no recursive type walk or unnecessary non-strict schema copy/dump. |
| `test_balanced_markup_and_atomic_fallback` | fixed | balanced nested parameter markup already existed; semantic whole-region fallback fixed. Embedded function/tool closes, unbalanced delimiters, duplicate names, UTF-8/CRLF, vertical-tab suffix and non-format prefix preserved. |
| `test_large_malformed_region_completes_without_reparse` | fixed | 22,000 malformed parameter opens and 1,000 false tool-region transitions; bounded suffix search and exact bytewise fallback. |
| `test_unknown_schema_keeps_legacy_inference` | upstream-already | missing/invalid types, partial or ambiguous unions, and undeclared parameters keep legacy inference. |
| `test_custom_tool_raw_input` | fixed | typed raw custom input, standalone closing tags, JSON-looking payload, wrong outer parameter rejection, basic custom grammar, raw output-session propagation. |
| `test_custom_tool_kind_survives_history_only_generation` | fixed | historical raw custom kind, unknown-name rejection, conflicting-kind atomic fallback; Auto history-only output. Generated calls under explicit None are intentionally not carried, as approved by lead to preserve upstream no-tools constraints. |
| `test_incremental_filter_valid_tool` | upstream-already | unified output decoder suppresses possible terminal tool suffix; no second stream filter. |
| `test_incremental_filter_fallback` | upstream-already | malformed/ordinary/partial marker bytes restored, plus continuation withholding retained. |

The fork's no-declarations parser fixtures are expressed with explicit untyped tool declarations because the upstream frontend contract owns the active name set. This does not reintroduce unknown callable names. Duplicate/conflicting declarations continue to be rejected at upstream request-contract construction; historical kind conflicts fail closed rather than reinterpreting a custom tool as a function.

## Verification

All required CPU corpus groups pass on the approved two-slot CPU lane, using a real detached Git clone, persistent incremental build, ccache, and the build helper's exclusive `flock` slot. Build image: `ov-build-env:master-64d10fbf-r2-cpu-local`; observed GCC 13.3.0 / CUDA compiler 13.1.115. Runtime containers mount the real driver with `--gpu --env CUDA_VISIBLE_DEVICES=`; these are CPU proofs, not GPU/model qualification.

| Test | Passing source | Time |
|---|---|---|
| `ninfer_tool_call_parser_test` | `9b59806eccde4b04081a877961f5ceaca441f2c8` | 0.03 s |
| `ninfer_qwen_tool_constraints_test` | same | 0.07 s |
| `ninfer_qwen_tool_schema_oracle_test` | same | 0.58 s |
| `ninfer_json_schema_test` | same | 0.05 s |
| `ninfer_json_schema_oracle_test` | same | 2.85 s |
| `ninfer_qwen_frontend_test` | `395c486696769f3330a724f7078788a8ad7c5ca3` | 0.74 s |

The first five passing tests were not rerun after the frontend-only fixture correction. The final frontend run also proves immutable loaded identity, exact custom-history wrapping, raw generated kind propagation, both non-strict output routes, marker parity, and existing preview/commit/continuation behavior.

Resolved qualification failures: public/internal `ChatMessage` lookup collision (explicitly qualify `ninfer::ChatMessage`); marker-fixture aggregate field order (use declaration order); expected custom declaration reordered by sorted `json` (use the template's existing `ordered_json`). No production change was made to hide the latter fixture mismatch. The final detached operation exits `FRONTEND_QUALIFICATION_EXIT=0`. The Windows log reader could not decode Chinese output; the original UTF-8 log and JSON proof were retrieved directly without rerunning tests.

Exact commands executed by the detached qualification script (variables denote the established source/build/helper/staging directories; private absolute locations are intentionally omitted):

```sh
export NINFER_BUILD_HOLDER=B3
export NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2-cpu-local
export NINFER_BUILD_JOBS=12
"$helper/build-wsl.sh" "$src" "$build" -- ninfer_tool_call_parser_test ninfer_qwen_frontend_test ninfer_qwen_tool_constraints_test ninfer_json_schema_test
"$helper/run-in-env-wsl.sh" --gpu --env CUDA_VISIBLE_DEVICES= --mount "$src" --mount "$build" -- ctest --test-dir "$build" --output-on-failure -R '^(ninfer_tool_call_parser_test|ninfer_qwen_frontend_test|ninfer_qwen_tool_constraints_test|ninfer_json_schema_test|ninfer_json_schema_oracle_test|ninfer_qwen_tool_schema_oracle_test)$'
```

After the fixture-only correction, the same helper built only `ninfer_qwen_frontend_test` and CTest selected only `^ninfer_qwen_frontend_test$`. The independent real-resource comparison then ran:

```sh
"$helper/run-in-env-wsl.sh" --gpu --env CUDA_VISIBLE_DEVICES= --mount "$src" --mount "$build" --mount "$stage" -- python3 "$stage/frontend-golden-reference.py" "$build/tests/ninfer_qwen_frontend_test" "$src" "$stage/v3-resources" "$stage/frontend-golden-proof.json" "$(git -C "$src" rev-parse HEAD)"
```

Evidence files: `qualify-v3.log` (five passing corpus groups), `qualify-v4.log` (frontend pass and terminal success), `frontend-golden-proof.json` (exact resource hashes, rendered prompts, token IDs, independent reference comparisons, and template-epoch diff). No formatter, linter, project-wide suite, live server, port binding, or model run was performed. Superseded queued primary-lane jobs were cancelled before execution when the CPU lane opened.

## Artifact prompt golden cause

- Reported baseline failure is accepted without rerunning it: vanilla `64d10fbf` / published v3 artifact, `ninfer_qwen3_5_prefix_real_test`, thinking-prompt golden.
- The 16/18 goldens originate at `d720e1cea6` (2026-07-15), in the old Qwen3.6 test path; the later move did not regenerate them.
- Extracted published template: 9,712 bytes, SHA-256 `a497db9e663941e6f7a05307c2bafa8374c12e142ad1fbea6229888d41f1ab16`; byte-identical to maintained `tools/chat_templates/qwen3_8.jinja`. Actual tokenizer SHA-256: `0997f410c57a1f4e53b09e4be8f4a172d90edd9564368fb0847030937229b9f3`.
- Actual shared C++ Frontend `count_tokens()` and prepared token counts agree. Decoded prompt bytes match independent Python Jinja2 for all four comparisons: current thinking **58**, current no-thinking **18**, prior Qwen3.6 template with the same actual tokenizer **16/18**. Prior template SHA-256: `7145316bcc837ddc11971fb9fdca588971502e707b9e6df83af0e9b683b2bd39`.
- Cause: stale model/template-epoch golden, not incorrect rendering or v2-to-v3 conversion. Infra separately proved all six frontend resources equal the shipped v2 resources; the upgrade itself did not introduce this preamble.
- Only the thinking golden changes, 16 → 58, with an explanatory source comment and actual-count diagnostics. The no-thinking golden remains 18. The full real GPU prefix scenario is left to the integration gate; no GPU pass is asserted here.

Exact rendered thinking diff (the additional system preamble accounts for all 42 tokens):

```diff
--- prior-qwen3_6
+++ published-v3-qwen3_8
@@ -1,3 +1,5 @@
+<|im_start|>system
+Reasoning effort is set to xhigh. Please think carefully through the task, validate key assumptions, consider plausible alternatives, and prioritize correctness, consistency, and clarity in the final answer.<|im_end|>
 <|im_start|>user
 你好，简单介绍一下你自己。<|im_end|>
 <|im_start|>assistant
```

## Remaining integration gates

- Merge the shared declaration provider before the frontend implementation.
- ServePort owns Responses custom wire objects, replay kinds, and declaration metadata. The frontend corpus proves its side of that bridge; the integrated wire corpus remains the serve slice's proof.
- K2 provider dependencies: frozen Contracts `746b29d2` consumed as `93446717`; Persistence's header-only codec provider `48235fc1` consumed as `fa22a42c` (only additive includes conflicted). EngineCache owns forwarding the cached frontend value through the approved engine adapter.
- Whole-program performance, durable fanout, native Windows build, real-model constrained generation, and frozen-candidate G4 are not asserted by this report.
