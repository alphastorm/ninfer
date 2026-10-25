# Infrastructure slice

## Source and ownership

- Base: upstream master `64d10fbf`; branch `rebase/infra`. Vanilla measurements use an unmodified upstream archive, not the ported integration tree. Changes are limited to container build inputs and reusable environment helpers.
- `Dockerfile` re-expresses the dependency completion from upstream dev `1e902680`: JPEG and io_uring build/runtime packages and the build-only CUDA driver stub. It retains hwloc and adds ccache/Python for the reusable `build-env` target. The lead approved explicitly installing Git in that development stage for provenance, with the runtime stage unchanged. Dev's shared-library `/build/lib` copy is intentionally omitted because this master builds static libraries. Build-identity arguments are forwarded to the integration-owned CMake contract; archive builds are not represented as clean product builds.

## Shared helper contract

The operator installs the two executable scripts from `tools/env/` together in the shared `common/` directory. Set `COMMON`, `SRC`, and `BUILD` to the operator's absolute paths and `NINFER_BUILD_HOLDER` to the holder that owns the active exclusive lease. **All builds and GPU commands run inside that lease.** The helpers do not acquire a second lock or change production.

```sh
export NINFER_BUILD_HOLDER=infra
"$COMMON/build.sh" "$SRC" "$BUILD"
"$COMMON/build.sh" "$SRC" "$BUILD" -DBUILD_TESTING=OFF -- ninfer-serve
"$COMMON/run-in-env.sh" --mount "$SRC" --mount "$BUILD" -- \
  ctest --test-dir "$BUILD" -R '^ninfer_public_api_test$' --output-on-failure
"$COMMON/run-in-env.sh" --gpu --mount "$SRC" --mount "$BUILD" \
  --read-only-mount "$MODEL_DIR" --seccomp "$COMMON/ninfer_io_uring_seccomp.json" \
  --env "NINFER_TEST_ARTIFACT=$MODEL" -- \
  ctest --test-dir "$BUILD" -L real --output-on-failure
```

- Defaults: `ov-build-env:master-64d10fbf`, Release/Ninja, `120a`, apps/tests on, benches off, `--parallel 16`, ccache launchers for C/C++/CUDA. Later CMake arguments override defaults; names after `--` select build targets. Use a distinct persistent build directory per holder and architecture. Source and build paths remain identical inside/outside the container.
- `run-in-env.sh` accepts `--gpu`, `--publish PORT`, `--volume NAME TARGET`, repeatable `--mount` / `--read-only-mount`, `--env KEY=value`, `--seccomp FILE`, and `--name SUFFIX`. It uses Docker's bridge by default; `--publish` maps only `127.0.0.1:PORT:PORT`, and published servers bind `0.0.0.0` inside the container on their holder's assigned port. Named volumes must belong to the caller (`ov-<holder>-...`). Containers are always `ov-<holder>-<suffix>`, auto-removed, run as the caller's UID/GID, and use a persistent 20 GB ccache. Image/cache overrides are `NINFER_BUILD_ENV_IMAGE` and `NINFER_CCACHE_DIR`.
- Integrated source transfer must retain real Git identity: clone/fetch the pushed slice or integration branch, then detach at its full commit SHA (or import a full Git bundle). Use a real detached repository, not a linked-worktree `.git` file pointing outside the Docker context; never copy unrelated repository metadata. A plain archive is valid only for this explicitly vanilla baseline. The Docker context retains `.git` for identity verification. `NINFER_SOURCE_DIRTY=auto` is not a clean-source override; Harness owns its absent-Git refusal.
- Docker Desktop's credential helper failed when resolving the Dockerfile frontend from a noninteractive WSL session. The image build uses a new, empty `DOCKER_CONFIG` directory for anonymous public-image resolution; no credentials are read, copied or changed. The failed attempt lasted 10.799 seconds and made no runtime changes.

## Qualification evidence scope

