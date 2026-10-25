# Portable host core (C1)

**C1 host-portability acceptance passes.** At the same combined source `47ebc3824`:
native core **9/9**, complete native sm_89 host inventory **52/52**, Windows
`ninfer-serve` links for **sm_89 and sm_86**, Linux host inventory **52/52** and Linux
artifact/expert/PLE GPU I/O **3/3**. No skips or compiler/link errors in these qualified targets.
This is not provider-union, native GPU numerical, package or release qualification.

## Scope and ownership

- Base: upstream master `64d10fbf`; branch `rebase/portable-core`. The pre-existing branch contained architecture code through `b5bd4926`; no new architecture merge was made into this base. The current proof uses an isolated combined commit: C1 `6661b7d03a4638f9eee278f5df9fdfbfbe3c13bd` plus ArchEnable `363ccccc881723b2c457f5f4278aba50bddc2954` yields `47ebc3824b8c32112458eb013e5d4bb20c8b98f6`. The later ArchEnable `6063e9d9` changes its report only.
- Owns core I/O/CPU, platform/dependency CMake, logging and media acquisition. Shared CMake/arithmetic, request-log process identity, artifact alignment, native test interposition and Python artifact I/O edits were explicitly coordinated with the integration/owning slices.
- Additional authorized MSVC operator fix: `ed99ff3fb`, six files under `src/ops/common/cpu` and `src/ops/moe`. No kernel routes, numerical tolerances, CPU admission feature checks or global ISA baseline were changed.
- Read-only design references: the shipped native fork's artifact reader/Windows helpers, `natpate/master` and `donchad3090/master`. The upstream host APIs and existing test registration patterns remain authoritative; no fork Program/scheduler transplant was introduced.
- No component publication, production installation, model mutation or build-host GPU use was performed. This is a slice qualification, not the integrated release gate.

## Native blocker table (§7)

| Original blocker | Resolution | Evidence boundary |
|---|---|---|
| `O_CLOEXEC` / `pread` / `O_DIRECT`, `statx`, `aligned_alloc` | Common range/alignment API; POSIX implementation preserved in `file_posix.cpp`; Windows `CreateFileW`, overlapped reads, optional unbuffered mode and per-handle storage/alignment queries in `file_windows.cpp`. Native handle is pointer-width, not a truncated descriptor. | File tests cover Unicode, sparse offsets above 4 GiB, ownership/move, short EOF, empty files and direct alignment. |
| Linux-only liburing queue | Linux source selected only on Linux. Windows has a real IOCP backend, queue-owned `ReOpenFile` handles, stable preallocated OVERLAPPED slots, exact completion/error delivery, wake and shutdown draining. No stub or disabled PLE route. | Shared queue suite plus real native byte-range-lock error fixture. Linux submission fault interposition retained. |
| Four `unsigned __int128` consumers | Exact two-limb `core/uint128.h`; migrated attention resource accounting, context cost, resource-manager cross-products and fractional MiB parsing. Saturation remains explicit; triangular factors are halved before multiplying. | Compile-time assertions, deterministic Python big-integer oracle, existing context/resource/options suites. |
| `ioctl(TIOCGWINSZ)` | Native console width via `GetConsoleScreenBufferInfo`; original 120-column fallback. Also native TTY and `localtime_s` handling. | Real native pipe/standard-handle logging capture, not a disabled test. |
| pkg-config-only dependencies | Linux pkg-config unchanged. Windows manifest/vcpkg FFmpeg, CURL and hwloc; no Windows liburing. One shared CUDA runtime selected before all targets; `/MD` CRT; pinned DirectStorage SDK. Host-only switches are passed to nvcc through `-Xcompiler`. | Real vcpkg install and native configure; link proof is recorded below, separately from configure. |
| `<cpuid.h>` | MSVC `<intrin.h>` / `__cpuidex` / `_xgetbv`; original feature mask and hwloc topology/strict binding retained. | CPU system, task-pool and executor suites; Windows processor-group-aware current-CPU observation. |
| POSIX media sockets | Winsock initialization and native DNS error handling; `ws2_32` linkage. Native-character root-prefix comparison avoids a lossy Unicode conversion. | Compiled/link coverage; no external-network behavior is asserted by this slice. |
| Platform / architecture gate | Linux or Windows x86-64; exactly one explicit CUDA architecture profile (`120a`, `89`, `86`), default `120a`. Architecture/device gates remain ArchEnable-owned. | Native profile `89`; Linux profile `120a`. This report does not substitute configure success for numerical GPU qualification. |

