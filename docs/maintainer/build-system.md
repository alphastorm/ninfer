# Build system

NInfer builds from its source tree with CMake 3.28+ and C++/CUDA 20.
The supported architecture is `sm_120a`; CUDA 13.4 is the validated development toolkit.
Product commands and prerequisites are in the
[README](../../README.md#quick-start); test and measurement workflows live in
[tests](../../tests/README.md) and [benchmarks](../../bench/README.md).

## Configuration

The root `CMakeLists.txt` owns language/toolchain constraints, build options and top-level
composition. Architecture selection and validation happen before `project()` detects CUDA.
The default configuration is Release. Ninja links and archives share the single-slot
`ninfer_link` pool; compilation uses the build command's parallelism.

| Cache option | Default | Scope |
|---|---|---|
| `NINFER_BUILD_APPS` | ON | CLI, HTTP server and perplexity evaluator |
| `BUILD_TESTING` | OFF | C++ tests and registered Python interoperability tests |
| `NINFER_BUILD_BENCHMARKS` | OFF | Op, model, Engine and context-cost benchmarks |
| `NINFER_NATIVE_PREFILL_GROUP64_A8` | OFF | Opt-in sm_86/sm_89 group64 integer-activation prefill |

The native group64-A8 profile is a distinct numerical build profile, not a change to the artifact
or its stored Use records. Loading resolves an effective activation permission identically for
address-independent planning and bound operands: row-split Q4/Q5 group64 A16-only Uses become
AllowA8; other Uses, geometry and auxiliaries remain unchanged. Paired projections use the
intersection of their effective permissions. The profile is rejected for other architectures.

Above 32 columns, the supported 27B Q4/Q5 residual projections, Q4 fused SwiGLU and paired Q4/Q5
input projections quantize transient activations per token and 64-channel group, execute signed
INT8 tensor-core products, and accumulate group-scaled results in FP32. FP32 activation scales
avoid half-precision scale underflow. The paired input projections share one quantization;
SwiGLU retains its fused FP32 nonlinear epilogue. T <= 32 and unsupported shapes keep their
existing routes. Padding is explicitly written, output tails are masked, and weights are never
repacked or persistently copied.

Scratch is caller-owned and reported by the same layout helper used at execution:
`round_up(T,512) * (K + 4*K/64)` bytes. This replaces (rather than adds to) the Q4 SwiGLU
materialized-intermediate route where that route was selected. Model workspace and total VRAM
depend on the complete planner; the formula is not a measured VRAM delta. OFF omits these CUDA
translation units. Keep this option OFF for qualified profiles until matched throughput,
independent Op-oracle, exact long-retrieval and powered output-quality gates pass; ON binaries
must use a distinct `NINFER_BUILD_PROFILE` identity.

Apps or tests enable the internal product support components: media acquisition, prompt input,
logging and serving. This is one derived condition, not a separate user option. FFmpeg and libjpeg
belong to model media decoding and remain required when apps are disabled. CMake discovers libjpeg
as `JPEG::JPEG`; the native Windows manifest supplies it through `libjpeg-turbo`.
Curl and spdlog are needed only for product support. A Python 3 interpreter is found when tests are configured. The larger Python
pytest suites and conversion/evaluation scripts run separately from CMake and CTest.

CUDA 13.4 and Python 3.11 describe the maintained environment, not configuration version gates.
CMake also discovers FFmpeg without imposing library version floors. Actual language/API support
is exercised by compilation and tests. CPU facilities target Linux x86-64 and use hwloc >= 2.5
for topology discovery without changing the caller's binding. The internal `ninfer_cpu` target
is ordinary C++, links hwloc/Threads, and can run its tests without CUDA initialization or linkage.
Core direct-file I/O uses liburing >= 2.5, discovered through pkg-config. Its registered files and
buffers are owned by the read queue's caller; the queue creates no CUDA resources or worker threads.
The libcurl 7.85 minimum has a concrete API basis:
media acquisition uses `CURLOPT_PROTOCOLS_STR` and `CURLOPT_REDIR_PROTOCOLS_STR`, introduced in
[that release](https://curl.se/libcurl/c/CURLOPT_PROTOCOLS_STR.html).

The checked-in `CMakePresets.json` provides:

- `release`: Release product binaries, tests and benchmarks disabled;
- `dev`: Release products, tests and benchmarks enabled;
- matching build presets and a `dev` test preset with failure output enabled.

Both configure presets use `build/`. Switching between them explicitly resets all three build
options; it does not create independent build trees. Build with `cmake --build build -j`.
For a separate configuration, override the binary directory with `-B build-<name>` and use that
directory in subsequent build/test commands.

Keep machine paths in the ignored `CMakeUserPresets.json`. For example, replacing the interpreter
placeholder with the interpreter from the selected environment:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "local",
      "inherits": "dev",
      "cacheVariables": {
        "Python3_EXECUTABLE": "/path/to/python3"
      }
    }
  ]
}
```

Configure that environment with `cmake --preset local`. Compiler paths may also be supplied here;
changing compilers requires a fresh build directory. There is no tools option, installed SDK,
package export or configure-time dependency download.

## Targets and dependencies

[`cmake/Dependencies.cmake`](../../cmake/Dependencies.cmake) owns system dependency discovery,
repository-pinned third-party targets and their product-support conditions. External include
requirements follow their consuming targets.
[`cmake/NinferTargets.cmake`](../../cmake/NinferTargets.cmake) provides private project includes
and the two CUDA archive policies.

[`src/CMakeLists.txt`](../../src/CMakeLists.txt) explicitly enters the component directories.
Each component's `CMakeLists.txt` owns its targets, dependency visibility and compile properties;
those files provide the current target and dependency inventory.

Component declaration order is not the execution dependency graph: CMake resolves named target
links during generation. Model runtime can refer to the runtime support target declared later;
support links core, while Engine links model runtime. The ownership and dependency contracts are
defined by [Engine architecture](engine-architecture.md) and [Op development](op-development.md).

## Explicit source ownership

Small components list their sources directly. Larger components include local `sources.cmake`
manifests which use `target_sources(<owner> PRIVATE ...)`. Included manifests anchor file paths
with `CMAKE_CURRENT_LIST_DIR`; `include()` does not change `CMAKE_CURRENT_SOURCE_DIR`.

- Every implementation under `src/` has one target owner and one explicit registration.
- Ops register implementations through their owning family manifests. Shared weight-input
  preparation and compile policy are owned by `src/ops/CMakeLists.txt`.
- Linear separates shared kernel and projection manifests. Format manifests list explicit sources
  and shape filenames with their actual `.cpp`/`.cu` extensions.
- Models retain distinct immutable-loading and mutable-runtime targets. Native loading, execution
  and Program manifests own architecture code; `common/qwen/sources.cmake` owns shared Frontend
  and Vision sources. Shared bindings and context/MoE mechanisms have explicit target entries.
  Runtime startup orchestration links model runtime; native startup planners only depend on
  runtime support/contracts. Checkpoints/recipes do not define additional compile owners.
- New files require an explicit manifest edit. New families require an explicit parent include.
  There is no recursive glob, directory discovery or generated shape/dispatch registry.

Adding a source manifest does not require another library. Add a target only for a meaningful
dependency, reuse or compilation boundary. Shape semantics, supported domains and finite
dispatch remain in C++; CMake only selects translation units.

## CUDA compilation boundaries

`ninfer_core` and `ninfer_ops` enable separable compilation (RDC) and resolve device symbols in
the static archive build. Keep this device-link boundary explicit.

`ninfer_nvfp4_non_rdc` disables separable compilation and device-symbol resolution. It contains
the warp-specialized Linear, LinearSwiGLU and causal-attention NVFP4 sources that depend on
`setmaxnreg` register transfer. Their owning family manifests register these sources into the
non-RDC target; host launchers connect them to the normal Ops. All three CUDA archive targets
retain `-lineinfo`.

Source-list maintenance must preserve language, architecture, RDC mode, device-link ownership
and numerical compiler options. Splitting a manifest does not reduce kernel instantiation work
or justify combining shape translation units or enabling unity builds.

## Consumers and verification

`apps/CMakeLists.txt` explicitly defines the three product executables. Tests and benchmarks
include their domain registrations without creating new CMake subdirectories, preserving
`build/tests/`, `build/bench/` and CTest working directories. The helpers are local to those
consumers. Tests explicitly name their linked libraries; public-header and pure-host checks keep
their isolated include/link requirements.

Tests can reuse compiled support libraries. Oracle options apply to the libraries containing
oracle code as well as the test executables. Test variants, skip return codes,
interop commands and special linker options belong with the affected registrations.

For build-organization changes, compare configured target/source ownership, compile flags,
device links and CTest commands/properties; check the relevant option combinations, build the
affected targets and run their behavioral tests. A shape-source edit should rebuild its own
translation unit plus necessary downstream links. Build-time or inference-speed claims require
measurements at that scope.