Receipts contain relative file identities, counts, hashes and metrics only. Private raw logs stay outside Git; no credentials, model text, checkpoint bytes, private host paths or GPU UUIDs belong in committed receipts. This slice's startup probe is a vanilla capacity observation, not product durability or release qualification.

## Environment proof

Upstream commit: `64d10fbf9f8e6bfc79ab0570b94674c5b9997929`. The initial build-env image was `sha256:502ca8b3465cb29fef4523a29fbaae5cece976e32f5524caf19e7ab05af35265`; its successful anonymous image build took **50.675 seconds**. The helper built all vanilla apps and tests with benchmarks off, `120a`, and `-j16` in **285.323 seconds**, exit zero. Lease job: `20261025T010304Z-infra-env-master-anonymous-210198` (338 seconds total, no leftover containers). This proves compilation, not GPU runtime validity of that initial image.

| Binary | Bytes | SHA-256 |
|---|---:|---|
| `apps/ninfer` | 302316680 | `9b07f7a06e718c3db69bb130f07fc4cb41bb12edcccdc4cbd533c0ea88a0a372` |
| `apps/ninfer-perplexity` | 302303160 | `aad246dc95761de9f90b5b79a4691f029b5c7a4476ebb918edcf3c59e54da631` |
| `apps/ninfer-serve` | 304492472 | `d7d90126c646b0d26517abedf17a05246a24af979b40f6bf6cf8eb905705f50d` |

The shared helper/environment announcement was delivered to all twelve peers after this proof succeeded. Exact measured build command was `NINFER_BUILD_HOLDER=infra "$COMMON/build.sh" "$SRC" "$BUILD"` against the vanilla source and persistent `infra/build-master` directory. `environment.json` records image/build exit codes, monotonic wall times and the binary hashes.

## Initial baseline: environment-invalid, not a product verdict

Lease `20261025T010906Z-infra-vanilla-ctest-214054` exposed two environment defects: the development-stage driver stub at a default CUDA library path displaced the real GPU driver (`cudaErrorStubLibrary`), and the Python template/schema oracles lacked Jinja2/jsonschema. Initial inventory was host **42 passed / 3 failed**, real-model **0 passed / 13 failed / 2 skipped**, other GPU **16 passed / 103 failed / 5 skipped**, explicit real loading exit **1**. None of the GPU failures is attributed to upstream or the product. Both missing-oracle packages and the isolated link-only stub fix were approved by the lead; all peers received a GPU-proof hold before the refresh.

The corrected Dockerfile puts the stub only at `/opt/ninfer-driver-stub/libcuda.so.1`, outside default/runtime search paths. Only `build.sh` configure/build invocations and the isolated Docker `build` stage expose that directory through `LD_LIBRARY_PATH`; `run-in-env.sh` does not expose it to GPU workloads. The development image explicitly includes `git`, `python3-jinja2` and `python3-jsonschema`; the runtime image does not.

## Corrected GPU-ready environment

`ov-build-env:master-64d10fbf-r2` is **`sha256:e7f34ca2f41ecea14da5cce0b2a30cae6b0877e3b3eb371bd92d103bca1121f1`**. The required `ov-build-env:master-64d10fbf` tag now aliases that same image. Its build took **49.582 seconds**; real `ninfer_device_test` passed in **1.298 seconds**, and the three previously missing-dependency CPU oracles passed in **2.709 seconds**. Lease: `20261025T011636Z-infra-env-r2-gpu-safe-225942`, exit zero, **54 seconds**, no leftover containers. All thirteen peers received the GPU-ready image and immutable ID immediately after proof. Pin `NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2` when producing receipts.

Exact corrective proof used `ctest --test-dir "$BUILD" -R '^ninfer_device_test$' --output-on-failure --timeout 60` with `--gpu`, then the three host oracles selected by `^ninfer_(chat_templates|json_schema_oracle|qwen_tool_schema_oracle)_test$` without `--gpu`. Receipt: `environment-r2.json`. Neither the source nor the compiled baseline binaries changed between the environment-invalid and corrected-image runs.

## Vanilla CTest command contract