## Additional contracts

- `src/core/io/durable_file.{h,cpp}` is the K6 host-only primitive library: exclusive binary creation, no-follow binary reading, exact stdio transfer with EINTR retry, flush, replace, directory creation/sync and anchored removal. Paths reject traversal and unsafe platform forms; a normal `.` root remains supported.
- POSIX directory walks use relative `openat` with `O_NOFOLLOW`; successful `mkdirat` fences the pinned parent. `remove_tree` handles regular files and final links without following them, walks directories relative to open descriptors and fences the outer parent after removal.
- Windows ancestor pins share READ and WRITE but deny DELETE, preventing directory rename/delete/swap while allowing the `FILE_ADD_FILE` access needed to publish a child. Each pin captures its volume serial and file index; immediately before rename every source/destination ancestor pin is rechecked for the same identity and absence of a reparse attribute. Directory walks/creation, stream opens, destination-entry checks and removal all use counted child names relative to pinned handles through `NtCreateFile`; only the local drive root is opened by pathname. Recursive removal still enumerates with `NtQueryDirectoryFile` and deletes by handle, removing reparse points themselves rather than following them.
- Windows replacement retains `FlushFileBuffers` on the staged source, handle-anchored `NtSetInformationFile(FileRenameInformationEx = 65)` with replacement/POSIX flags and a single leaf relative to the pinned destination, then `FlushFileBuffers` on that parent handle. Sources may be regular disk files or real disk directories; reparse sources are refused. This covers DurableStore's nonempty generation publication, sibling-parent tombstones and quarantine moves without changing callers; payload/manifest file flushes remain the caller's prerequisite before a directory move. This Ex policy requires Windows 10 1709 or newer and NTFS; the program targets Windows 11. Common parent file objects are borrowed to avoid duplicate opens. Standalone `sync_directory` remains a non-throwing no-op and reports `dir_sync: "ntfs-write-through"`; Linux reports `"fsync"`. Authoritative K6 refinement: Contracts `b1993837` (supersedes `fc640827` pin policy). No Win32/MoveFileEx fallback, retry/sleep or power-loss experiment is introduced or claimed.
- Re-expressed dev `4ec903a0` in `src/artifact/materializer.cpp`: each file's actual offset/memory alignment drives read planning; aligned host interiors remain in-place, boundaries are staged; pinned staging retains the original owner plus alignment slack, and peak staging accounting includes that requested slack. A synthetic geometry test forwards real Linux statx / Windows handle-info calls, and GPU interposition calls the real runtime.
- Native CUDA failure counterparts keep artifact materialization, expert-cache and PLE tests registered. GNU `--wrap` stays Linux-only; Windows wrappers resolve real CUDA functions. No fake success, weakened assertion, numerical tolerance change or blanket skip was introduced.
- Python artifact I/O keeps Linux `pread`/`pwrite`/`fdatasync` behavior and uses explicit-offset Win32 I/O on real CRT handles. The real-file test covers binary control bytes, concurrent disjoint offsets, sparse files beyond 4 GiB and writeback. Windows cache-discard is explicitly advisory/no-op; file flush is not omitted.
- Additional MSVC blockers discovered by the real build: per-object `/arch:AVX2` or `/arch:AVX512` replaces ignored GCC ISA switches; BF16 instructions remain explicit and feature-gated. `NINFER_UNREACHABLE()` selects compiler semantics, and scalar half conversion uses the same F16C instruction through compiler-portable vector intrinsics.

## K6 correction and full-link portability causes

- Root cause of the original error 32: a rename into a directory performs a
  `FILE_ADD_FILE` access, which conflicts with the old pin's deny-WRITE sharing.
  Native information class 65 and staged-leaf DELETE sharing do not bypass that
  directory sharing check. Pins now use exactly `FILE_SHARE_READ | FILE_SHARE_WRITE`,
  never DELETE sharing; identity/attribute checks immediately precede rename.
- This is an anchored cutover, not merely a share-mask change: Windows child directory
  creation, traversal, stream opening and destination-leaf inspection now use
  `NtCreateFile(RootDirectory=pin, leaf)` with `FILE_OPEN_REPARSE_POINT`.
  Stream handles explicitly request `FILE_READ_ATTRIBUTES` for their mandatory check.
- Main confirmed that confined same-volume cross-parent moves remain permitted.
  Refusal tests cover redirected/reparse parents, source/destination links and traversal,
  not a new same-parent-only rule. Main additionally approved real directory sources:
  DurableStore needs nonempty generation publication, sibling-parent tombstones and quarantine.
