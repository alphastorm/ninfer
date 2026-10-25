# Portable host core (C1)

**Acceptance is not established.** The real builds exposed architecture-owned compiler blockers before the full host-suite/server-link gates. The implementation and exact exercised results are separated below.

## Scope and ownership

- Base: upstream master `64d10fbf`; branch `rebase/portable-core`. Architecture dependency: `rebase/arch-enable` code through `b5bd4926ff595cefc12f6bae786727dfc017214b`. Its independent report remains available with `git show b5bd4926:REPORT.md`.
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
- Windows ancestors are pinned with sharing that prevents rename and reparse conversion. Recursive removal opens single counted child names relative to those handles with `NtCreateFile`, enumerates with `NtQueryDirectoryFile`, and deletes by handle; reparse points are removed themselves, never followed.
- Binding Windows durability policy was corrected after the real sharing diagnostic: `FlushFileBuffers` on the staged file, handle-anchored `NtSetInformationFile(FileRenameInformationEx = 65)` with replacement/POSIX flags and a single leaf relative to the pinned destination, then `FlushFileBuffers` on that parent handle. This Ex policy requires Windows 10 1709 or newer and NTFS; the program targets Windows 11. Deny-WRITE/DELETE pins and reparse checks remain intact. Common parent file objects are borrowed, not reopened with incompatible sharing. Standalone `sync_directory` remains a non-throwing no-op and reports `dir_sync: "ntfs-write-through"`; Linux reports `"fsync"`. Authoritative K6 correction: Contracts `fc640827`. No Win32/MoveFileEx fallback or power-loss experiment is claimed.
- Re-expressed dev `4ec903a0` in `src/artifact/materializer.cpp`: each file's actual offset/memory alignment drives read planning; aligned host interiors remain in-place, boundaries are staged; pinned staging retains the original owner plus alignment slack, and peak staging accounting includes that requested slack. A synthetic geometry test forwards real Linux statx / Windows handle-info calls, and GPU interposition calls the real runtime.
- Native CUDA failure counterparts keep artifact materialization, expert-cache and PLE tests registered. GNU `--wrap` stays Linux-only; Windows wrappers resolve real CUDA functions. No fake success, weakened assertion, numerical tolerance change or blanket skip was introduced.
- Python artifact I/O keeps Linux `pread`/`pwrite`/`fdatasync` behavior and uses explicit-offset Win32 I/O on real CRT handles. The real-file test covers binary control bytes, concurrent disjoint offsets, sparse files beyond 4 GiB and writeback. Windows cache-discard is explicitly advisory/no-op; file flush is not omitted.
- Additional MSVC blockers discovered by the real build: per-object `/arch:AVX2` or `/arch:AVX512` replaces ignored GCC ISA switches; BF16 instructions remain explicit and feature-gated. `NINFER_UNREACHABLE()` selects compiler semantics, and scalar half conversion uses the same F16C instruction through compiler-portable vector intrinsics.

## Observed toolchain and dependencies

| Item | Observed value |
|---|---|
| Native compiler | VS 2022 BuildTools; toolset `14.44.35207`; MSVC `19.44.35228.0` |
| Native CUDA | Toolkit `13.3.73` |
| Native build tools | CMake `4.4.2`; Ninja `1.13.2`; Python `3.13` |
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

The native target list is `ninfer-serve`, the host targets below, artifact/expert-cache/PLE helpers, and every target named by `ctest --test-dir $BUILD -L ops --show-only=json-v1`. The manifest must be nonempty and is exported with relative executable names, labels and skip codes, without private source/backtrace paths.

```powershell
cmake --build $BUILD --parallel 12 --target @targets
ctest --test-dir $BUILD -C Release --output-on-failure -R `
  '^ninfer_(cpu_.*|file|durable_file|read_queue|uint128.*|artifact_cpu_materialization|artifact_file_io|ple_disk_prepare_host|context_cost|resource_manager|pretty_logging|serve_options)_test$'
```

Focused native core build/test, while the independent operator graph is blocked:

```powershell
$env:CUDA_VISIBLE_DEVICES = ''
cmake --build $BUILD --parallel 12 --target ninfer_cpu_system_test ninfer_cpu_task_pool_test `
  ninfer_cpu_executor_test ninfer_file_test ninfer_durable_file_test `
  ninfer_read_queue_test ninfer_uint128_test
