# Third-party source notices

## Native RK4V4 KV codec

The RK4V4 scalar codebook, packed key expansion, warp least-squares encoder, and
packed G32 value writer in `src/ops/kv_cache/kernels/rk4v4.cuh` and
`src/ops/kv_cache/kernels/write/rk4v4.cuh` are adapted from
[ashalliants/ninfer-3090](https://github.com/ashalliants/ninfer-3090), commit
`f5cec0a61adf23693144af4c0e8171ec95868141`:

- `src/ops/kv_cache/int8_g64_codec.cuh`
- `src/ops/kv_cache/append/kernel.cuh`

RK4V4 was introduced by Warlax in commit
[`0b4f95884b6d5407208f68c77112a09831270ce0`](https://github.com/ashalliants/ninfer-3090/commit/0b4f95884b6d5407208f68c77112a09831270ce0).
The G32 value grouping comes from
`2ba10da2acf3460697c6081834761715f134b548`.
These source files have no separate copyright header; their repository distributes
them under Apache License 2.0, retained in this repository's `LICENSE`.
Modifications adapt the codec to this fork's typed page layouts, shared INT8
attention templates, native Windows compilation, and versioned durable storage identity.

This is the Lloyd-Max RK4V4 codec, not `rk4v4-e8` or another E8 lattice codec.
No code was imported from iamwavecut/ninfer-all, TertiumOrganum1, or UDPSendToFailed
for this change. Other vendored dependencies retain their notices under `third_party/`.