- `tests/io/test_durable_file.cpp` adds first same-parent publication, real flush-order/
  deny-DELETE observation, retained old-reader bytes, valid child/sibling publication,
  nonempty directory publication/tombstones, an actual in-place junction conversion
  after source flush, independent returned-index/volume faults, old-current preservation
  on refusal, and no-follow recursive/final-link removal. Windows IAT interposition is
  test-only; the real OS performs every flush and the real junction conversion.

The full native build exposed the following independent compiler/fixture causes.
Each repair is compiler-neutral or Windows-only; none disables a test, relaxes an
oracle/tolerance, suppresses a diagnostic, or supplies a fake runtime result.

| Cause / repair | File anchor | Coordinated owner |
|---|---|---|
| Public declarations relied on transitive `<array>` / `<span>` includes; imported isolated `252bb209d` as `6149384cf`. | `include/ninfer/types.h:3`; `src/serve/generation_service.h:18` | Integrator2 |
| Static utf8proc was compiled/consumed as dllimport (`C2491`); imported PUBLIC `UTF8PROC_STATIC` from `9ef4c80ac` as `b1f99a76b`. | `src/text/CMakeLists.txt:7` | Integrator2 |
| XGrammar's unreachable macro was empty under MSVC, causing `C4716`; `__assume(0)` gives it the same unreachable contract as the existing GNU builtin. | `third_party/xgrammar/cpp/support/utils.h:363` | Integrator2 / shared third_party |
| Jinja used POSIX `localtime_r`; the Windows branch uses checked `localtime_s`, preserving the same invalid-time exception. | `third_party/llama-jinja/jinja/value.cpp:284` | Integrator2 / shared third_party |
| Removed `shared_ptr::unique()` API; the copy-on-write predicate is now the equivalent `use_count() != 1`. | `src/models/common/context/kv_history.cpp:763` | Persistence3 |
| Request-log fixture unconditionally included `unistd.h`; native `_getpid` follows the production request-log convention. | `tests/test_request_log.cpp:16,874` | ServePort2 |
| Timing fixture's `near` helper collided with Windows headers; rename only, unchanged comparison and tolerance. | `tests/test_host_timing.cpp:16` | EngineCache3 |
| Expert-cache oracle used `std::any_of` without `<algorithm>`. | `tests/test_expert_cache.cu:7` | ArchEnable2 |
| Artifact/model-loading fixtures rewrote files while unnecessary old Readers were alive. Scope those Readers to their assertions; on Windows verify the existing immutable pin refuses truncation with the real filesystem error, while POSIX retains the real post-open truncation/EOF test. | `tests/artifact/test_reader.cpp:222`; `tests/models/qwen3_5/test_loading.cpp:171` | PortableCore2 / Persistence3 |
| CUDA 13.3's monolithic Windows import-loader object collided with test wrapper definitions (`LNK2005/LNK1169`). Generate a conventional per-export import archive from the selected real DLL for the three interposition tests only, ordered after their existing CUDA-bearing static archives. A wrapper has one definition, every unwrapped API still imports that DLL, and GNU `--wrap` is unchanged. | `tests/cmake/NinferTests.cmake:21,65`; artifact/CoreTests/PLE test registration | Integrator2 / test registration; PortableCore2 implementation |
| MSVC did not deduce the two-field `CalibratedPlan` aggregate (`C2641/C2780`); spell its already-declared `Setup::ExecutionPlan` template argument explicitly at both returns. | `src/runtime/engine/model_startup.h:52,69` | EngineCache3 |
| Unicode oracle pipes defaulted to Windows cp1252, raising `UnicodeEncodeError` in the writer thread and stranding the real probe until its unchanged deadline. Select UTF-8 explicitly for every selected Jinja/JSON-schema/regex/Qwen probe and template source read. | `tests/text/test_chat_templates.py:16,293`; `test_json_schema.py:27`; `test_regex_choice.py:55`; `tests/models/common/qwen/test_tool_schema.py:46` | Frontend / shared tests |

## Observed toolchain and dependencies

