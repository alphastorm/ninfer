# NInfer native Windows lanes

The native dependency manifest includes libjpeg-turbo for the Pillow-compatible
JPEG decoder. CMake resolves `JPEG::JPEG` explicitly; native runtime packaging
continues to collect its transitive DLLs from the selected vcpkg installation.

## 2026-10-29 — Native component 0.8.0-beta.1 (qualification pending)

The RTX 3090 candidate switches from MTP3 to DFlash2 K=7, with one concurrent
request, INT8 KV and a 131,072-token context ceiling. Its 24 GB fit uses zero
extra device state slots, 768 MiB admission headroom, load-time Q8-to-Q4
(`Q4_G64_FP16`) conversion of draft MLP gate/up weights and host-mapped token
embeddings. The conversion and mapping apply only to DFlash2 on `sm_86`/`sm_89`;
other weights retain their artifact representation. The RTX 4090 is rebuilt
from the same source with its existing MTP3 fit: two extra device state slots,
640 MiB admission headroom and artifact-format, device-resident weights.
Both lanes identify component `0.8.0-beta.1`, with lineage base `752dd887`.
The published model artifact, keep-warm settings and GPU-owner policies are unchanged.

Release specifications are now schema 5; server configurations remain schema 4.
`runtime_shape.fit` binds device state slots, admission headroom, draft MLP format
and embedding residency to the selected backend/architecture. Packaging and
installation reject schema-4 release specifications, missing fit/proposal policy,
and configurations that disagree with the specification. The untouched experimental
RTX 5090 Windows specification remains schema 4 and cannot be built by this packager.

Host context remains an exact translation of the predecessor Host KV budget plus
24 native state images of 153,954,304 bytes each: 12,284,837,888 bytes
(11,715.734375 MiB) on the RTX 3090 and 15,506,063,360 bytes
(14,787.734375 MiB) on the RTX 4090. The P14 RTX 3090 DFlash2 fit proof used
16,384 MiB instead; that run does not qualify this shipping Host budget.
The DFlash2 state-image size must be re-measured on the final build, and the exact
shipping configuration must pass the native context, restart, quality and performance
gates before promotion. Package-build receipts remain `hardware-pending`.
Startup JSON reports `artifact.effective_weights.converted_device_bytes` and
`artifact.effective_weights.host_mapped_bytes`; status exposes the same
`effective_weights` object. Qualification of this model's native DFlash2 fit must
observe both counts greater than zero rather than assume the conversion/mapping ran.

Upgrade with the candidate's `Install-Release.ps1` into the existing lane state
root. The installer stops the incumbent through its own controller and retains the
previous release for `Control-Release.ps1 -Action Rollback`; package and cache roots
remain isolated by release. **RTX 3090 MTP durable sessions do not carry across the
DFlash2 upgrade.** Start new sessions after upgrading. The incumbent's checkpoint
root is preserved for rollback, never imported into DFlash2. Qualification must
exercise upgrade and exact rollback to the `v0.7.0-beta.3` incumbent.

## 2026-10-26 — RTX 3090 qualified power cap 350 W

The RTX 3090 lane's managed GPU-owner lease now caps the card at 350 W instead of
300 W (owner-approved). `qualified_power_limit_w` is 350 and the C1 qualification
envelope `managed_power_envelope_w` is 351 W (the same 1 W sampling allowance). The
owner's 370 W default that release restores, the accepted prior-limit range
(100-390 W) and every RTX 4090 value are unchanged. The `beta.2` RTX 3090 qualification
ran at 300 W, so a package built from this specification needs a fresh RTX 3090
qualification at 350 W before it can claim a hardware pass.

## 2026-10-26 — Experimental RTX 5090 Windows shape (not a release)

`lanes/rtx5090` describes a native `sm_120a` feasibility build, not a qualified
component or a replacement for the container. It retains DFlash2 K=7, BF16 KV,
131,072 context, 146,368 fixed KV tokens, two concurrent requests, vision,
1,024-token prefill chunks and the container's 20,867.734375 MiB Host budget.
The Host translation records the historical budget, not a measured DFlash2
state-image size. Model bytes and schema-3 checkpoints are unchanged.

