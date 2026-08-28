# tools/bench

Maintainer orchestration for the public `ninfer_bench` throughput tool and the external Serve TTFT
client. Correctness/parity tooling lives separately under [`tools/parity`](../parity).

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

Each raw report must be `ninfer_bench_report` schema v13. The flattened summary and schema-v3 matrix
manifest carry native names from the report: selected target, canonical `weights_id`, artifact,
load/read/upload/staging values, Engine memory arenas including the non-additive Vision layout
inside the unified workspace and CUDA Graph allowance, per-test planned logical and
allocator-observed workspace peaks, KV capacity and
payload, configured proposal head and graph mode, phase timings and throughput, and speculative
rounds/drafts/acceptance/fallbacks. The matrix manifest is descriptive and records the commands and
selected local inputs; it does not make repository state part of report validity.

`run_serve_corpus.py` runs both registered targets and both published MTP0/MTP3 suites when both
artifacts are supplied. Pass one `--artifact` to select a single target and `--mode mtp0` or
`--mode mtp3` to run only that suite. The 35B-A3B-only `--mode dflash7` route runs the same
decode corpus with DFlash block=8 (`k=7`) and the optimized proposal head. Add
`--sampling greedy` to force exact argmax while retaining the same fixtures and repetition count.
Its schema-v6 result and flattened summaries retain the canonical `weights_id`, request Host
exposure, and decode Host/Device-wait time per round received from the schema-v17 serving records.
Request exposure is a latency distribution value and is never summed across concurrent requests;
worker aggregation uses the serving `throughput.host_work` interval deltas. The stochastic route pins its complete
temperature/top-p/top-k/min-p/presence/frequency profile explicitly, so model-default changes do
not alter the measurement method.

## Concurrent serving benchmark

`run_serve_concurrency.py` measures two separate concurrency properties through real loopback
Chat Completions requests:

- `decode-saturation` submits one long-decode wave and uses only complete one-second intervals in
  which every decode round has exactly the configured batch size. Ramp-up, prefill, and drain
  intervals are excluded.
- `corpus-makespan` shuffles the existing mode-specific corpus once with the fixed seed `20260811`,
  then runs that same order with exactly `N` persistent client workers. A worker submits the next
  request only after its current response completes, and makespan ends when the final response has
  been read. Request bodies are sent in shuffled-order sequence while response waits remain fully
  concurrent, removing client-thread arrival races without serializing inference.

Each concurrency point starts a fresh server because its execution graphs and memory plan are
startup-fixed. Prefix reuse is disabled, startup and warmup are outside both measurements, and the
runner writes per-point JSON, raw serving JSONL, and combined JSON/CSV/Markdown summaries.
The point report records the shuffle seed, dispatch method, shuffled position, and canonical corpus
position for every request.

```bash
python3 tools/bench/run_serve_concurrency.py \
  --artifact qwen3_6_27b=out/qwen3_6_27b_nvfp4.ninfer \
  --mode mtp3 --suite decode-saturation \
  --concurrency 1 --concurrency 2 --concurrency 4 \
  --decode-tokens 8192 \
  --output profiles/bench/concurrent-decode

python3 tools/bench/run_serve_concurrency.py \
  --artifact qwen3_6_27b=out/qwen3_6_27b_nvfp4.ninfer \
  --mode mtp3 --suite corpus-makespan \
  --concurrency 1 --concurrency 2 \
  --output profiles/bench/concurrent-corpus
```

Use `--kv-capacity auto` when the fixed corpus needs more shared KV than the default 262,144-token
pool. A point is intentionally not resumable: combining fragments from separate server processes
would not preserve either a steady interval or one continuous makespan.

## Guarded remote GPU profiling

`run_remote_sm120_mtp3_workflow.sh` is the sole operator entry point for the deferred RTX 5090
packet. It stages and invokes `run_sm120_mtp3_workflow.sh` as the target-side implementation. The
controller entry point runs one fixed, versioned sequence; it accepts no payload command or shell
fragment:

1. Require a completely clean `PROFILE_SOURCE_SHA` worktree descended from
   `PROFILE_UPSTREAM_BASE_SHA`, exact artifact SHA-256, a profiling image distinct from the runtime
   candidate, `/usr/bin/docker`, the configured `default` Unix-socket context and Docker Desktop
   Linux daemon ID,
   the Docker `nvidia` runtime, host `nvidia-smi`, and the executable maintained controller.
