# NInfer native Windows lanes

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
fresh checkpoint reader, and retries require progress rather than a fixed attempt cutoff. State
image slots, KV capacity, and continuation slots all participate in guarded reclaim. Normal disk
quota retention still applies after the restore finishes. Automatic checkpointing remains best
effort under live traffic;
explicit checkpoint requests and graceful managed shutdown provide the observable save outcome.

The Responses endpoint continues to reject unsupported cache hints, reasoning summaries, and
encrypted reasoning requests. A client must omit options this runtime does not implement;
server-side continuation is not a substitute for a requested encrypted output field.