The shared packager and installer accept that lane's DFlash2/C2 profile without
loosening the shipped RTX 4090/3090 MTP/C1 shape. Each lane's immutable
`runtime_shape` declares concurrency, speculative backend and draft-token bounds;
shared lifecycle scripts select no policy from a GPU name or lane-number literal.
An absent runtime policy is refused. The controller passes DFlash2
and the optimized proposal head explicitly. The existing qualification
orchestrator's MTP pressure recipe is not an RTX 5090 acceptance procedure.
Use isolated experiment roots; no managed installation or release promotion is
implied. Native build, smoke and comparison evidence must establish whether
all-native packaging is worthwhile before a release qualification is attempted.

The MSVC host stub cannot take 128-byte-aligned TMA aggregates by value. BF16,
FP8 and NVFP4 descriptor types therefore retain explicit alignment in device
compilation and on non-MSVC hosts, while Windows host aggregates use the CUDA
header's host alignment. Encoder-local storage remains explicitly aligned.
This does not replace TMA with another kernel or add descriptor allocations.
`ninfer_tma_descriptor_abi_test` checks real TMA transfers through each production
descriptor type, both directly and through a CUDA graph, on `sm_120a`.

The standalone feasibility build `92a9995e` compiled with MSVC 19.44 / CUDA
13.3.73 and passed the full-shape smoke on an RTX PRO 6000 Blackwell (96 GB,
driver 616.92, 600 W). This is **not an RTX 5090 result**: the actual-card lease
was not acquired. The 4,541-fresh-token / 1,024-output-token C1 run measured
461.95 decode tokens/s and 4,108.50 prefill tokens/s, with 31,382 MiB ready/peak
device usage. Real TMA descriptor loads passed directly and through CUDA graphs
for BF16, FP8 and NVFP4; the three affected Linux sm_120a shape translation units
produced byte-identical SASS against the base.

The separate Nsight-instrumented PRO 6000 C1/C2 workload measured 1.186 s C1
TTFT and 2.167/2.356 s C2 TTFT. Its round-boundary GPU idle median was 560.17 us;
in-round `cudaGraphLaunch` CPU time averaged 283.24 us. These different-host,
instrumented numbers do not establish an improvement over the container.
Authenticated schema-3 checkpoints passed two cold-process exact-key restores
through DirectStorage 1.3: the initial 3.531 GB payload save took 35.24 s, and
restore-plus-continuation requests took 17.62/26.94 s (restart/load time excluded).
All-native release promotion remains withheld pending a same-5090 comparison
and full output qualification. The later ordered S6/S7/S8 integration head needs
its own build and qualification; these measurements bind the standalone build.

The native build workstation subsequently failed a hardware stress check and
reported a memory-corruption bugcheck. All native binaries produced there,
including those built before the failure, and the measurements above are now
**provisional diagnostic evidence only**, not qualification or integration-ready
evidence. A stable-host deterministic double build with matching binary hashes,
followed by the native gates, is required before those claims can be reinstated.
The independent Linux SASS comparisons are unaffected.

## 2026-10-24 — Upstream-master native packages (qualification pending)

The native component candidates are `v0.7.0-qwen38-4090-beta.2` and
`v0.7.0-qwen38-3090-beta.2`. They use upstream master `64d10fbf`, the published
model-format-v3 artifact `81f924d4`, and schema-4 release specifications/server configurations.
Package-build receipts remain `hardware-pending`; only a completed qualification
and restoration chain can establish a hardware pass.

Both native lanes pin the same published file as the container lane: revision
`1cbd84e7221e51186bd7f093a149912d2489625b`, 20,437,521,664 bytes. They do not pin a
native-specific upgrade. MTP3/INT8 KV remain explicit; the artifact's inactive
DFlash2 component must not be materialized by the native MTP configuration, which
is a separate native runtime qualification assertion.
The controller and pressure launcher retain `--lm-head-draft` for the optimized
MTP proposal head; the upstream default full head is not the native profile.
The v0.13.1 qualification profiles explicitly require checkpoint origin
authentication and admission headroom of 640 MiB (RTX 4090) / 768 MiB (RTX 3090).
Public profiles retain port 18082; port-only scratch rehearsals have their own
config bytes, hashes and provenance and do not replace the public identity.

