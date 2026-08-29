# NInfer-3090 v0.6.1 for Windows

This native Windows release supports Qwen3.8-27B, Qwen3.6-27B, and the compact, text-only
Qwen3.6-35B-A3B v0.3.1 artifact. The runtime provides paged KV, concurrent request execution,
compatible-prefix reuse, bounded admission, ReplaySSM, reasoning-effort control, and
OpenAI/Anthropic serving APIs.

## Requirements

- Windows 11 x64;
- GeForce RTX 3090 with a recent NVIDIA driver;
- Microsoft Visual C++ 2022 runtime;
- a supported `.ninfer` model artifact.

The bundled applications and dependency DLLs are native Windows executables. Model artifacts are
not included in the release archive.

## OMP v0.2 transactional package

The OMP v0.2 native package is built from the deterministic **tools/release/package.py** Windows
payload by **packaging/windows/qwen38-3090-omp-v0.2/New-Package.ps1**. The release is fixed to build
profile **omp-v0.2.0-rtx3090**, CUDA architecture **sm_86**, and the pinned Qwen3.8 artifact identity
in the release specification. The three executables and every declared app-local DLL are covered by
the inner and outer SHA-256 manifests. Package and checksum values are emitted in
**package-build-receipt.json** at build time. That immutable build receipt deliberately remains
`hardware-pending`. For the exact package SHA-256 it names, a later **passed** external
[beta qualification receipt](qualification/receipts/qwen3.8-27b-rtx-3090-v0.2.0.json) is the sole
post-hardware authority and explicitly supersedes that pre-hardware status. An incomplete receipt
does not supersede either pending authority. A final receipt does not rewrite the
historical build receipt or the packaged release specification: consumers with only the archive
must still treat it as hardware-pending until they verify the hash-bound external beta receipt.

The three checksum roles are distinct. Archive member **SHA256SUMS.txt** authenticates every file
inside the binary package. The product-stem `.SHA256SUMS` emitted by `package.py` binds the binary
package, source archive, and SPDX SBOM triad. The LF-only outer **SHA256SUMS** is the one closed
distribution set: it lists that inner triad manifest, the triad itself, all four separately
published lifecycle scripts, and **package-build-receipt.json**, each exactly once. The outer
manifest cannot and does not self-hash.

The default C1 profile binds to **127.0.0.1**, requires a one-line API-key file, uses 65,536 maximum
context, automatic INT8 KV capacity, a 1,024-token prefill chunk, MTP3, and authenticated durable
session checkpoints with a 64 GiB total quota and 256 MiB staging bound. A package-specific
configuration may instead bind to an IPv4 address in Tailscale's **100.64.0.0/10** range. Wildcard,
LAN, and public listen addresses are rejected, and CORS remains disabled.

Complete Windows beta qualification is GPU-only at the target 300 W cap and requires correctness,
context, process restart, performance, shipped lifecycle rollback, state-security, and exact OMP
acceptance on the same package. The current external receipt is incomplete and claims none of those
Windows gates. CPU-heavy, mixed-load, and overnight thermal claims remain outside the release gate.

### Current preview and deferred live-package gates

Package `b313904eb22a271d99d21d589075cc0102ab081e6835e9600f3c74c8d0cc48cc`
is the current preview. It binds runtime source `c5db3495ce08a6756ffb8962a3ca5142343239ef`
and package source `96b0101fde0483271ef002d8e6dfe3e431f8286f`. The earlier `09a9f24a...`
archive is superseded and must not ship. A clean neutral Windows build, deterministic package tests,
instrumented transaction tests, and focused native checkpoint contracts passed for the replacement.

The authorized COMMUNITY RTX 3090 lane passed the current-source 3/3 native contract build. It did
not yield usable live-model evidence: one exact-model attempt reached SHA verification before the
remote command exited, later CUDA 13.1 startup returned `cudaErrorUnknown`, the CUDA 12.8 device
probe also failed, and a subsequent provider allocation never published SSH. Every owned pod was
deleted, no secure-cloud substitution was made, and the receipt records protocol, 64K retrieval,
process-restart continuation, and C1 performance as `not_run` rather than reusing old counters.

The Windows RTX 3090 was released for user servicing before the rebuilt archive existed. Therefore
the exact current-package Windows install/security/bidirectional-rollback and OMP gates remain
deferred. The adjacent machine-readable receipt is incomplete and does not supersede either
immutable `hardware-pending` authority:

| Gate | Result |
| --- | --- |
| Exact replacement assets | Closed nine-entry outer checksum set; package/source/SBOM/build receipt are hash- and byte-bound |
| Neutral Windows build | `sm_86`; response-store, checkpoint-store, and HTTP-contract tests passed |
| Remote native contracts | Current runtime source; 3/3 focused tests passed; pod deleted |
| Fresh live protocol / 64K / restart / C1 | `not_run` — COMMUNITY CUDA runtime unavailable; prior-package results are not projected |
| Checkpoint deletion contracts | Failed-delete, cross-session LRU eviction, durable-only LRU delete, post-commit sync, and middle/latest/standalone restart shapes passed |
| Instrumented lifecycle | Passed with 22 enumerated substitutions; no shipped-byte or ACL claim |
| State-security fixture | Passed with instrumented GPU-power calls and prepared-lease restore; no hardware claim |
| Historical Windows evidence | Package `e74c097f...` at source `7555db29...` only; it does not apply to the current package |
| Fresh current-package Windows / OMP | Deferred after the authorized handoff; counters and evidence hashes are null/zero and no supersession is claimed |

This preview evidence is not beta authorization and does not authorize an
unattended evidence role. The separate frozen automatic-use corpus did not meet its quality floor,
so that route remains disabled. JSON-schema `response_format` is also unsupported and rejected
rather than ignored.

Responses `DELETE` is logical object deletion, not a secure-erasure primitive. A deleted response is
no longer addressable and is absent from the next durable transcript, including after process
restart. When a surviving descendant still depends on ancestor context, its Engine checkpoint may
retain the ancestor token/KV state needed for that continuation. Delete descendants before their
ancestors, then remove the appliance checkpoint/state root under the protected lifecycle, when
secure erasure of the complete session context is required. Deleting a standalone or latest response
leaves that response non-restorable; native tests cover middle, latest, and standalone shapes.
After a committed delete, the superseded inactive generation is eagerly tombstoned and reclaimed
even below quota. An active checkpoint reader or filesystem cleanup failure can defer physical stale
bytes until later quota pressure or whole-session/state-root removal; this is another reason not to
treat per-response deletion as secure erasure. A live LRU miss is restored from the authenticated
checkpoint before deletion, preventing a 404 followed by restart resurrection.

Run the installer from an elevated PowerShell session with the package hash from the immutable OMP
NInfer product manifest. A `SHA256SUMS` file delivered beside the archive is useful for local
diagnosis but is not an independent trust root:

~~~powershell
$manifestUrl = 'https://raw.githubusercontent.com/alphastorm/omp-ninfer/v0.2.0-beta.1/releases/v0.2.0-beta.1/manifest.json'
$manifest = Invoke-RestMethod -Uri $manifestUrl
$variant = @($manifest.components.ninfer_variants | Where-Object { $_.id -ceq 'rtx3090-windows-native' })
if ($variant.Count -ne 1) { throw 'RTX 3090 variant is absent or duplicated' }
$package = '.\ninfer-rtx3090-omp-v0.2.0-windows-x86_64-cuda12.8-rtx3090.tar.gz'
$packageSha = [string]$variant[0].package_sha256
.\Install-Release.ps1 -PackagePath $package -PackageSha256 $packageSha -ModelArtifactPath (Resolve-Path .\models\qwen3_8_27b.ninfer) -ApiKeyFile .\api-key.txt
~~~

The model stays at the supplied external path and is never copied into a candidate release. The
installer hashes it once, records immutable size/time/hash metadata, copies the one-line secret into
a release-scoped ACL-restricted file, prepares the candidate, and atomically advances
**prepared_release**, **active_release**, and **previous_release**. A repeated exact install returns
**already_installed** without rehashing the model. An interrupted transaction blocks re-entry until
the explicit repair command completes:

~~~powershell
.\Install-Release.ps1 -RepairInterruptedInstall
~~~

The installed controller supports **Status**, **Start**, **Stop**, **Restart**, **Rollback**, and
**Uninstall**. Restart validates the three executables and configuration without rehashing the
model. Rollback swaps only the exact active and previous release identities and restores the
original release if the target does not become ready. Uninstall removes package files, app-local
DLLs, caches, managed secret copies, state, and the scheduled task while preserving the external
model and restoring the prior GPU owner.

## Download the compatible Qwen3.6-35B artifact

The published RTX 3090 measurements use the compact 20.84 GiB container-v1 artifact. Pin its
revision because the Hugging Face repository's unpinned `main` file is now the larger 21.22 GiB
container-v2 artifact with DFlash weights:

```powershell
hf download neroued/Qwen3.6-35B-A3B-NInfer `
  qwen3_6_35b_a3b.ninfer `
  --revision c8b8c1c0df4c74df3c190c6aa3a7f24dc614721c `
  --local-dir models

