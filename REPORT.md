# Harness port — upstream master `64d10fbf`

## Delivery state

- Branch: `rebase/harness`. Accepted executable/tooling source: `3faeff4427e306549c13c01048ce882b3e13fcb3`; upstream base: `64d10fbf9f8e6bfc79ab0570b94674c5b9997929`. A report-only successor does not change the exercised code.
- Sources: shipped RTX 5090 fork `1302d639` and its native-compatible packaging/provenance API. No community code copied. Upstream TTFT, corpus, concurrency, scheduler, grammar and public benchmark implementations remain authoritative.
- All four assigned acceptance boundaries passed: vanilla-master/v3 `serve_contract.py`, vanilla-master/v3 explicit-public `agent_protocol.py`, real compiled Linux package creation, and branch CI. This is not G1/G2/G3, native Windows, or full product/private-session qualification.
- Shared CMake/app provenance registration was supplied and authorized by Integrator. Infra prerequisites `1422cb510`, `ee39c4626`, `8f3ab12e6`, and supervised-helper checkpoint `8700b2b21` are consumed. The two helper files include the complete prerequisite Windows-path/Docker-routing/slot interface, not undefined partial cherry-pick hunks.

## Changed surfaces

| Surface | Files / anchors | Result |
|---|---|---|
| Verified build identity | `cmake/BuildIdentity.cmake:1`, `cmake/verify_build_source.cmake:21`, `include/ninfer/build_info.h:1`, `src/core/build_info.cpp:1` | Same nine build identity fields as shipped; clean Git commit/state rechecked before product compilation; no-Git archives cannot claim clean provenance. |
| Artifact-free version entry points | `apps/cli/main.cpp:259`, `apps/serve/main.cpp:32`, `bench/inference/ninfer_bench.cpp:162`; shared root/core CMake | `--version` returns before artifact parsing or GPU-context construction. Binaries still need the driver ABI at dynamic-load time. |
| Public/product smoke clients | `tools/smoke/serve_contract.py:35,148`, `tools/smoke/agent_protocol.py:104,282,719`, `tests/test_agent_protocol.py:1` | Auth helpers, strict stream/output/budget accounting, schema-success tests, typed tools/results, Responses lineage; explicit public versus default product coverage. |
| Profiling/controller tools | `tools/bench/run_sm120_mtp3_profile.sh:1`, `run_sm120_q4_mtp3_profile.sh:1`, `summarize_ncu_q4.py:163`; other added controller/A-B tools; `tests/test_sm120_*.py` | Current public Q4/Q5 CLI, native round timing, measured Engine Nsight capture, immutable preparation/counter gates, and host regression coverage. |
| Current log consumers | `tools/bench/run_serve_corpus.py:89`, `tools/bench/ttft/diagnostics.py:12`, `tests/bench/ttft/test_measurements.py:299` | Request-log schema 30; TTFT diagnostic calculations unchanged. |
| Packaging/lifecycle | `tools/release/package.py:1`, `tools/lifecycle/ninfer_container.py:824`, pinned io_uring seccomp JSON; release/lifecycle tests | Full Linux and native-layout APIs carried; deterministic archives, checksums, SPDX, measured identity and confined container operations preserved. |
| CI/publication tooling | `.github/workflows/build.yml:1`, `.github/workflows/publish-runtime-image.yml:23,66` | CUDA 13 apt toolkit, ccache, bounded 120a compilation and host tests; separate full builds; manifest-gated MSVC configure; explicit immutable runtime base input. |
| Documentation | `tools/bench/README.md:7` | Invocation/coverage authority and complete receipt/consumer changelog. |

## Exercised evidence

Lease: `20261025T030535Z-F1-harness-public-contract-460272`, holder `F1`, priority 2, 15-minute cap. The self-contained configure/build/package/smoke batch exited **0 after 46 seconds**, with **0 owned leftovers and 0 unowned containers left running**. Persistent incremental build: **7 completed steps**. The preceding clean build completed **636 steps**.

Environment: `ov-build-env:master-64d10fbf-r2`, immutable image ID `sha256:e7f34ca2f41ecea14da5cce0b2a30cae6b0877e3b3eb371bd92d103bca1121f1`; external common helpers at source checkpoint `8700b2b21cef2e76ed7a3270ffaa9b4a30f80af8`. The smoke server was the unpatched vanilla-master baseline, with the read-only published v3 artifact, BF16 KV, 8,192 context/KV tokens, one lane, Vision, greedy decoding and thinking disabled. Server and observers ran inside the same container on loopback port **18160**; no invalid external WSL HTTP observer. Server tests left `CUDA_VISIBLE_DEVICES` absent. Artifact-free binary/package tests used the real driver ABI with zero visible CUDA devices, not a runtime driver stub.

