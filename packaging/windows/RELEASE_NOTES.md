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
`context_cache.host_kv_mib`) for the host that runs the lane. `cudaMallocHost` has to find the
pages at startup immediately after the controller has read the 18 GB model artifact through the
file cache, which leaves the free-and-zero list empty: on the 32 GiB RTX 4090 host the 8 GiB
default Host KV pool with 24 state slots (13.3 GB pinned) failed that allocation on two managed
starts, while 4 GiB with 24 slots (9.2 GB pinned) starts and serves the whole agent-protocol
contract. The 64 GiB RTX 3090 host keeps the 8 GiB pool. Host state slots are not a memory
dial to trade away: at 8 slots the protocol's post-delete continuation fails, so both lanes
carry 24.