Get-FileHash .\models\qwen3_6_35b_a3b.ninfer -Algorithm SHA256
```

Expected SHA-256:
`9e8378398d2b789a77224b5110c7590adbbc6fd4accd139b918157b2b9da7163`.

The v0.5 runtime reader accepts both v1 and v2 containers. An error that says only
`artifact magic is not NInfer version 1` comes from an older executable; replace it with the
[v0.5.0 Windows release](https://github.com/Don-Chad/ninfer-3090/releases/tag/v0.5.0-rtx3090).
Although v2 is readable, its DFlash-bearing payload is not the artifact used to qualify the 24 GB
3090 cohort profiles, so pinned v1 remains the recommended download.

## Run the concurrent server

Keep KV capacity explicit on a 24 GB card. Automatic sizing reserves an additional 1 GiB of
headroom and may reject an otherwise viable compact-35B configuration.

```powershell
.\ninfer-serve.exe models\qwen3_6_35b_a3b.ninfer `
  --host 127.0.0.1 --port 8080 `
  --api-key-file .\api-key.txt `
  --max-context 4096 --kv-capacity 4096 `
  --max-concurrency 4 --max-pending-requests 32 `
  --prefill-chunk 512 --kv-dtype int8 `
  --spec mtp --draft-tokens 3 --lm-head-draft
```

Prefix reuse is enabled by default; `--no-prefix-reuse` disables it. The server exposes OpenAI
Responses, OpenAI Chat Completions, and Anthropic Messages-compatible endpoints. Run
`.\ninfer-serve.exe --help` for the complete option list.

The compact 35B artifact does not contain DFlash weights. Do not select `--spec dflash`; the
runtime reports the missing optional weights explicitly.

## Qwen3.8-27B C8/8K profile

ReplaySSM reduces speculative GDN state memory enough for the maximum-concurrency Qwen3.8 profile
to use MTP3:

```powershell
.\ninfer-serve.exe models\qwen3_8_27b.ninfer `
  --host 127.0.0.1 --port 8080 `
  --api-key-file .\api-key.txt `
  --max-context 8192 --kv-capacity 8192 `
  --max-concurrency 8 --max-pending-requests 32 `
  --prefill-chunk 1024 --kv-dtype int8 `
  --spec mtp --draft-tokens 3 --lm-head-draft
```

This 8,192-token shared-pool configuration measured 114.73 aggregate end-to-end tok/s for eight
simultaneous 128-token generations and peaked at 21,818 MiB. Set `--kv-capacity 65536` when all
eight requests need independent 8K capacity; that stronger reservation measured 114.88 tok/s and
23,745 MiB peak. Avoid competing GPU processes.

The cohort size is fixed at startup, but its active membership is not: every decode round compacts
all ready requests into one batch, while completed slots disappear. Follow-up requests wait in the
bounded pending queue and join at a safe round boundary when a lane and memory are available. This
is more predictable than unrestricted dynamic batching because maximum VRAM, workspace, and CUDA
Graph shapes are reserved in advance.

Qwen3.8 supports `low`, `medium`, and `xhigh` reasoning effort. For Chat Completions add the
top-level field `"reasoning_effort": "xhigh"`; Responses uses
`"reasoning": {"effort": "xhigh"}`. The CLI accepts
`--reasoning-effort low|medium|xhigh`.

The paged cache supports BF16, INT8, and experimental opt-in `rk8v4` storage. INT8 remains the
recommended default. On the development RTX 3090, `rk8v4` raised the measured C1 automatic-sizing
boundary from 171,648 to 226,560 tokens with MTP and CUDA Graphs disabled and 1 GiB headroom, but a
matched hard-output test was not quality-equivalent. Use it only after validating your workload.

## Measured 35B capacity

These results used the compact 20.84 GiB artifact on an otherwise idle RTX 3090:

| Workload | Concurrency | Aggregate decode | Peak VRAM |
|---|---:|---:|---:|
| 128 output tokens/request | 1 | 162.7 tok/s | 21.90 GiB |
| 128 output tokens/request | 2 | 267.9 tok/s | 22.21 GiB |
| 128 output tokens/request | 4 | 366.2 tok/s | 22.83 GiB |
| 128 output tokens/request | 6 | 383.4 tok/s | 23.47 GiB |
| 512 output tokens/request | 2 | 399.1 tok/s | within 24 GB |

Concurrency 8 was rejected by admission rather than overcommitting the GPU. Repeating a compatible
26-token prompt reused 24 prefix tokens and reduced measured prefill from 371 ms to 10 ms.

## Build from source

Use Visual Studio 2022, CUDA 12.8 or newer, CMake, and vcpkg:

```powershell
$vcpkgToolchain = 'C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake'

cmake -S . -B build-windows -G 'Visual Studio 17 2022' -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$vcpkgToolchain" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DCMAKE_CUDA_ARCHITECTURES=86
cmake --build build-windows --config Release --parallel
```

The source rejects unsupported CUDA architectures for this fork. CUDA 13 uses MSVC's conforming
preprocessor automatically.

## Release validation

The v0.5 Windows release gate rebuilt `ninfer.exe`, `ninfer-serve.exe`, and `ninfer_bench.exe`,
loaded the official Qwen3.8 artifact, generated coherent output, and completed C1-C4 plus C8/8K
serving checks. Focused tests cover artifact reading/materialization, request memory, admission,
paged KV, prefix append, speculative rounds, and the relevant SM86 W8 linear paths.
