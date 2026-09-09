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
