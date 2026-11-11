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

## Native group64 A8 prefill

The groupwise integer-activation quantizer, row-split Q4/Q5 MMA schedule, and
residual/SwiGLU epilogues in `src/ops/linear/kernels/group64_a8/mma.cuh`, their
launch wiring in `src/ops/linear/kernels/group64_a8/launch.cu`, and the paired
projection row mapping in
`src/ops/linear/split/q4_q5/shapes/n4096_n12288_k5120/shape.cpp` and
`src/ops/linear/split/q4_q5/shapes/n7168_n7168_k5120/shape.cpp` are adapted from
[ashalliants/ninfer-3090](https://github.com/ashalliants/ninfer-3090), commit
`4a178c4b1fd78555b5efd9b07d10d5b1946ca3ad`:

- `src/ops/common/rowsplit_a8_mma.cuh`
- `src/ops/linear_swiglu/q4a8/a8_linear_add.cu`
- `src/ops/linear_swiglu/q4a8/q4a8_linear_swiglu.cu`
- `src/ops/gdn_input_proj/q4_q5/q4_q5_gdn_input_a8.cu`
- `src/ops/attn_input_proj/q4_q5/q4_q5_attn_input_a8.cu`

These source files have no separate copyright header; their repository distributes
them under Apache License 2.0, retained in this repository's `LICENSE`.
Modifications adapt the schedule and launch wiring to this fork's caller-owned
workspace, typed group64 Use policy, FP32 activation scales, and padded
partial-column/output-tail contract.
