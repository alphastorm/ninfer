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

Bounded secondary-lane builds exposed a public/internal `ChatMessage` name-lookup collision in the new history-contract signature and an out-of-order C++ designated initializer in the new marker fixture. Both are corrected (explicit `ninfer::ChatMessage`; declaration-order marker fields); changed-source incremental build and corpus execution are next. No CUDA compilation or tests ran on the workstation. Superseded queued primary-lane jobs were cancelled before execution when the lead opened the two-slot CPU lane.

Planned targeted build/test targets: `ninfer_tool_call_parser_test`, `ninfer_qwen_frontend_test`, `ninfer_qwen_tool_constraints_test`, `ninfer_json_schema_test`, and the existing JSON/tool independent-schema oracles. No formatter, linter, project-wide suite, live server, port binding, or model run is part of this slice.

## Remaining integration gates

- Merge the shared declaration provider before the frontend implementation.
- ServePort owns Responses custom wire objects, replay kinds, and declaration metadata. The frontend corpus proves its side of that bridge; the integrated wire corpus remains the serve slice's proof.
- K2 provider dependencies: frozen Contracts `746b29d2` consumed as `93446717`; Persistence's header-only codec provider `48235fc1` consumed as `fa22a42c` (only additive includes conflicted). EngineCache owns forwarding the cached frontend value through the approved engine adapter.
- Whole-program performance, durable fanout, native Windows build, real-model constrained generation, and frozen-candidate G4 are not asserted by this report.