ctest --test-dir $BUILD -C Release --output-on-failure `
  -R '^ninfer_(cpu_.*|file|durable_file|read_queue|uint128.*|artifact_file_io)_test$'
```

Linux build and tests are one lease batch, persistent build directory and 15-minute cap:

```sh
export NINFER_BUILD_HOLDER=C1
export NINFER_BUILD_ENV_IMAGE=ov-build-env:master-64d10fbf-r2
"$COMMON/build.sh" "$SOURCE" "$BUILD" \
  -DCMAKE_CUDA_ARCHITECTURES=120a -DBUILD_TESTING=ON -DNINFER_BUILD_BENCHMARKS=OFF -- \
  ninfer-serve ninfer_cpu_system_test ninfer_cpu_task_pool_test ninfer_cpu_executor_test \
  ninfer_file_test ninfer_durable_file_test ninfer_read_queue_test ninfer_uint128_test \
  ninfer_artifact_materialization_test ninfer_expert_cache_test ninfer_ple_disk_prepare_test \
  ninfer_context_cost_test ninfer_resource_manager_test ninfer_pretty_logging_test ninfer_serve_options_test
"$COMMON/run-in-env.sh" --gpu --env CUDA_VISIBLE_DEVICES= \
  --mount "$SOURCE" --mount "$BUILD" --seccomp "$COMMON/ninfer_io_uring_seccomp.json" \
  --name host-tests -- ctest --test-dir "$BUILD" --output-on-failure \
  -R '^ninfer_(cpu_.*|file|durable_file|read_queue|uint128.*|artifact_cpu_materialization|artifact_file_io|ple_disk_prepare_host|context_cost|resource_manager|pretty_logging|serve_options)_test$'
"$COMMON/run-in-env.sh" --gpu --mount "$SOURCE" --mount "$BUILD" \
  --seccomp "$COMMON/ninfer_io_uring_seccomp.json" --name gpu-io-tests -- \
  ctest --test-dir "$BUILD" --output-on-failure \
  -R '^ninfer_(artifact_materialization|expert_cache|ple_disk_prepare_gpu)_test$'
```

The first Linux test container exposes the real driver library with zero visible devices. GPU I/O checks run without that mask. The older stub-driver image is not GPU qualification evidence.

### Results established so far

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

The full native link/host-suite and Linux shim gates remain incomplete; these rows are not an acceptance claim. The focused native core batch builds seven CPU/I/O/math executables and selects nine core/Python cases with zero visible CUDA devices. It is not a substitute for the blocked full suite. The architecture dependency's separate corrections and future native numerical/decode checks must not be mistaken for C1 test results.

## Preserved behavior and remaining gates

- Linux keeps POSIX direct I/O, liburing, pkg-config and directory fsync. Windows receives real platform implementations rather than omitted execution paths. No shipped family is intentionally dropped by this slice.
- The only explicitly different durability primitive is the lead-approved Windows directory-sync policy above. Native GPU arithmetic, integrated durable restart, concurrency/keep-warm, package/controller and release gates remain their owning slices' responsibilities.
- Final acceptance requires successful native host suites and `ninfer-serve` link, the Linux shim batch, and coordinated Windows GPU execution of artifact/expert-cache/PLE counterparts. Compilation or a registered test is never reported as an exercised pass.

## Current-step handoff

- Runtime source is pushed through `f689e6bde`; the current native core result is **8/9, not green**. No source-only or package qualification is asserted.
- K6 hard blocker: the approved native information-class-65 rename still returns sharing violation with strict parent pins, including the simplest closed-stream/same-parent case. Buffer termination, the Win32 wrapper and a retained reader are not the remaining cause. The interaction between directory pin access/sharing and native rename needs resolution without silently weakening the frozen security contract.
- ArchEnable2 has the BF16 native TMA correction `ca85f2573` and the native driver/staging handoff; that owner continues combined-source `89`/`86` operator/server/loading qualification. The full portable branch, not merely the six-file CPU fix, is required for Windows.
- Integrator2 owns provider integration and final cross-platform proofs. Contracts and DurableStore were notified of the exact K6 clean cutover. All C1 build/test observers and native commands from this step have returned; no production install/publication or GPU-host binary handoff was claimed.