The host-only selection is explicit because upstream does not label every CPU/GPU test. The following equivalent compact regex selects the same 45 tests as the receipt producer:

```sh
HOST_REGEX='^ninfer_(public_api|cpu_(system|task_pool|executor)|read_queue|host_timing|jinja|chat_templates|weight_bank|gdn_replay_records|artifact_(reader|backing|cpu_materialization)|scheduler|context_cost|resource_manager|kv_capacity|sampling_defaults|grammar|regex_choice|regex_choice_oracle|json_schema|json_schema_oracle|media_decode|prompt_input|pretty_logging|perplexity_evaluation|cli_options|openai_schema|openai_responses|openai_responses_store|anthropic_schema|serve_options|request_log|metrics|http_error_handler|http_transport|qwen_frontend|qwen_tool_constraints|qwen_tool_schema_oracle|tool_call_parser|qwen3_5_loading|qwen3_5_state_image_layout|bench_support|context_cost_measure)_test$'
ctest --test-dir "$BUILD" -R "$HOST_REGEX" --output-on-failure --timeout 90
ctest --test-dir "$BUILD" -L real --output-on-failure --timeout 180
ctest --test-dir "$BUILD" -LE real -E "$HOST_REGEX" --output-on-failure --timeout 90
"$BUILD/tests/ninfer_qwen3_5_loading_real_test" \
  --artifact "$MODEL" --vision --speculative dflash2 --proposal optimized
```

Each command runs in `run-in-env.sh` with source/build/model-parent mounts and the approved io_uring seccomp policy. The first phase has no GPU and sets `CUDA_VISIBLE_DEVICES=`; later phases use `--gpu` and `NINFER_TEST_ARTIFACT="$MODEL"`. The direct loading command is necessary because that upstream CTest entry does not pass `--artifact` and otherwise skips. Per-test status/timing is extracted from CTest JUnit into `baseline.json`; skipped or incompatible-artifact cases are not successful coverage.

## Corrected vanilla real-model baseline

Lease `20261025T011826Z-infra-vanilla-ctest-r2-228794` used the immutable r2 environment and unchanged upstream binaries. The real-model CTest phase took **189.272 seconds**: **7 passed, 6 failed, 2 skipped**. Direct explicit loading with Vision + DFlash2 + optimized proposal head passed in **5.728 seconds**.

| Real-model test suffix | Result | Observation / classification |
|---|---|---|
| `native_transactions_test` | Pass | 16.4513 s |
| `preemption_real_test` | Pass | 39.8965 s |
| `tools_real_test` | Pass | 18.6609 s |
| `score_real_test` | Pass | 10.8232 s |
| `vision_workspace_test` | Pass | 9.61869 s |
| `dflash2_real_test` | Pass | 26.7466 s; upstream defaults include batch 8 / draft window 15 |
| `dflash_prefill_real_test` | Pass | 12.6333 s |
| `prefix_real_test` | Fail | `artifact tokenizer/chat template changed the thinking prompt golden` |
| `grammar_real_test` | Fail | `KV write exceeds its page reservation` — genuine vanilla red, not a driver-stub failure |
| `agent_continuation_real_test` | Abort | `constraints require default EOS, text output, no custom stops, and one output language` |
| `moe_real_test` | Fail | Dense artifact supplied to the 35B MoE fixture; invalid load summary |
| `moe_offload_real_test` | Fail | Fixture's 16 GiB device budget cannot hold this dense artifact/resources; no MoE-offload coverage |
| `dflash_real_test` | Fail | Artifact has no old `dflash` component |
| `qwen3_5_loading_real_test` | Skip | CTest omits the required explicit artifact; separate direct loader passed |
| `qwen4_exp_loading_real_test` | Skip | No Qwen4/Flash-Next artifact/argument supplied; no coverage claim |

The first thirteen suffixes above belong to `ninfer_qwen3_5_`. All baseline reds/skips and their raw excerpts were shared with the integrator; the three substantive prefix/grammar/continuation boundaries were escalated to the lead. No failing golden or assertion was edited.