| Item | Observed value |
|---|---|
| Native compiler | VS 2022 BuildTools; toolset `14.44.35207`; MSVC `19.44.35228.0` |
| Native CUDA | Toolkit `13.3.73` |
| Native build tools | CMake `4.4.2`; Ninja `1.13.2`; Python `3.13` |
| Native test-only environment | Isolated Python venv; existing oracle dependencies Jinja2 `3.1.6`, jsonschema `4.26.0` (`tests/text/requirements.txt` constrains jsonschema to `>=4.23,<5`) |
| vcpkg baseline | `4bca8fd8654e5ba76f92661db7bfe954768ad8ef` |
| Runtime ports installed | CURL `8.21.0#1`; FFmpeg `8.1.2#3` with zlib; hwloc `2.11.2`; zlib `1.3.2#1` |
| Native CRT redistributable | MSVC redist directory `14.44.35112` |
| DirectStorage | SDK `1.3.0`, archive SHA-256 `8ab6c1082537565388b79050d94fdd7f5f437cd0f1d909e7b19f786042b97177` |
| Linux compiler / CUDA | GNU `13.3.0`; CUDA `13.1.115` in the lease build environment |
| Linux qualification image | `ov-build-env:master-64d10fbf-r2`, ID `sha256:e7f34ca2f41ecea14da5cce0b2a30cae6b0877e3b3eb371bd92d103bca1121f1` |

Build-host toolchain facts and realized vcpkg dependency versions were sent to ArchEnable and ProductOps. Native builds use an isolated installed-dependency tree and 12 workers under the shared CPU-slot lock. Physical machine names, absolute machine paths and raw build receipts remain private.

## Commands and proof boundaries

`SOURCE`, `BUILD`, `VCPKG_ROOT`, `VCPKG_INSTALLED`, `PYTHON`, `COMMON` and `VSINSTALLDIR` below denote private operator-provided locations. The argument lists are preserved without publishing those locations.

Native environment and configure:

```powershell
$vs = "$env:VSINSTALLDIR\Common7\Tools\VsDevCmd.bat"
$lines = & cmd.exe /d /s /c ('"' + $vs + '" -arch=x64 -host_arch=x64 >nul && set')
if ($LASTEXITCODE -ne 0) { throw 'VS environment failed' }
foreach ($line in $lines) {
  $i = $line.IndexOf('=')
  if ($i -gt 0) {
    [Environment]::SetEnvironmentVariable($line.Substring(0, $i), $line.Substring($i + 1), 'Process')
  }
}
cmake -S $SOURCE -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CUDA_ARCHITECTURES=89 -DNINFER_BUILD_APPS=ON -DBUILD_TESTING=ON `
  -DNINFER_BUILD_BENCHMARKS=OFF `
  "-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  "-DVCPKG_INSTALLED_DIR=$VCPKG_INSTALLED" -DVCPKG_TARGET_TRIPLET=x64-windows `
  "-DPython3_EXECUTABLE=$PYTHON"
```

The final native batch uses the complete host inventory below, `ninfer-serve`,
architecture/artifact/expert-cache/PLE/loading-real helpers, and every target named
by `ctest --test-dir $BUILD -L ops --show-only=json-v1`. Python-only case names are
not build targets; CPU materialization maps to `ninfer_artifact_materialization_test`,
and PLE host maps to `ninfer_ple_disk_prepare_test`. Both inventories must be nonempty.
`SOURCE` is detached at `47ebc3824`; `PYTHON` selects the isolated test venv.

```powershell
$hostSelection='public_api|cpu_(system|task_pool|executor)|read_queue|host_timing|jinja|chat_templates|weight_bank|gdn_replay_records|artifact_(reader|backing|cpu_materialization|file_io)|scheduler|context_cost|resource_manager|kv_capacity|sampling_defaults|grammar|regex_choice|regex_choice_oracle|json_schema|json_schema_oracle|media_decode|prompt_input|pretty_logging|perplexity_evaluation|cli_options|openai_schema|openai_responses|openai_responses_store|anthropic_schema|serve_options|request_log|metrics|http_error_handler|http_transport|qwen_frontend|qwen_tool_constraints|qwen_tool_schema_oracle|tool_call_parser|qwen3_5_loading|qwen3_5_state_image_layout|bench_support|context_cost_measure|durable_contract_headers|durable_contracts|file|durable_file|uint128|uint128_exact|ple_disk_prepare_host|architecture_contract'
$pattern="^ninfer_($hostSelection)_test$"
$corePattern='^ninfer_(cpu_.*|file|durable_file|read_queue|uint128.*|artifact_file_io)_test$'
$manifest=(ctest --test-dir $BUILD -R $pattern --show-only=json-v1 | Out-String) | ConvertFrom-Json
$pythonTests=@('ninfer_uint128_exact_test','ninfer_artifact_file_io_test','ninfer_chat_templates_test','ninfer_regex_choice_oracle_test','ninfer_json_schema_oracle_test','ninfer_qwen_tool_schema_oracle_test')
$hostTargets=@($manifest.tests.name | Where-Object { $_ -notin $pythonTests } | ForEach-Object {
  if($_ -eq 'ninfer_artifact_cpu_materialization_test') {'ninfer_artifact_materialization_test'}
  elseif($_ -eq 'ninfer_ple_disk_prepare_host_test') {'ninfer_ple_disk_prepare_test'}
  else {$_}
})
$ops=(ctest --test-dir $BUILD -L ops --show-only=json-v1 | Out-String) | ConvertFrom-Json
$c2Targets=@('ninfer-serve','ninfer_architecture_contract_test','ninfer_artifact_materialization_test','ninfer_expert_cache_test','ninfer_ple_disk_prepare_test','ninfer_qwen3_5_loading_real_test') + @($ops.tests.name)
$allTargets=@($hostTargets+$c2Targets | Sort-Object -Unique)
cmake --build $BUILD --parallel 12 --target ninfer-serve
cmake --build $BUILD --parallel 12 --target @allTargets -- -k 0
ctest --test-dir $BUILD -C Release --output-on-failure --timeout 180 `
  -R $pattern -E $corePattern --output-junit "$NATIVE/k6-host-47ebc3824.xml"
```