Master's `--host-context-mib` replaces the fork's separate Host KV/State pools.
The layout-aware translation preserves the old Host KV budget plus 24 MTP
StateImages of 153,954,304 bytes each: 15,506,063,360 bytes (14,787.734375 MiB) on
the RTX 4090, and 12,284,837,888 bytes (11,715.734375 MiB) on the RTX 3090.
These are exact byte translations, not new smaller-host qualifications. Device
state slots remain extra capacity beyond the active lane (2/4 respectively).
Master owns its catalog/retention policy; obsolete private-slot limits are not
passed to the new binary. INT8 KV, MTP3, context 131,072 and the existing power,
prefill and keep-warm settings are unchanged.

The shared-CUDA and `/MD` build ships its app-local CUDA/MSVC runtime DLLs, not
just vcpkg and DirectStorage. The qualification collector follows transitive
imports through the selected CUDA compiler's bin directories, VS 2022 CRT
redistributable and vcpkg tree. Both DirectStorage DLLs enter that graph,
including the dynamically loaded core. Missing selected CUDA/CRT imports or a
failed import inspection refuse packaging rather than relying on a development
toolkit or an unrelated DLL on the target's `PATH`.

The shared installer/controller/owner-lease shape, protected ACLs and bearer-file
handling are retained. Readiness consumes status schema 2 and requires this
process's request-log schema-30 startup to attest the selected UUID, CUDA ordinal
and GPU name within 600 seconds. Only the server child receives
`CUDA_DEVICE_ORDER=PCI_BUS_ID`; `CUDA_VISIBLE_DEVICES` must be absent. Packaged
protocol checks now exercise supported Chat/Responses JSON schemas and
128-character tool names rather than expecting structured output to be rejected.
Authenticated shared-prefix reuse is valid across sessions; private continuation
selection is not.

The selected-card interactive ceiling remains 1 GiB. Its measurement is now the
UUID-indexed `nvidia-smi --query-gpu=memory.used` value, converted from MiB to bytes,
matching native acceptance. WDDM per-process `DedicatedUsage` is commit/budget
diagnostic data, not resident usage, and cannot refuse a start or replace a failed
NVML observation. Status keeps the WDDM process table separately, including an
explicit unavailable diagnostic when that provider fails.

Packages carry the repository's canonical `tools/smoke/agent_protocol.py` and
`serve_contract.py`; the older Windows-only protocol copy is removed. C1 and
pressure invoke the explicit product contract. Its private-user-context reuse
probe preserves the strict private/foreign path, quiescence, greedy parity and
decode-correlation gates; a tiny prompt that legitimately selects cost-first
root prefill is not used as the reuse oracle. Qualification retains the real
schema-2 result beneath each phase receipt, not invented legacy checks/status.

Each new release instance writes only its own `session-checkpoints-v3` root
(outer schema 3/native checkpoint-set format 1). Earlier roots are preserved for
rollback and are never discovered or imported by the new runtime. The
DirectStorage queue creates its root through the common K6 no-follow directory
primitive before constructing GPU/DXGI resources, preserving the existing
checkpoint-root failure context.
An older release's own schema-3 configuration remains capability-gated to its own binary,
status schema and isolated checkpoint root. Managed stop signals the per-launch
event, waits for synchronous final saves, and accepts only a launch-bound
schema-1 shutdown report with `complete=true` and no refused sessions. Skipped
sessions are counted, not treated as failed saves. Restart qualification checks
the real scheduler/reuse counters reset to a measured cold-process baseline before
continuing either saved session. The server's cache-disabled startup warmup is real
prefill/decode work, so absolute zero is not its baseline. The proof cold-starts
before seeding, requires a new PID and exact equality with all three measured
counters after restart, and records both snapshots without hard-coded token counts.
An idempotent Start preserves stopped-entry recovery before the strictly graceful
cold setup; the tested restart's graceful-stop requirements are unchanged.
Both marker seeds use the canonical approximately 1.6K-token private user context;
exact marker recall and positive cached-input tokens remain mandatory for both
explicit publication and managed-stop-only persistence.
Explicit saves must acknowledge `schema: "ninfer-durable-operation/1"` with
authenticated, committed native publication and real coverage/counts; checkpoint
status separately requires schema 3. Neither accepts the predecessor's envelope.
Public qualification phase receipts hash host-route and incumbent-root bindings;
restoration summaries expose a bounded failure reason and diagnostic digest, not
raw operator errors. The local orchestration state retains its private write-ahead
recovery information and is not a public release asset.

Disposable qualification installs may supply `-ScratchTaskSuffix C3`, creating
the specification's task name plus `-Scratch-C3` without changing the production
task. The canonical product root refuses that option. Use the existing
`New-Package.ps1 -ServerConfig` override to bind a scratch port into the package's
exact configuration hash; the native-operations rehearsal uses port 18210 and a
separate protected state root. This option grants no production-route mutation.

