# tools/bench

Maintainer orchestration for the public `ninfer_bench` throughput tool, serving corpus/concurrency
runners, and the external Serve TTFT client. Correctness is owned by the affected suites under
[`tests/`](../../tests/README.md).

## Fork harness on upstream master

The fork's runtime source base is upstream master `64d10fbf`. Keep the upstream
TTFT, corpus, concurrency, and public `ninfer_bench` implementations; their
diagnostics and scheduling coverage supersede the older fork versions.

The port adds the SM120 profiling/controller scripts, `run_profile_request.py`,
the fixed A/B campaign summarizer, and `summarize_ncu_q4.py`. A profile packet
binds a clean source commit, artifact SHA-256, prepared binary/script/corpus
checksums, native `sm_120a` cubins, and the selected toolchain image. Preparation
does not execute GPU workloads. Capture runs the independent Q4/Q5 correctness
tests before the performance-counter permission gate, then collects public
Q5 cold-cache eager/Graph timings, Q4 T=1/T=4 NCU captures, an MTP3 native round
timing, and a measured public-Engine MTP3 Nsight Systems capture.

Master has no fork-only post-mixer candidate API. The scripts do not pass
`--candidate`, `--activation-state`, `--route`, or `--describe` to its Q4/Q5
benchmarks. The native round target is `ninfer_qwen3_5_mtp_round_bench`; only
`ninfer_bench` accepts `--profile-measured`. The old private C16 versus split2,
Q4-produced activation, and chain-cost conclusions are not inferred from the
new public measurements.

Use the selected Python 3.11 profiling environment and an explicitly authorized
GPU lease. The standalone controller wrappers have their own production
stop/restore contract; do not run them inside a separately managed overnight
window or use them to bypass its holder/container/port ownership rules.

```bash
python3 -m unittest discover -s tests -p 'test_sm120_*.py'
python3 -m unittest discover -s tests -p 'test_release_package.py'
python3 -m unittest discover -s tests -p 'test_lifecycle_container.py'
python3 -m unittest discover -s tests -p 'test_agent_protocol.py'
```

`tools/smoke/serve_contract.py` accepts an optional `--api-key-file` and retains
upstream's output-budget checks alongside exact greedy stream/non-stream output,
finish-reason, and token-accounting parity. Cache-hit counts are validated for each
request but need not match: the repeat can reuse a prefix that the cold request
computed.
`tools/smoke/agent_protocol.py` defaults to the authenticated **product** battery:
status identity, request-correlated private reuse, cross-session Responses
isolation, tool arguments/results, JSON-schema output, and streaming parity.
Its explicit `--contract public` mode exercises vanilla upstream's public
protocols, tool round trip, Responses lineage/delete behavior, JSON schemas on
Chat and Responses, and streaming. A public receipt never claims product
identity or session-isolation coverage. Product status failures never trigger a
fallback to the public battery.

```bash
python3 tools/smoke/agent_protocol.py --contract public \
  --base-url http://127.0.0.1:18160 --model q38-ninfer
python3 tools/smoke/agent_protocol.py --contract product \
  --base-url http://127.0.0.1:18160 --model q38-ninfer --api-key-file /run/secrets/ninfer-key
```

Release packaging remains fail-closed on clean, matching build provenance.
`ninfer`, `ninfer-serve`, and `ninfer_bench` support `--version` without loading
an artifact or creating a GPU context. CMake verifies the configured Git commit
and clean/dirty state before compilation. A source archive without Git metadata
cannot claim to be clean; the lifecycle builder materializes a detached Git
repository. The Docker context must include that metadata, not a private
linked-worktree `.git` pointer.

The branch build workflow uses the NVIDIA apt CUDA 13 toolkit, cached ccache,
host-only CTest selections, and bounded `sm_120a` core/Q4 object compilation.
Complete runtime builds are separate weekly/manual jobs. Windows MSVC
configure-only activates when the PortableCore `vcpkg.json` manifest is present;
it is not a Windows build or native-GPU qualification. No CI job runs GPU
inference. The runtime publication workflow requires an explicit immutable,
qualified master-compatible `base_image`; it does not reuse the shipped old
runtime base digest automatically.

### Receipt and consumer changelog

- `ninfer_agent_protocol_smoke` schema **2** replaces schema **1**'s
  structured-output-rejection expectation with Chat/Responses JSON-schema
  success. It adds `contract`; product identity flags remain false in public
  mode, and unexercised isolation paths/statuses are null.
- Status consumers pin `ninfer_server_status` schema **2**, preserving fork
  field names and required identity/counter assertions.
- Serving corpus and TTFT diagnostics consume request-log schema **30**.
  Benchmark result schemas and TTFT diagnostic calculations are unchanged.
- SM120 preparation manifests use schema **2**, capture manifests schema **3**,
  and newly produced public Q4 shape/capture summaries schema **2**.
  Historical Q4 schema **1** input remains readable with its original strict
  kernel/route checks; it is never relabeled as a master capture.