The corrected no-GPU host run passed **42/45**. Its remaining three failures (`openai_schema`, `request_log`, `http_error_handler`) were ELF linkage to `libcuda.so.1`, not failed assertions; the three Python oracles now passed. Their narrow follow-up injects the real driver but sets `CUDA_VISIBLE_DEVICES=` so the host tests cannot select a CUDA device. The runtime stub remains excluded.

That narrow follow-up passed, giving **45 passed / 0 failed / 0 skipped** aggregate host coverage. It is recorded separately in `baseline.json` so the initial missing-driver-library boundary is not erased. The closeout lease `20261025T014632Z-infra-host-baseline-closeout-304281` exited zero.

The supplementary nonreal-GPU sweep reached **94 of 124** completed tests before its **900-second** overall cap: **90 passed, 4 timed out, 0 assertion failures**, with 30 not completed (one interrupted at `ninfer_qsa_sparse_gqa_test`). This extra sweep is explicitly incomplete, not a full-suite pass and not substituted for the requested real-model inventory.

## Artifact upgrade evidence

The standard-library upgrader at the frozen upstream commit ran through the lease with separate output directories; no input artifact was modified. Production conversion took **19.566 seconds**, native conversion **17.531 seconds**, both exit zero. Artifact upgrade/hash job: `20261025T010442Z-infra-artifact-v3-identity-210593`, **86.675 seconds** measured work.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| Production v2 input | 20437336576 | `0634abb07024221de141456cf04a42ab74b18bc38e1b781c6eb2e062a467eec3` |
| Published v3 | 20437521664 | `81f924d440c27261d820c19a9f8d45794c5aee410f8a68bd358133fa8c0375da` |
| Production v2 upgraded locally to v3 | 20437571568 | `facba3bd318139c50899d4a0d74576837f73f0f498a461961467d56df10f4944` |
| Native v2 input | 18210531328 | `eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e` |
| Native v2 upgraded locally to v3 | 18210749936 | `5476dfd6b243332f1754a494469ba4a1c8206edba6518b61dba1e0f4338664e7` |

The native output is `models/v3/native-eec39564-upgraded/qwen3_8_27b.ninfer` relative to the operator's model root. The v3 reader validated its directory/framing: **1,118 tensors**, one file. The two production v3 artifacts each contain **1,184 tensors**; their dedicated chat templates are byte-identical, **9,712 bytes**, SHA-256 `a497db9e663941e6f7a05307c2bafa8374c12e142ad1fbea6229888d41f1ab16` (empty template diff).

Physical object IDs are intentionally different (`weight/000000`-style published IDs versus named upgrader IDs), and fused `parts` bindings plus Use auxiliaries can share one physical tensor. Comparing IDs directly is not payload evidence. The raw manifest comparison records **6,588 field differences**, including object/reference naming and Use ordering; payload comparison must canonicalize the complete semantic reference set of each unique tensor, not assume every binding has an `object` member.

**5090 binding recommendation: use published v3 `81f924d4`.** The completed comparison matched **1,184 / 1,184 unique tensor payload SHA-256 digests**, with no geometry differences and no unmatched semantic references. It also matched all **six frontend resources byte-for-byte**. Canonicalization includes every fused binding part and every Use auxiliary, hashes each physical tensor once, and keys Uses by parameter/input rather than array position. This is full coverage, not a hash sample.

Only **six semantic manifest fields** differ: `/files/0/payload_bytes` (**20,437,148,928** published versus **20,437,157,872** upgraded), plus provenance `converter`, `ranking`, `recipe`, `sources`, and `upgraded_from`. Published provenance records the converter/recipe/source locations; the upgrade records its v2 origin. No component, binding, Use policy, tensor geometry, tokenizer, template or preprocessing-resource difference remains. Absolute upstream provenance source paths in the two diff receipts were replaced by SHA-256 redaction markers (**three paths per receipt**), preserving field identity without committing private paths.