The v0.13.1 orchestrator requires an explicit scratch root, task suffix, protected
operator-key path, C1 port and pressure port. It neither discovers bearer files in
shipped roots nor unregisters a canonical task for a clean-install test. Suffix
`I6` uses C1 ports 18210/18211 and pressure ports 18200/18201 for the two lanes.
The clean-install task has a separate `-Clean` suffix. Restoration checks only
scratch-owned processes and the selected card's lease/power state.
Each staging root is itself atomically created with the protected administrator/
SYSTEM DACL, not merely inherited from a protected parent. Temporary protection
helpers include the lane and a per-invocation nonce so simultaneous lane installs
cannot replace or remove one another's helper.

`--qualified-runtime-bin` and `--qualified-build-receipt` consume the integrator's
exact three-executable/MSVC runtime set without rebuilding it. Staging verifies
copy hashes and each executable's clean, matching source/profile/architecture
identity. Public and port-only scratch packages are each built twice, including
the complete outer checksum set; the emitted equivalence binds both package and
configuration hashes plus identical executable/controller bytes.
This release binds the binary patch-stack identity, `RuntimeSourceSha` and
packaging/source-archive commit to the same cut commit. Older executable hashes
are not relabelled. Staging directories are keyed by that commit so a fresh
install cannot reuse a predecessor's prepared root. The integrator rebuilds and
compares actual payload hashes before deciding which prior test evidence is
content-address equivalent.

`New-Package.ps1` requires the product P4 generator and genuine resolved vcpkg,
CUDA, vendored-source and CRT/DirectStorage inventories. The outer distribution
adds `<package-stem>.cdx.json` (CycloneDX 1.5) without replacing the file-level SPDX
or inner checksum asset. `SHA256SUMS` closes exactly ten files (eleven including
itself); the package-build receipt binds the dependency SBOM, generator and input
digests. Hardware status remains pending in that immutable build receipt.

A completed and restored orchestration also emits `<lane>.json` in the product's
native-qualification shape. Its public package/configuration binding is separate
from the unmodified observed scratch identity and launch attestation. Protocol,
shared-pool pressure, context, restart, rollback, security, client and benchmark
fields are adapted from actual phase receipts; failed/missing evidence cannot
become passing flags. A first-lineage rollback remains `not_applicable`, not passed.

Shared-pool pressure failures retain their scratch checkpoint store, request log
and drained stdout/stderr. A retained pressure root refuses a new attempt until
an operator explicitly authorizes its cleanup; it is never silently freshened.
Automatic scratch removal occurs only after a passing remote receipt and local
validation of the canonical product protocol result. Transport failures,
non-passing receipts and malformed protocol payloads preserve the evidence.

The UUID-scoped C1 benchmark preserves fractional `power.draw` and
`temperature.gpu` telemetry from `nvidia-smi`. Utilization and used-memory retain
their integer units; maximum tracking and the power-envelope refusal are unchanged.

Native admission waits within the existing pending-request timeout when an
in-flight checkpoint export temporarily pins the device states needed for reclaim.
Successful or failed export completion releases those pins before waking admission;
durable coverage checks and protection of unsaved session progress are unchanged.

Privacy qualification has two explicit domains. Shipped executable/DLL directories
and both public/scratch payload archives reject every Windows user-profile spelling,
macOS user-directory or Linux home-directory path, plus the known private host names
and credential shapes. Forward separators and escaped Windows separators are covered;
a zero result for only the original spelling is not sufficient. Bare forbidden root
prefixes are rejected even when no owner component follows; source reference reporting
retains its separate owner-aware interpretation.

Source trees and source archives reject actual operator homes/hosts and credential
shapes. Public upstream paths, placeholder examples and HTTP route examples are
reported separately as public/generic references, using structured path components
rather than raw absolute paths in receipts. Scanner definitions and fixtures construct
their marker strings at runtime, so the scanner does not match its own definitions.
The private host/home scope stays in the operator checkpoint; public receipts retain
only its digest and the generic reference list. The source-archive hash/size is bound
in the immutable package-build receipt and all three archive privacy scans must pass.

## Historical predecessor notes

The sections below describe the fork-based releases and their qualification
history, not additional capabilities or acceptance evidence for this candidate.