- Profile toolchain image revision **3** adds the master hwloc/liburing build
  dependencies and invalidates older cached profile images.
- Linux release/package identity schema **1**, Windows schema **2**, lifecycle
  receipts, and runtime-image publication receipt schema **3** remain unchanged.

## External Serve TTFT

[`ttft/README.md`](ttft/README.md) defines the black-box latency benchmark. The measurement runner
uses frozen text/media requests and public streaming protocols without calling Engine. A separate
controller manages the fixed Qwen3.8-27B NVFP4/FP8 Serve profiles and fresh-process isolation.

```bash
python3 tools/bench/run_serve_ttft_campaign.py --campaign resource --samples 5
```

The controller chooses the profile, starts and stops Serve for every sample, runs the external
client, stages the NVFP4 artifact once in `/dev/shm`, stores raw/progress/Serve artifacts below
`profiles/bench/ttft/`, records structured per-request Serve diagnostics, and writes Markdown,
JSON, and CSV summaries. The case catalog, exact profiles, TTFT boundary, and fixture qualification
are documented in the dedicated README.

## Corpus baker

`ninfer_bench` benchmarks prefill at an exact length by slicing the first `P` token ids of a
committed corpus, so the corpus must be real, in-distribution text (not random tokens) and at
least as long as the largest prefill you want to run. `make_bench_corpus.py` bakes that corpus
offline with a local Hugging Face Qwen3.6 tokenizer.

Outputs (committed):

```text
bench/fixtures/bench_corpus.ids            whitespace-separated decimal token ids (exactly --tokens)
bench/fixtures/bench_corpus.manifest.json  tokenizer id, token count, and source description
```

Content sources:

- Built-in curated multi-domain prose (Chinese / English / code / math) — the default. It is
  encoded WITHOUT the chat template or special tokens, then tiled (paragraphs rotated each cycle)
  and truncated to exactly `--tokens`. Repetition only fills length; because prefill/decode
  throughput is token-count / bandwidth bound, it does not bias the numbers.
- `--source-text <file>` (repeatable) — tokenize your own long meaningful text instead, e.g. a
  downloaded public-domain book or a concatenated document set, for genuinely diverse very long
  content. The committed default is `~64k` tokens; raise `--tokens` and/or pass `--source-text`
  for more.

The binary slices `[0:P]`; the manifest is provenance only.

## Requirements

Install the tokenizer dependencies into the active Python environment:

```bash
pip install -r tools/bench/requirements.txt
```

The tokenizer is loaded locally only; the tool never downloads from the network. Pass
`--tokenizer-path` or set `NINFER_TOKENIZER_PATH`.

## Regenerate / check

```bash
# Regenerate the committed corpus from the built-in bank (default 65536 tokens).
python3 tools/bench/make_bench_corpus.py \
  --tokenizer-path /path/to/local/Qwen3.6-27B/tokenizer \
  --tokens 65536

# Bake from your own downloaded/assembled text instead (kept local; not committed).
python3 tools/bench/make_bench_corpus.py \
  --tokenizer-path /path/to/local/Qwen3.6-27B/tokenizer \
  --tokens 131072 --source-text /path/to/book.txt

# Check that the committed .ids and its descriptive manifest agree; no tokenizer or source needed.
python3 tools/bench/make_bench_corpus.py --check
```

`--tokens` is the exact committed corpus size and the ceiling on prefill length; increase it (and
optionally use `--source-text`) to benchmark longer prefills, memory permitting.

## NInfer performance matrix

`run_ninfer_bench_matrix.py` runs the layered public-Engine `ninfer_bench` matrix against the native
`.ninfer` artifact and stores its local reports under `profiles/bench/`. Its defaults are:

```text
artifact: out/qwen3_6_27b.ninfer
binary:   build/bench/ninfer_bench
corpus:   bench/fixtures/bench_corpus.ids
```

The matrix treats MTP `k=3` with the optimized proposal head as the primary path, keeps `k=0` and
`k=5` as controls, and sweeps `k=0..5` on representative context-decode cases. Decode-bearing cases
cover CUDA Graph and eager execution; prefill-only cases vary prompt length and prefill chunk.

```bash
# Configure the benchmark targets once; they are off in the default public build.
cmake -S . -B build -DNINFER_BUILD_BENCHMARKS=ON

# Inspect commands without running the model.
python3 tools/bench/run_ninfer_bench_matrix.py --preset core --dry-run

# Main run. Builds build/bench/ninfer_bench first, then writes JSON and summary.csv.
python3 tools/bench/run_ninfer_bench_matrix.py --preset core

# Longer run that adds 32k/64k prompt and context-decode points.
python3 tools/bench/run_ninfer_bench_matrix.py --preset full

# Run only the MTP draft-window sweep.
python3 tools/bench/run_ninfer_bench_matrix.py --preset full --suite mtp_sweep
```