Complete comparison lease: `20261025T012435Z-infra-artifact-reference-sha256-239425`, **28.502 seconds** measured work, exit zero. Native upgrade component inventory is **`mtp`, `text`, `vision`**, with binding prefixes `mtp`, `proposal`, `text`, `vision`; it has no DFlash/DFlash2 component. These identities and the published-v3 recommendation were sent to the lead, integrator and kernel-routes owner.

Exact upgrade command shape: `python3 "$SRC/tools/upgrade_ninfer_v2_to_v3.py" "$V2_INPUT" "$V3_OUTPUT"`; SHA-256 was computed with bounded 8 MiB reads. `artifacts.json`, `tensor-payloads.json`, `manifest-field-diff.json` and `chat-template.diff` are the scrubbed receipt family.

## Secondary lane: owner state and restore recipe

Before pausing any owner workload, the bounded inventory found Strata running from **`E:\omp-strata-0.2.3-omp18.12.6-pro-262k`**, profile **`win11-rtxpro6000-iq3s-262k-slots4-parking-strata0.2.3-omp18.12.6`**. Both OCR containers (`alpha-diligence-ocr-paddle`, `alpha-diligence-ocr-mineru`) were **exited**. Preserve that exact state: do not invoke the generic window `take`/`release` pair, which expects running OCR and would start them on release. No other owner's engine/root or GPU power policy is changed.

The recorded owner restore command is this PowerShell, executed on the secondary Windows host through its existing SSH alias:

```powershell
$root = 'E:\omp-strata-0.2.3-omp18.12.6-pro-262k'
$profile = 'win11-rtxpro6000-iq3s-262k-slots4-parking-strata0.2.3-omp18.12.6'
& "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe" `
  "$root\tooling\scripts\omp_strata.py" start --root $root `
  --profile "$root\tooling\profiles\$profile.json"
```

Use the same controller/root/profile with `stop` to pause, and `status` to verify restoration; leave both OCR containers exited. Notify the lead with the exact restore command before the pause, and restore Strata when the program is no longer using the correctness lane. The native process is not stopped by an ad-hoc kill.

Docker Desktop **4.96.3**, Linux engine **29.9.2**, WSL kernel **6.18.33.2** and a registered **`nvidia`** runtime already exist. The `tensorfold` WSL2 distro has Git/flock and 808 GiB free; its Docker Desktop integration is disabled. An ext4 UNC mount refused the missing distro mount service; the Windows-drive bind alternative passed (`WSL_BIND_OK`). CPU setup therefore uses Windows-drive source/build paths through the existing Windows Docker client, without restarting global Docker Desktop, changing host networking, installing a second daemon, or altering another product's distro/toolchain. The two-hour setup bound begins at **01:57:27 UTC**.