## 2026-10-11 — Native lane versions for multi-GPU hosts (qualification pending)

RTX 3090 advances from `v0.6.2-beta.1` to `v0.6.3-beta.1`; RTX 4090 advances from
`v0.6.10-beta.1` to `v0.6.11-beta.1`. These are new packages for the UUID-bound
multi-GPU-host lifecycle scripts. Runtime source is unchanged. Hardware qualification
of both new packages is pending.

## 2026-10-10 — UUID-bound multi-GPU hosts (qualification pending)

Native RTX 3090 and RTX 4090 packaging can select its own card on a host with several
NVIDIA GPUs. Install with optional `-GpuUuid` (the full NVIDIA UUID); without it, exactly
one card must match the lane's GPU name and compute capability. The release records that
UUID, name, installation-time index, and canonical PCI address. Start resolves the UUID's
current index, so adding another card does not retarget an older release. Older records
without a PCI address retain UUID-and-name binding. Packaged config bytes and hash identities
are unchanged; `engine.device` is not the hardware selector.

Only the server child receives `CUDA_DEVICE_ORDER=PCI_BUS_ID`. The operator must still leave
`CUDA_VISIBLE_DEVICES` absent: hiding cards is not supported. Readiness requires this launch's
`server_start.environment.gpu_uuid` to match the release binding within 600 seconds; mismatch
or missing evidence stops the child. Request logs stay append-only: the wrapper captures the
pre-launch byte offset and file creation identity, reads only complete new JSONL records,
and requires `server_instance_id` to contain this child's PID. Truncation, replacement,
wrong-process startup records, and a 1 MiB search without evidence fail closed.

Every owner-controller power query/set is UUID-indexed. Interactive workload accounting maps
GPUAdapterMemory LUIDs through D3DKMT to the selected PCI bus/device/function, then counts
only GPUProcessMemory instances on those LUIDs. An idle selected adapter with no process
instances is valid; another card's desktop workload does not block it. Missing or unmappable
selected-adapter counters, unmappable active process LUIDs, malformed active counters, and
nonzero PCI domains refuse observation rather than
assuming an idle GPU. Software adapters with no PCI device are excluded. A controller invoked
without `-GpuUuid` refuses a multi-GPU host.

Install defaults to the bundled UUID-aware owner controller on both clean installs and
upgrades; explicit adapters must declare `-GpuUuid` and attest UUID/PCI in status. Inner
leases bind both UUID and PCI and validate policy before any restore. Old unbound active
owner leases must be restored safely with their original controller before upgrading; they
are never guessed onto a card. A stopped, lease-free predecessor remains rollback-capable,
including when its recorded GPU index is stale. A running legacy unscoped wrapper blocks
upgrade/qualification; it is never stopped through its unscoped owner on the multi-GPU host.
No lane version or release spec is changed.

Pre-UUID packages are unsupported on multi-GPU hosts: their owner restore uses an unindexed
`nvidia-smi -pl`, which sets every visible card. This also applies when recovering an old
lease; do not run that legacy restore on a multi-GPU host. Binary predecessor rollback under
the current UUID-aware shared controller is distinct from running a pre-UUID package's scripts.

Both managed Start and scheduled Run validate release layout/hashes, the selected GPU identity
and absence of `CUDA_VISIBLE_DEVICES`, and port availability before acquiring a power-changing
GPU-owner lease. Repeated owner stop preserves the caller's explicit or implicit UUID selection,
including the legacy lease behavior on a single-visible-GPU host.

Both lanes still listen on port 18082 and retain separate task names and state roots.
**Only one lane may run at a time.** Start (including direct scheduled Run) refuses an occupied
port before acquiring a GPU-owner lease. Simultaneous lane operation is not supported.
The native qualification harness accepts `--gpu-uuid`, applies the same unique-qualified-card
selection rule, and binds clean/upgrade installs, direct pressure launches, security checks,
power restoration probes, and benchmark GPU metrics to that card. Fresh real-host acceptance
is still required; this packaging change is not a release or a hardware qualification claim.

Qualification preflight runs again on every resume. It records the incumbent's root and running
state durably before stopping anything, and cleanup uses that record even when preflight fails.
An interrupted window retains an outstanding incumbent restart obligation until restoration passes.
The qualification summary remains `restore-pending` until restoration passes; failed restoration
cannot leave a passed summary. A resumed window invalidates any prior passed summary before stopping
the incumbent, and successful cleanup does not hide a failed qualification phase.