Default outputs:

```text
profiles/bench/ninfer-<preset>-<timestamp>/
  commands.sh
  manifest.json
  json/<suite>/<case>.json
  logs/<suite>.<case>.stderr.txt
  summary.csv
  summary.json
```

Use `--resume` to skip completed JSON reports in an existing `--output-dir`, and `--preset smoke`
for a minimal script/runner check. `--no-build` uses the binary supplied by `--bench` without
building it.

Each raw report must satisfy the current [`ninfer_bench_report` contract](../../bench/README.md#reports).
The flattened summary and schema-v4 matrix manifest carry native facts from the report:
architecture, public name, actual formats, prefill signature, artifact,
load/read/upload/staging values, Engine memory arenas including the non-additive Vision layout
inside the unified workspace and measured CUDA Graph memory deltas, per-test planned logical and
allocator-observed workspace peaks, KV capacity and
payload, configured proposal head and graph mode, phase timings and throughput, and speculative
rounds/drafts/acceptance/fallbacks. The matrix manifest is descriptive and records the commands and
selected local inputs; it does not make repository state part of report validity.

## Serving corpus benchmark

[Published coverage and model results](../../docs/performance.md) identify the recorded runs.
The [serving methodology](../../docs/performance/methodology.md) owns workload definitions,
metric boundaries, aggregation, comparison rules, and publication format. This section describes
runner usage and output files.

`run_serve_corpus.py` accepts explicit `--artifact LABEL=PATH` entries. Labels identify report groups;
the selected artifact supplies the architecture, public name and weight bindings.
Omitting `--mode` selects MTP0 and MTP3; repeat `--mode` to select a subset. Use `dflash7` for
Qwen3.6-35B-A3B DFlash K=7 and `dflash2_7` for Qwen3.8-27B DFlash2 K=7, with companion weights
in the selected artifact. `--sampling greedy` selects exact argmax; the default is stochastic.
Run commands with a selected Python 3.11 interpreter, as in the model-page reproduction entries.
Both serving runners accept `--kv-dtype bf16|int8|fp8|nvfp4|k8v4` (default: `int8`).
Specify it explicitly when recording a new campaign; the Qwen3.8 performance profile uses
`--kv-dtype fp8`. The runner verifies the loaded KV representation and records the selection in
its results. A serial run cannot resume records collected with a different KV dtype.

The serial runner writes `run.jsonl`, `summary.csv`, `summary.md`, and per-server logs under
`server/`. JSONL contains the completed requests and responses; CSV/Markdown contain fixture and
category summaries. The output directory is supplied explicitly with `--output`.

Its schema-v8 result and flattened summaries retain the KV dtype, actual `prefill_signature`, request Host
exposure, and decode Host/Device-wait time per round received from the schema-v30 serving records.
Request exposure is a latency distribution value and is never summed across concurrent requests;
worker aggregation uses the serving `throughput.host_work` interval deltas. The stochastic route pins its complete
temperature/top-p/top-k/min-p/presence/frequency profile explicitly, so model-default changes do
not alter the measurement method.

## Concurrent serving benchmark

`run_serve_concurrency.py` selects `--suite decode-saturation` or `--suite corpus-makespan`.
Their distinct time boundaries and workload dispatch are defined in the
[serving methodology](../../docs/performance/methodology.md#workloads-and-measurement-boundaries).
Repeat `--concurrency` to select C points; each point starts a fresh server. The point report
records the actual Engine configuration, automatic KV capacity, shuffle seed where applicable,
dispatch method, and per-request positions.
Both serving runners disable prefix reuse and set extra Device state and Host context capacity to zero.

Schema-v4 outputs include `points/*.json`, `server/*.jsonl`, and combined `summary.json`, `summary.csv`, and
`summary.md`. C=1 corpus runs also write complete responses in `corpus/<point>/results.jsonl` and
per-request phase summaries in that directory; older campaigns may have only point reports and
server logs. Historical model pages identify the report directory associated with each table.

```bash
python3 tools/bench/run_serve_concurrency.py \
  --artifact qwen3_6_27b=out/qwen3_6_27b_nvfp4.ninfer \
  --kv-dtype int8 \
  --mode mtp3 --suite decode-saturation \
  --concurrency 1 --concurrency 2 --concurrency 4 \
  --decode-tokens 8192 \
  --output profiles/bench/concurrent-decode

python3 tools/bench/run_serve_concurrency.py \
  --artifact qwen3_6_27b=out/qwen3_6_27b_nvfp4.ninfer \
  --kv-dtype int8 \
  --mode mtp3 --suite corpus-makespan \
  --concurrency 1 --concurrency 2 \
  --output profiles/bench/concurrent-corpus
```

Use `--kv-capacity auto` when the fixed corpus needs more shared KV than the default 262,144-token
pool. A point is intentionally not resumable: combining fragments from separate server processes
would not preserve either a steady interval or one continuous makespan.
