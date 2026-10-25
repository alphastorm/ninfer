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
- `run-in-env.sh` accepts `--gpu`, repeatable `--mount` / `--read-only-mount`, `--env KEY=value`, `--seccomp FILE`, and `--name SUFFIX`. It uses host networking; servers must bind loopback and their holder's assigned port. Containers are always `ov-<holder>-<suffix>`, auto-removed, run as the caller's UID/GID, and use a persistent 20 GB ccache under the caller's cache directory. Image/cache overrides are `NINFER_BUILD_ENV_IMAGE` and `NINFER_CCACHE_DIR`.
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