One runtime for the RTX 4090 (`sm_89`) and RTX 3090 (`sm_86`) lanes, built from mainline
`port/native-lanes-on-mainline` rather than the two divergent lane branches. The package carries
the same context cache, warm arrival across a restart, and streamed verified-or-refused restore
that ship on the RTX 5090 container lane, with Windows platform support, the DirectStorage read
queue, and the MSVC build of the host tree.

Each lane installs into its own isolated state root through `Install-Release.ps1`, runs one
authenticated request at a time on a loopback or Tailscale address under a scheduled task owned by
SYSTEM, and keeps durable session checkpoints under its per-release cache. The lane's exact
engine configuration is bound by hash into the build identity; the lane specification names the
GPU, architecture, power policy, and task identity the installer enforces.

The lane configuration sizes the pinned host pools (`context_cache.host_state_slots`,
`context_cache.host_kv_mib`) for the host that runs the lane. The RTX 4090 keeps the qualified
11,264 MiB Host KV pool, 24 host state slots, and 32,768 MiB runtime-host floor. A 4 GiB pool
could serve a ceiling-sized session but could not restore its checkpoint; startup now discloses
the restorable token bound and export refuses a session larger than that bound. The RTX 3090
profile is unchanged. No smaller-host deployment profile is qualified by this change.

When shared capacity blocks restore, the engine may reclaim another inactive resident session
whose checkpoint is current. The verified generation stays pinned against quota eviction for
the entire restore, including saves of later victims. A resident session ahead of its checkpoint
is saved and pinned first; a refused save leaves that resident intact. Every retry reads from a
fresh checkpoint reader. The retry bound allows two progress steps per catalog slot plus the final
import, so new arrivals cannot extend one restore indefinitely. State image slots, KV capacity,
and continuation slots all participate in guarded reclaim. Normal disk
quota retention still applies after the restore finishes.

Live sessions survive a graceful stop. An automatic checkpoint refused at a transient gate - the
newest turn not yet catalogued, or another request's transaction in progress - retries up to six
times per completed turn once the engine quiesces. Before admission evicts a session whose newest
turn is not on disk, the engine saves it, waiting while that turn's reply is still being stored;
the admitting request waits, bounded by its own queue deadline. Re-saving a session under the
disk quota no longer deletes other sessions' only checkpoints, including when a cleanup fails. A
graceful stop counts a session whose newest response is already on disk as nothing to save.
Automatic checkpointing remains best effort under live traffic; explicit checkpoint requests and
graceful managed shutdown provide the observable save outcome.

Unmodified OpenAI clients get durable sessions. A Responses or chat request's `prompt_cache_key`
becomes the session identity when API authentication is configured: the key is hashed under its
own domain and never stored raw, and a request that also sends `ninfer_session` or
`X-NInfer-Session` is refused on both endpoints. Without authentication the key names nothing and
the request stays in the anonymous pool; a null key is the same as no key. A client that resumes
after both it and the server restarted, and so replays its transcript without
`previous_response_id`, has its session's checkpoint restored on that session's first request
since the start; the engine's exact prefix match decides how much it reuses. This applies to every
session name, so a `ninfer_session` or `X-NInfer-Session` client that opens a new conversation
under a saved session name also imports that checkpoint first. Requests without a session identity
behave exactly as before. The endpoint still rejects reasoning summaries, encrypted reasoning
requests, and the other cache options it does not implement.

A tool result may be content parts instead of a string: a Responses `function_call_output` or
`custom_tool_call_output` whose `output` is an array of `input_text` and `input_image` parts - what
stock OMP sends after its read tool opens an image file - becomes one tool turn with its image.
This lane is text-only, so it refuses that image as `vision_disabled` rather than as a malformed
request; `input_file` parts and empty arrays are refused.

The shared-prefix catalog defaults to one owner per active request or per cache marker a request
may place, whichever is larger (four at one active request), so each agent type's system and tool
prefix keeps its own owner instead of evicting the previous type's.