2. Create an operation-owned Docker config containing only `{}`. This prevents inherited
   `credsStore=desktop.exe` routing. The workflow preserves the already-configured Docker Desktop
   WSL integration; it never toggles the integration, changes a Docker context, starts a daemon, or
   attempts daemon recovery.
3. Reuse or build the operation's profiling image from the exact configured CUDA/Nsight base and
   checksum-pinned CPython 3.11.11 source. Then configure and build the Q4, Q5, post-mixer,
   MTP-round, and numerical-gate targets in that pinned container with no GPU and no network.
   `prepare` records exact source, upstream base, artifact, toolchain, binary, CMake, CUDA, and
   native `sm_120a` identities.
4. Enter `run_gpu_profile_lease.sh` synchronously. The lease revalidates the exact Docker route,
   host GPU tools, candidate names/port, and incumbent/rollback restoration route before stopping
   production. Immediately before the outage it also runs `nvidia-smi` in a no-network container
   from the exact profiling image ID, while production is still healthy. Its fixed payload requires
   the lease's transient live-process marker and independently verifies that the exact incumbent is
   stopped. The lease isolates the payload process group,
   confirms every profiling candidate is gone before restart, and restores production on success,
   failure, HUP, INT, or TERM.
5. Run `capture` from the read-only prepared build. The Q4/Q5 numerical gates run first. The cold Q5
   NCU launch is then the counter-access gate: the packet must contain a nonempty `.ncu-rep` and CSV
   without `ERR_NVGPUCTRPERM` before timings, Nsight Systems, remaining Q5 captures, or the nested Q4
   packet can run. Every NCU/NSYS capture is bounded by the configured timeout.

`run_sm120_mtp3_profile.sh prepare` and `capture` are internal workflow phases, not operator
commands. Preparation occurs before the production stop; the GPU lease contains no compilation.
`run_sm120_q4_mtp3_profile.sh`, `run_long_prefill_mtp3_cycle.sh`, and `run_profile_ab.sh` remain
versioned payloads for their own explicitly selected experiments.
Do not construct another launcher, heredoc, inline Python shim, or remote shell script around any
payload or target workflow.

Copy `profile_lease.conf.example` to the controller once and replace every `REPLACE_ME` with
identities from maintained-controller receipts and current target state. Do not put the populated
file in Git. The controller workflow requires a clean committed source, creates a unique remote
operation, transfers and verifies a complete Git bundle and generated target config, runs target
preflight, executes the synchronous target workflow, then retrieves receipts. Every
`PROFILE_BUILD_DIR`, `PROFILE_PREPARE_DIR`, `PROFILE_RESULT_DIR`, and `PROFILE_DOCKER_CONFIG` is
generated fresh. `PROFILE_TOOLCHAIN_IMAGE_ID`, `MODEL_SHA256`, `PROFILE_SOURCE_SHA`, Docker endpoint,
and Docker daemon ID are equality gates, not descriptive metadata. The configured profiling base
and `TOOLCHAIN_IMAGE` runtime candidate must exist locally. The controller builds the final
profiling image only when its tag is absent, pins its resulting image ID into the operation config,
and requires it to differ from the runtime image. The profiling image must contain CMake, Ninja,
CUDA 13.1, NCU, NSYS, and Python 3.11; bootstrap and `prepare` check those commands before
compiling and before any production stop.

After Docker-route recovery, prove the route without an outage by running the controller's
`check` mode. It stages a fresh exact source/config operation, pins or builds the profiling image,
and exercises target preflight plus the GPU-container admission while production remains running.
It writes a controller receipt but creates no profiling result directory and never requests a
production stop:

```bash
tools/bench/run_remote_sm120_mtp3_workflow.sh check /absolute/path/profile.conf
```

The scheduled capture is exactly one synchronous controller command. It performs source staging,
target preflight, preparation, lease, payload, restoration, and receipt retrieval; do not detach it
or invoke its target phases separately:

```bash
tools/bench/run_remote_sm120_mtp3_workflow.sh run /absolute/path/profile.conf
```

Completion requires lease `exit-status.txt` equal to `0`, `production-restored-at.txt`, final
production inspect/status receipts, `sm120-mtp3/ncu/counter-access-verified.txt`, and
`sm120-mtp3/packet-complete.txt`. `SIGKILL` and host power loss cannot execute a shell trap; use the
already-maintained production controller for recovery. After a Windows reboot, first sign in to the
operator's interactive Windows session and start Docker Desktop; wait for the pinned Linux daemon
and existing WSL integration, then use the maintained controller. Never toggle the integration,
context, or daemon as a recovery action.
