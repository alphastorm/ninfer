# NInfer native Windows lanes

One runtime for the RTX 4090 (`sm_89`) and RTX 3090 (`sm_86`) lanes, built from mainline
`port/native-lanes-on-mainline` rather than the two divergent lane branches. The package carries
the same context cache, warm arrival across a restart, and streamed verified-or-refused restore
that ship on the RTX 5090 container lane, with the Windows platform code (D3D12 residency arena,
DirectStorage read queue) and the MSVC build of the host tree.

Each lane installs into its own isolated state root through `Install-Release.ps1`, runs one
authenticated request at a time on a loopback or Tailscale address under a scheduled task owned by
SYSTEM, and keeps durable session checkpoints under its per-release cache. The lane's exact
engine configuration is bound by hash into the build identity; the lane specification names the
GPU, architecture, power policy, and task identity the installer enforces.

The lane configuration also sizes the pinned host pools (`context_cache.host_state_slots`,
`context_cache.host_kv_mib`) for the host that runs the lane, because `cudaMallocHost` on
Windows must find the pages at startup right after the controller has hashed the 18 GB model
through the file cache: on the 32 GiB RTX 4090 host 24 state slots plus the 8 GiB default KV
pool (13.3 GB pinned) failed that allocation on two of four starts, so that lane ships 8 slots
and 4 GiB (about 6.6 GB); the 64 GiB RTX 3090 host keeps 24 slots and 8 GiB.