A start could fail when the driver refused to pin the 11 GiB host-KV pool
(alphastorm/omp-ninfer#48): the lane's task exited before the release was ready. On a 32 GiB host
the running server commits about 39 GiB, so a start extends the system-managed pagefile, and the
pool is its largest charge. A pin charges its size and then a page-lock remainder of 11-20 MiB;
when free commit covered the size but not the remainder, the pin raced the pagefile extension, and
a start that lost was refused with 10 MiB of commit free. Before every pinned host allocation the
server now commits and releases its size plus 1/64 through an ordinary allocation, which waits for
the pagefile to extend, so the pin itself never waits on an extension. A refusal that still occurs
reports the commit limit, available commit and available memory.

Decode rounds are shorter. The MTP verify pass's small-extent Q4 and Q5 input projections share
each activation load across weight rows, and the Q4 MLP gate/up kernel pads its staged weight rows
so its shared-memory reads no longer conflict. Each row keeps its arithmetic order. On the RTX 5090,
where the routes were tuned, outputs are bit-identical to the previous kernels and MTP3 decode is
about 10% faster from 1K to 31K tokens of context. The native lanes build these routes for their
architecture, except that the MLP down and mixer output projections keep one row per warp: sharing
loads across two rows made them 2-22% slower on the RTX 4090.

New sessions after idle start at back-to-back speed on the RTX 4090. After its last work the card
steps P2 -> P3 -> P5 -> P8 (210 MHz SM, 405 MHz memory) within about 6 s, and a new session's
prefill after 12-58 s idle took 0.29-0.47 s against 0.146 s back to back. The RTX 4090 lane sets
`engine.gpu_keep_warm_ms` to 60000: for 60 s after the engine goes idle, a single-warp kernel on
its own stream spins 50 ms of every 100 ms, which holds P2 at about 72 W above idle, until a
request is pending. The RTX 5090's pattern of 3.5 ms every 10 ms does not hold this card. With it,
sessions after 12-58 s idle prefilled in 0.146 s and every output was unchanged (omp-ninfer EXP-064,
EXP-066). The spin reads and writes no memory. The controller passes `--gpu-keep-warm-ms` only when
a release's own configuration declares a positive value, so a rollback to an earlier release never
receives it. The RTX 3090 lane declares 0.

Long sessions keep their cache. OMP's compaction handoff resends the whole session with
`tool_choice: none`, and the runtime rendered that turn without the declared tools, which Qwen
places before the leading instruction: every handoff re-prefilled the session from root (63.8-66.3 s
to first token for 97.4-97.5K tokens on the RTX 4090). The tools now render for `none` as for
`auto`, and the template's tool-call opening token ends the turn instead. Handoffs of 78.4-80.4K
tokens reused 78.0-79.9K cached tokens and started in 0.58-0.70 s, and their summaries ran
3,235-5,241 bytes.

Near capacity the materialization planner's 5 ms search can stop before it reaches a target that
keeps the session's own continuation. It then fell back to evicting that continuation and
prefilling the whole turn again from root (about 102,600 tokens and 68 s on the RTX 4090,
omp-ninfer EXP-072). The fallback now starts from the cheapest feasible target that keeps it: in
three such stops during a long session, each turn reused 79.2-101.6K cached tokens.

A crash after a compaction restored the pre-compaction checkpoint. The compacted session's turns
stayed under the 32,768-token automatic-save minimum, and an unstored handoff under the session's
key could move its indexed lineage. A session that has a checkpoint on disk now keeps it current at
any size, and an unstored Responses turn leaves the lineage alone. After a hard kill, the next turn
reused 32,167 cached tokens in 2.9 s; the previous runtime prefilled 32,173 tokens from root
(29.2 s).

Long sessions keep their checkpoints when short sessions fill the store. Quota reclamation took the
oldest checkpoint first, so a run of short sessions could delete every long session's checkpoint:
about 26 short OMP sessions of about 930 MiB each filled the RTX 5090's 24 GiB store. Checkpoints
below `--session-checkpoint-min-tokens` now go first. On the RTX 4090, a 60,026-token session
(2.30 GiB checkpoint), then 11.9K-token sessions (702-704 MiB each) past the 24 GiB quota, then a
crash: the long session's next turn reused 60,057 cached tokens in 3.1 s. The previous runtime had
reclaimed its checkpoint first, and the same turn failed with `previous_response_not_found`.

A controller action no longer fails when the server is writing a checkpoint. Every action walks
the lane's state tree, and the walk could reach a checkpoint staging directory the server had just
renamed into its generation (`Cannot find path ...\.staging-...`). An entry that disappears during
the walk is now skipped; any other failure still stops the action.