Native core build/test (nine cases, also the first phase of the complete host batch):

```powershell
$env:CUDA_VISIBLE_DEVICES = ''
cmake --build $BUILD --parallel 12 --target ninfer_cpu_system_test ninfer_cpu_task_pool_test `
  ninfer_cpu_executor_test ninfer_file_test ninfer_durable_file_test `
  ninfer_read_queue_test ninfer_uint128_test
ctest --test-dir $BUILD -C Release --output-on-failure --timeout 180 `
  -R '^ninfer_(cpu_.*|file|durable_file|read_queue|uint128.*|artifact_file_io)_test$'
```

The final Linux build/tests are one lease batch, persistent build directory and
15-minute cap. The full host suffix inventory is the 52-case native inventory:
the nine core cases plus the 43 names listed in the current proof section.
`HOST_TARGETS` is that inventory's C++ targets with the same six Python-only exclusions
and the two target mappings above; `HOST_PATTERN` selects all 52 names.

```sh
export NINFER_BUILD_HOLDER=C1
export NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2
export NINFER_BUILD_JOBS=12
"$COMMON/build.sh" "$SOURCE" "$BUILD" \
  -DCMAKE_CUDA_ARCHITECTURES=120a -DBUILD_TESTING=ON -DNINFER_BUILD_BENCHMARKS=OFF -- \
  ninfer-serve ninfer_expert_cache_test "${HOST_TARGETS[@]}"
"$COMMON/run-in-env.sh" --gpu --env CUDA_VISIBLE_DEVICES= \
  --mount "$SOURCE" --mount "$BUILD" --seccomp "$COMMON/ninfer_io_uring_seccomp.json" \
  --name k6-host-tests -- ctest --test-dir "$BUILD" --output-on-failure -R "$HOST_PATTERN"
"$COMMON/run-in-env.sh" --gpu --mount "$SOURCE" --mount "$BUILD" \
  --seccomp "$COMMON/ninfer_io_uring_seccomp.json" --name k6-gpu-io-tests -- \
  ctest --test-dir "$BUILD" --output-on-failure \
  -R '^ninfer_(artifact_materialization|expert_cache|ple_disk_prepare_gpu)_test$'