| Check | Observed result |
|---|---|
| `serve_contract.py` | PASS: count_tokens **15**; Chat output **2**, stop; Responses input **19**, output **2**; image prompt **85**; Anthropic `end_turn`. Responses stream/store/get/input-items/continue/delete and exact greedy stream reconstruction completed. |
| `agent_protocol.py --contract public` | PASS, schema **2**: Chat and Responses JSON-schema decoding both true; typed forced tool/result-array round trip; stream/non-stream stop with **2** completion tokens; first turn, continuation, **2** forks, parent delete **200**, subsequent get **404**, surviving descendants retrieved and continued. Tool-result continuation legitimately ended at its output budget (`length`). |
| Product/private coverage labeling | Public receipt explicitly reports status/session identity false and private/isolation paths/statuses null. Default product battery remains fail-closed on schema-2 identity, real scheduler/reuse counters and session isolation; it never auto-falls back to public. |
| Compiled release identity/package tests | **6 passed in 0.337 s**, including all three selected app `--version` commands without a model or visible GPU. |
| Actual `package.py` output | Linux tar, checksum list and SPDX created from clean source with build profile `harness`, Release, architecture `120a`; receipt schema **1**, dirty false. |
| GitHub branch CI | [Run 40156691541](https://github.com/alphastorm/ninfer/actions/runs/40156691541) **success** at the accepted source SHA. Linux CUDA/host/bounded-120a job: **2 min 43 s**. Windows job exercised manifest detection only because PortableCore was absent on this isolated branch; weekly/manual full build skipped. |
| Host profiling tests | **30 passed**, 13.078 s. |
| Host lifecycle tests | **15 passed**, 0.428 s. Private repository-owned temporary fixtures preserve, rather than bypass, writable-ancestor validation. |
| Host protocol tests | **12 passed**, 0.003 s; additionally the explicit public-parent error regression passed in isolation after its final change. |
| TTFT/corpus Python tests | Initial local run: 56 passed, two schema-29 fixture defaults failed; both exact failures passed after migration to 30. Full targeted tests then succeeded in branch CI. No diagnostic assertion was removed. |

Actual package files:

| Asset | SHA-256 |
|---|---|
| `ninfer-qwen38-rtx5090-v0.13.0-linux-x86_64-cuda13.1.tar.gz` | `921b5be125e8998de2c39d671d6730e65fede4f5f18428e284a20e2b6b92a9d2` |
| `ninfer-qwen38-rtx5090-v0.13.0-linux-x86_64-cuda13.1.spdx.json` | `a3497d59021a42de4c0cae876613edc7e90387e7e2bc6cae175fa72fad310b4c` |

Complete sanitized primary outputs and subject/image/helper/lease bindings: `local://HarnessAcceptance.json`. Private raw logs and packages remain in the holder's evidence workspace, not Git. No credentials, model text, checkpoint payloads or private host paths are committed.

## Commands exercised

`SRC` is a real detached checkout of the accepted source SHA; `BUILD` is the holder's persistent build directory; `ROOT` is its workspace; `COMMON` is the operator-provided helper directory; `V3_MODEL` is the read-only v3 `qwen3_8_27b.ninfer`. Private absolute paths are intentionally not recorded.

```bash
python3.13 -m unittest discover -s tests -p 'test_sm120_*.py'
python3.13 -m unittest discover -s tests -p test_release_package.py
python3.13 -m unittest discover -s tests -p test_lifecycle_container.py
python3.13 -m unittest discover -s tests -p test_agent_protocol.py
python3.13 -m unittest discover -s tests -p test_agent_protocol.py -k responses_lineage
# pytest ran in an isolated ignored environment; the host interpreter lacked it.
python -m pytest -q tests/bench/ttft/test_measurements.py tests/test_serve_corpus.py
python -m pytest -q \
  tests/bench/ttft/test_measurements.py::test_global_intervals_include_unowned_demotion_and_shutdown_tail \
  tests/bench/ttft/test_measurements.py::test_missing_runtime_fields_remain_explicit

NINFER_BUILD_HOLDER=F1 NINFER_BUILD_ENV_IMAGE="$R2_IMAGE_ID" \
  "$COMMON/build.sh" "$SRC" "$BUILD" \
    -DNINFER_BUILD_BENCHMARKS=ON -DNINFER_BUILD_PROFILE=harness \
    -DNINFER_UPSTREAM_BASE_SHA=64d10fbf9f8e6bfc79ab0570b94674c5b9997929 \
    -- ninfer ninfer-serve ninfer_bench
# In a real-driver container with CUDA_VISIBLE_DEVICES empty:
NINFER_TEST_BINARY="$BUILD/apps/ninfer" \
NINFER_TEST_SERVE_BINARY="$BUILD/apps/ninfer-serve" \
NINFER_TEST_BENCH_BINARY="$BUILD/bench/ninfer_bench" \
  python3 -m unittest discover -s tests -p test_release_package.py
python3 "$SRC/tools/release/package.py" --source "$SRC" \
  --ninfer "$BUILD/apps/ninfer" --ninfer-serve "$BUILD/apps/ninfer-serve" \
  --output-dir "$ROOT/package-$SOURCE_SHA" --release-version v0.13.0 \
  --platform linux-x86_64-cuda13.1 \
  --upstream-base-sha 64d10fbf9f8e6bfc79ab0570b94674c5b9997929 \
  --release-head-sha "$SOURCE_SHA" --build-profile harness
# The following server and clients ran together inside the GPU container:
"$VANILLA_BUILD/apps/ninfer-serve" "$V3_MODEL" \
  --host 127.0.0.1 --port 18160 --model-id q38-ninfer \
  --max-context 8192 --kv-capacity 8192 --max-concurrency 1 \
  --prefill-chunk 1024 --kv-dtype bf16 --vision --greedy --no-thinking \
  --log-stats-interval-ms 0
python3 "$SRC/tools/smoke/serve_contract.py" \
  --base-url http://127.0.0.1:18160 --model q38-ninfer --health-timeout 180
python3 "$SRC/tools/smoke/agent_protocol.py" --contract public \
  --base-url http://127.0.0.1:18160 --model q38-ninfer --health-timeout 30
```

## Compatibility decisions and closed failures

- Preserve exact greedy output, finish reason, generated/reasoning/total token accounting and output budgets. Validate each cached-token count, but do not equate cold and hot request cache telemetry: the live failure showed identical prompt 19/output 2/stop with cache **0 → 10**. Both clients and a negative regression cover this class.
- Use each protocol's real thinking wire: Chat `enable_thinking=false`, Responses `reasoning.effort=none`. Responses schema and lineage fixtures reject the Chat-only flag. Public missing-parent continuation requires master's `response_not_found`; product requires shipped `previous_response_not_found`, confirmed by ServePort. Neither profile accepts both codes as a fallback.
- Keep receipt version changes explicit: agent smoke 2 (schema success and coverage labeling), status consumers 2, request log 30, profile preparation 2/capture 3/public Q4 manifest+summary 2, profile image revision 3. Historical Q4 schema-1 input remains strictly readable. Linux package/identity schema 1, Windows schema 2, lifecycle and image-publication receipt schemas are unchanged.
- Drop only obsolete fork-private benchmark APIs/conclusions (post-mixer candidate/C16/activation-state chains); use master's public Q4/Q5 and Engine routes instead. No runtime product family was removed. No real NCU/Nsight performance or kernel-parity result is claimed by these host profiling tests.

## Integration ownership / remaining program gates

- K7 audit delivered at `local://K7-status-consumers.md` to ServePort, EngineCache, Integrator, Contracts, ProductOps and NativeOps: field-to-consumer table, mandatory non-null attestation/UUID/restart/reuse/performance fields, optional diagnostic fields, and atomic schema migration points.
- NativeOps owns subsequent Windows-specific `package.py` changes and native controllers/packaged smoke. ProductOps owns strict product startup-key and save-receipt consumers. The default product battery still needs the integrated status-2/private/durable server proof; the vanilla public receipt intentionally cannot claim it.
- Actual MSVC configure/build/native qualification, G1 restart, G2 decode parity, G3 interaction/failure matrix and G4 review remain their owners' gates. The Windows CI job is implemented but its configure phase was not exercised on this isolated pre-PortableCore branch. No tags, releases, GHCR pushes, workflow dispatches, upstream posts or production-controller operations were performed.