`build-wsl.sh` / `run-in-env-wsl.sh` configure that bridge. Use `/mnt/<drive>/...` paths, not an unintegrated distro's ext4 paths. The build wrapper selects **12 workers** and acquires one of **two native-WSL `flock` slots** for the complete configure/build lifetime. The generic helper translates only bind-source/config paths to Windows syntax; container paths stay identical to the WSL paths. An isolated Docker CLI config is passed explicitly as `--config` (a WSL environment variable alone did not cross Windows interop). The committed anonymous Docker Hub entry contains no credentials and prevents Docker CLI's automatic native credential-store discovery, which otherwise refused the noninteractive Windows logon session. [Docker CLI's predicate](https://github.com/docker/cli/blob/master/cli/config/config.go) is `!configFile.ContainsAuth()` before native-store detection. No credential values are copied, logged or modified.

### Proven two-slot CPU service

The local CPU-only recipe image is **`ov-build-env:master-64d10fbf-r2-cpu-local`**, ID **`sha256:62a4feee8f308cde61095355ff11977986ecce388ad4f057c23014950b681326`**. Its build took **222.544 seconds**. Two simultaneous real CMake builds acquired different locks (`slot-0` and `slot-1`) and passed: `ninfer_public_api_test` target in **24.590 seconds**, `ninfer_cpu_system_test` target in **25.573 seconds**. Their CTest executions passed in **0.493 / 0.397 seconds**. This proves the two CPU slots and path/config bridge, not a full GPU application build or GPU qualification. `cpu-lane.json` records the image, timings, slot markers and exit codes.

The CPU-ready announcement and exact detached invocation were broadcast to every live peer. The invocation inside the existing Windows detached launcher is:

```sh
wsl.exe -d tensorfold -u root --exec env \
  NINFER_BUILD_HOLDER=YOUR_HOLDER \
  NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2-cpu-local \
  bash "$COMMON/build-wsl.sh" "$SRC" "$BUILD" -- ninfer-serve
```

The outer operator uses `omp-strata-remote.py launch "$SECONDARY_HOST" "$WINDOWS_WORK" "$COMMAND" "$WINDOWS_LOG"`; do not launch a long WSL build as an SSH-owned foreground child. The source must be a real detached Git checkout under a Windows-drive path. The shared helper directory also contains `run-in-env-wsl.sh` and the io_uring policy. GPU tests remain **unauthorized by this CPU-ready announcement**.

For native filesystem tests, `--volume ov-YOUR_HOLDER-native-root /nativeio --env TMPDIR=/nativeio` provides a caller-owned Docker volume in the Linux VM, independent of DrvFS source/build mounts. The actual probe returned filesystem magic **`0xef53`** (native ext family, not DrvFS/9p) and passed write/unlink. The owning backend suite still decides its supported-I/O contract; no test is skipped or suppressed. The durable-store owner received this recipe without changing its lead-approved current qualification batch.

At the requested wrap-up boundary, **Strata is still running and both OCR containers remain exited as found**. No pause/restore, host reboot, power change, Docker Desktop restart, or GPU computation on the secondary card was performed. Exact-image/model export remains a separately queued lease job; the local CPU image is deliberately not presented as the exact exported r2 image. The secondary full build/model copy/GPU test are not proven.

## Startup-budget observations and validity boundary

The full product engine shape **does construct on the 32 GB card**: 131,072 context, two active lanes plus two extra StateImages, DFlash2 draft window 7, optimized proposal head, BF16 KV, prefill 1,024, Vision and the exact shared-host equivalent **21,881,405,440 bytes** (`--host-context-mib 20867.734375`). The translation is 16 GiB of former host KV plus 24 native StateImages of **195,897,344 bytes** each; upstream's shared pool does not recreate separate fork slot quotas.

| Measured automatic-startup field | Bytes / value |
|---|---:|
| Resolved KV capacity | 146432 tokens; 2288 pages of 64 tokens |
| Device budget | 32429309952 |
| Weights | 19973043200 |
| Runtime reservation | 11350282753 |
| Available after startup | 1072693248 |
| CUDA graph rebuild estimate (legacy field) | 0 |
| Actual graph calibration retained | 31457280 |
| Startup retained | 31689728 |
| Total device memory | 34162343936 |

Explicit `--kv-capacity 131072` also constructed. Explicit **262144** refused before serving with exit **1**, `Device budget cannot hold model weights and required execution resources`. These observations come from actual `server_start` records and the actual fatal process exit, not from readiness polling. `vanilla-server-start.json` contains the sanitized measured baseline supplied to EngineCache; the zero legacy graph estimate must not substitute for the nonzero calibration/startup-retained measurements.

The original external WSL observer used Docker host networking and could not reach the container's network namespace, even though a diagnostic internal `/health` returned **HTTP 200**. Its 240-second observer timeouts are **not capacity refusals**. That job was terminated after **947 seconds**; the lease stopped its one owned leftover container. Its receipt is explicitly archived as transport-invalid. The current `startup-budget.json` states the actual engine outcomes and leaves **the one-request/two-in-flight HTTP proof and maximum-explicit-capacity search unproven**. The corrected same-container observer implementation is retained outside Git for the lead to run; no success is fabricated.

The final receipt census parsed **14 JSON receipts** and found **zero** forbidden private-path/host/GPU-UUID markers. Upstream absolute provenance paths had already been replaced with digest-only markers. `receipt-census.json` records the exact inventory.