```

The first Linux test container exposes the real driver library with zero visible devices. GPU I/O checks run without that mask. The older stub-driver image is not GPU qualification evidence.

Exact final source/driver arguments (private transport/root locations are variables):

```text
qualify-k6-windows.ps1 -Revision 47ebc3824b8c32112458eb013e5d4bb20c8b98f6 -SourceRef refs/heads/verify/portable-core-full-6661b7d03-363ccccc8
build-windows.ps1 -Revision 47ebc3824b8c32112458eb013e5d4bb20c8b98f6 -Arch 86
gpu-lease submit --holder C1 --label portable-full-47ebc3824-final --priority 2 --minutes 15 -- bash $INPUT/verify-current-linux.sh 47ebc3824b8c32112458eb013e5d4bb20c8b98f6 refs/heads/verify/portable-core-full-6661b7d03-363ccccc8
```

Native jobs use the existing WMI launcher and
`flock -x /var/lock/ov-ninfer-build-slots/slot-0.lock`, 12 workers, no CUDA-visible
devices for host tests. The Linux wrapper imports its exact bundle under the lease,
checks out the supplied detached hash, and performs all build/test phases in that one job.
The three private C1 dispatcher scripts were removed after all jobs settled; the direct
configure/build/test recipes above remain the reproducer. Logs, exact source bundles
and hashed native build/ops manifests are retained. Shared/C2 operator helpers were not changed.

### Earlier diagnostic results (superseded by the native proof below)

| Source / batch | Observed outcome |
|---|---|
| `c6b5d5ada`, first Linux iteration | Configure succeeded; build stopped after 506/648 steps, wall `153.70 s`, because LP64 `auto` deduction mixed `uint64_t` and `ULL`. Fixed with explicit limb types in `46dd34def`; no tests ran in that failed iteration. |
| `2237f6467`, first native host iteration | Configure and all 11 vcpkg package installs succeeded; build stopped around 103/173 at Linux-only `sched.h` in the executor test. Replaced with group-aware Windows processor observation in `b0f18412`; no tests ran in that failed iteration. |
| `3594a1377`, native full operator iteration | Native core library linked; build failed at 203/1471 on `_cvtsh_ss`, `__builtin_unreachable` and ignored GCC ISA switches in MoE CPU sources. Exact operator fixes are in `ed99ff3fb`. No host-test or server-link pass is claimed for this iteration. |
| `ed99ff3fb`, source transport | Stopped before configure/build (`1.336 s`): the newly created bundle exported the branch, not a `HEAD` ref. Operator transport now fetches `refs/heads/rebase/portable-core`; source behavior was not changed to mask this failure. |
| `ed99ff3fb`, corrected native transport/full build | Source import and configure succeeded; build reached 98/1289, `101.566 s`, then MSVC `C2719` rejected a 128-aligned by-value `descriptors` parameter in `src/ops/linear/kernels/bf16/bf16_a16_tma_mma.cuh:116`. The earlier CPU ISA/intrinsic diagnostics were passed. No full host-suite or server-link pass is claimed. ArchEnable2 supplied its native-only TMA gate fix as `ca85f2573`; that fix is not qualified by this failed build. |
| `3594a1377`, Linux lease job `20261025T020633Z-C1-portable-core-final-360834` | Exit 1 after 36 lease seconds; build wall `32.87 s`, stopped after 71/367 steps. NVCC host emission repeated the FP8 `Rows` default (`fp8_template_launch.cuh:94`, first declaration `fp8_a8_tma_mma.cuh:22`). The imported `b5bd4926` removes the defaults, but this older-source failure is not evidence that the correction passes. No tests ran; cleanup reported zero leftover/unowned containers. |
| `ed99ff3fb`, focused native core batch | All seven executables built/linked (29 tasks); 8/9 cases passed, `2.42 s`. Only durable replacement failed with sharing violation. CPU system/task pool/executor, wide arithmetic plus exact oracle, file I/O, IOCP queue and Python artifact real-file tests passed. No skip was reported. |
| Native sharing diagnosis | New temporary-file experiments: parent-chain share mask `3` (READ/WRITE, deny DELETE) allowed `MoveFileExW(...,9)`; mask `5` (READ/DELETE, deny WRITE) failed with error 32; mask `7` allowed it. The lead rejected weakening the pins and selected the anchored-rename implementation in `5a5885513`. No sleep/retry or broad error suppression was added. |
| `5a5885513`, first anchored native replacement | Configure succeeded and the changed library/test linked (16 tasks); 8/9 cases passed, `2.38 s`. Durable replacement reached `SetFileInformationByHandle(FileRenameInfoEx)` but returned error 87 before the parent flush. The variable-length buffer was corrected to include the full structure, trailing array element/padding and a zero-terminated leaf in `26bfbd582`, with a one-character target-name regression. |
| `26bfbd582`, corrected Win32 buffer | Configure and 16-task incremental build succeeded; 8/9 cases passed, `2.32 s`. The Win32 Ex call still returned 87. Per the lead's explicit decision, `009c92967` cleanly replaces it with native information class 65 while retaining the same buffer, flags, anchored names and flush sequence. |
| `009c92967`, native information class 65 | Configure and 16-task build succeeded; 8/9 cases passed, `2.46 s`. The native rename passed the previous parameter validation but returned sharing violation, before parent flush. This is not a successful publication proof. |
| `993e7d97d`, reader semantics / named regression phases | Configure and 16-task build succeeded; 8/9 cases passed, `2.25 s`. Read-only leaf handles share DELETE (not WRITE) so an old reader can retain its file object across POSIX replacement; parent pins remain unchanged. The named failure is the **first same-parent publication**, before the retained-reader/cross-parent/removal regressions. `f689e6bde` also grants DELETE sharing to the staged leaf handle, without relaxing any parent pin. |
| `f689e6bde3318cc4c5758d88b65f9affa306369b`, final current-step native core proof | Configure succeeded (`23.4 s` plus `1.3 s` generation), changed library/test linked (14 incremental tasks); 8/9 passed, `2.36 s`, exit 8, no skips. The first same-parent native publication still returns sharing violation. Staged-leaf DELETE sharing did not resolve it. Native K6 publication, parent flush and the downstream retained-reader/cross-parent/no-follow/removal regressions are **not proven**. |

These earlier red rows are preserved as diagnosis, not current acceptance results.
The native successor uses the full 52-case host inventory rather than the earlier
15-case selection. An architecture compile inventory is never a numerical GPU pass.

### Current native sm_89 proof

| Gate | Exact observed result / receipt |
|---|---|
| Source | Combined `47ebc3824b8c32112458eb013e5d4bb20c8b98f6`; portable code `6661b7d03`; architecture code `363ccccc`. |
| Native core | **9/9 pass**, zero failures/skips, `10.00 s`; `core-tests-47ebc3824.log`. Includes durable publication/removal, three CPU suites, file/IOCP, Uint128 plus exact oracle, Python real-file artifact I/O. |
| Remaining complete host inventory | **43/43 pass**, zero failures/skips, `6.48 s`; `k6-host-tests-47ebc3824.log`. Together with core: **52/52**, no omitted baseline cases. |
| Server | `k6-serve-build-47ebc3824.log`: 25 incremental tasks, final task `Linking CXX executable apps/ninfer-serve.exe`; exit 0. |
| Architecture/helper compilation | `k6-host-build-47ebc3824.log`: 170 incremental tasks, exit 0. `c2-build-sm89.json` binds **97 binaries**, including all **92 registered ops cases** plus artifact/expert/PLE/loading-real helpers, to the exact source and hashes; `c2-ops-manifest-sm89.json` preserves relative executable names/properties. |
| Server bytes / SHA-256 | `322383360`; `f7ec4d3426457bc5f9ccb706df369dfe032b94cd4a5e00a5dcfc8bcde8634c1d`. |
| Batch | `k6-native-full-47ebc3824.log`, `PORTABLE_K6_NATIVE_EXIT=0`, marker wall `95.0595732 s`; shared 12-worker CPU slot; CUDA devices hidden throughout. No ops GPU, real-model loading or server decode was executed here. |

All 43 non-core host cases passed. Names below use the `ninfer_` prefix and `_test`
suffix; the exact case inventory is `k6-host-names-47ebc3824.txt`:

```text
public_api gdn_replay_records host_timing jinja chat_templates weight_bank
artifact_reader artifact_backing artifact_cpu_materialization scheduler context_cost
resource_manager kv_capacity sampling_defaults grammar regex_choice regex_choice_oracle
json_schema json_schema_oracle media_decode prompt_input pretty_logging
perplexity_evaluation cli_options openai_schema openai_responses openai_responses_store
anthropic_schema serve_options request_log metrics http_error_handler http_transport
bench_support context_cost_measure qwen3_5_loading qwen3_5_state_image_layout qwen_frontend
qwen_tool_constraints qwen_tool_schema_oracle tool_call_parser ple_disk_prepare_host
architecture_contract
```

The earlier complete-host attempts are not silently counted as passes:
`2a4266e30` had 7/43 pass, 6 fail and 30 not run after compiler/link blockers;
`269a1959f` had 24/43 pass, 5 fail, 1 timeout and 13 not run. Their causes were the tabled
missing declarations/platform APIs, monolithic CUDA import collisions, live immutable
fixture handles and locale-dependent Unicode pipes. The final 47-source run closes
all of those classes without skipping cases, suppressing diagnostics or changing tolerances.

### Additional native sm_86 compile proof

Main assigned the second native profile after the sm_89 pass. The same immutable
`47ebc3824b8c32112458eb013e5d4bb20c8b98f6` was configured in a separate build directory:

- `C1-native86-47ebc3824.log`: **1,497/1,497 build tasks**, including a real
  `ninfer-serve.exe` link, `C2_NATIVE_BUILD_EXIT=0`, wall `656.5560171 s`.
- `c2-build-sm86.json` and `c2-ops-manifest-sm86.json`: **97 hashed binaries**,
  **92 ops cases**, source `47ebc3824`, architecture `sm_86`, exit 0.
- Server: `327513088` bytes; SHA-256
  `71daf2b282f1ff0bbed0d4eaa5216a54307413068837b94f917cb4895d637dc3`.
- This was compilation/linking only. The 52-case host run above is sm_89;
  no sm_86 host, GPU, real-model or decode execution is claimed by C1.
- The native CPU slot/source were released unchanged at `47ebc3824`.
  ArchEnable3 owns both profiles' binary bundling, kernel gating and GPU runs.

### Matching final Linux build and runtime proof

| Gate | Exact observed result / receipt |
|---|---|
| Source / lease | Exact combined `47ebc3824b8c32112458eb013e5d4bb20c8b98f6`; job `20261025T051221Z-C1-portable-full-47ebc3824-final-549933`, Main-authorized priority 2, 15-minute cap. |
| Build | **332/332 tasks**, including `ninfer-serve` and every selected host/helper target; build wall **120.06 s**. GNU `13.3.0`, CUDA `13.1.115`, image ID `sha256:e7f34ca2f41ecea14da5cce0b2a30cae6b0877e3b3eb371bd92d103bca1121f1`. |
| Host / CPU | **52/52 pass**, zero failures/skips, **3.75 s**; the same inventory as the native core plus remaining-host lists. Includes every `tests/io` and `tests/cpu` case, artifact reader/backing/CPU materialization, real Python file I/O, request logging, Jinja/JSON/Qwen independent oracles and model-loading host fixtures. |
| Real GPU I/O | **3/3 pass**, zero failures/skips, **2.40 s**: `ninfer_expert_cache_test` (`0.39 s`), `ninfer_artifact_materialization_test` (`1.74 s`), `ninfer_ple_disk_prepare_gpu_test` (`0.27 s`). |
| Batch / cleanup | `k6-linux-47ebc3824.log`: `PORTABLE_K6_LINUX_EXIT=0`; lease exit 0 after **131 s**; **0 leftover containers stopped, 0 unowned containers left running**. |

The host phase loaded the real driver with CUDA devices hidden; the subsequent GPU
I/O phase ran on the leased device. The configure/build shim is never presented as
runtime or GPU evidence. This was one batched build + host suites + artifact I/O job,
not separately cherry-picked successes.

Earlier corrected-source K6-only Linux batches also passed (`7145934a7`, `b74cd0ee9`,
`f7c7dca26`: 15 host + 3 GPU I/O cases). They are superseded by the matching final
47-source full inventory above. Main explicitly cancelled the two queued, unexecuted
superseded full batches ending `521500` / `544456` before admitting final job `549933`;
their cancellation exit 130 is not a test failure or qualification result.

## Preserved behavior and remaining gates

- Linux keeps POSIX direct I/O, liburing, pkg-config and directory fsync. Windows receives real platform implementations rather than omitted execution paths. No shipped family is intentionally dropped by this slice.
- The only explicitly different durability primitive is the lead-approved Windows directory-sync policy above. Native GPU arithmetic, integrated durable restart, concurrency/keep-warm, package/controller and release gates remain their owning slices' responsibilities.
- Windows GPU execution of artifact/expert-cache/PLE counterparts, both native GPUs' numerical/decode gates, the complete provider union and package/controller admission remain their owning slices' responsibilities. Compilation or a registered test is never reported as an exercised GPU pass.

## Integration boundary

- `rebase/portable-core` runtime code is pushed through `6661b7d03`; the combined native proof is the separate immutable `47ebc3824`, not an architecture merge into this branch.
- **Mergeable C1 provider:** matching Linux/Windows proof at `47ebc3824`, with both native profile links. Report-only successor commits do not change the qualified runtime code.
- K6 is implemented and native-tested, including real generation-directory publication/tombstones. Contracts `b1993837`, information class 65, flush order, no-follow anchoring and `ntfs-write-through` acknowledgement are retained.
- The full branch is required for Windows: earlier individual CPU/ABI fixes or the older eight-of-nine result are not its current qualification.
- Integrator3 owns provider-union admission; ArchEnable3 owns kernel gating and all native GPU runs. NativeOps owns package identity/DLL closure. Existing C1+Arch binaries must never be relabelled as a provider-union release.
- No production installation, component publication, model mutation, build-host GPU execution, fallback rename, retry/sleep or power-loss proof is claimed.
