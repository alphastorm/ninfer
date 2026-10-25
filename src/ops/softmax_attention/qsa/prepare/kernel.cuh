#pragma once

#include "ops/softmax_attention/qsa/prepare/norm_rope.cuh"
#include "ops/softmax_attention/qsa/prepare/arguments.cuh"
#include "ops/softmax_attention/qsa/prepare/cache.cuh"

namespace ninfer::ops::detail::qsa {

__device__ __forceinline__ int coordinate(const PrepareArguments& a, int t, int b, int axis) {
    return a.rope[t + std::int64_t(a.tokens) * ((a.axes == 1 ? 0 : axis) + a.axes * b)];
}

template <bool BoundedPhase>
__device__ __forceinline__ void prepare_raw(const PrepareArguments& a, int t, int b, int lane,
                                            std::int64_t m) {
    const int record = a.offsets[b] + t;
#pragma unroll
    for (int j = 0; j < 4; ++j)
        *a.raw_records.at(lane + 32 * j, record, b) = a.ik[m * 128 + lane + 32 * j];
    if (lane < 3) *a.rope_records.at(lane, record, b) = coordinate(a, t, b, lane);
    const int p = a.positions[m];
    if (p % 4 != 3) return;
    const int base = a.positions[std::int64_t(b) * a.tokens], first = p - 3;
    const int slot = first < base ? a.slots[b] : 0;
    float pooled[4]{};
#pragma unroll
    for (int j = 0; j < 4; ++j) {
        const int token = first + j;
        const auto* raw = token < base ? a.tail.at(0, token - (base - base % 4), slot)
                                       : a.ik + (std::int64_t(b) * a.tokens + token - base) * 128;
#pragma unroll
        for (int c = 0; c < 4; ++c) pooled[c] += 0.25F * __bfloat162float(raw[lane + 32 * c]);
    }
#pragma unroll
    for (int c = 0; c < 4; ++c) pooled[c] = __bfloat162float(__float2bfloat16_rn(pooled[c]));
    const int axis = lane % 3;
    const int rho =
        first < base ? *a.first_rope.at(axis, slot) : coordinate(a, first - base, b, axis);
    const float2 sc = prepare_sincos<BoundedPhase>(a.frequency[lane], rho);
    norm_rotate<128>(pooled, a.wik, a.epsilon, sc, lane);
    const int page = a.tables[p / 64 + std::int64_t(a.table_rows[b]) * a.table_stride];
    auto* out      = a.index_keys.at((p / 4) % 16, 0, page);
#pragma unroll
    for (int j = 0; j < 4; ++j) out[lane + 32 * j] = __float2bfloat16_rn(pooled[j]);
}

// A CTA owns four heads/roles. Small calls interleave groups per token; larger calls visit
// one head group across tokens for memory throughput. Both use direct batch indexing and
// constant head-group arithmetic. Compression reads immutable projections/tail, never records.
// A carried block retains its first token's three axes even when current coordinates use A=1.
template <KvCacheStorage Storage, bool BoundedPhase, bool VectorWeights, bool TokenMajor>
__global__ __launch_bounds__(128) void prepare_kernel(const __grid_constant__ PrepareArguments a) {
    const int t = TokenMajor ? blockIdx.x / 8 : blockIdx.x;
    const int b = blockIdx.y, lane = threadIdx.x % 32;
    const int group = TokenMajor ? blockIdx.x % 8 : blockIdx.z;
    const int task  = group * 4 + threadIdx.x / 32;
    const auto m    = t + std::int64_t(b) * a.tokens;
    const bool live = t < a.valid[b];
    __shared__ float2 coefficients[32];
    if (live && threadIdx.x < 32)
        coefficients[lane] =
            prepare_sincos<BoundedPhase>(a.frequency[lane], coordinate(a, t, b, lane % 3));
    __syncthreads();
    if (task < 24) {
        const auto offset = (m * 24 + task) * 256;
        vector_head<256, VectorWeights>(a.q + offset, a.wq, a.out_q + offset, a.epsilon,
                                        coefficients, lane, live);
    } else if (task < 28) {
        const auto offset = (m * 4 + task - 24) * 128;
        vector_head<128, VectorWeights>(a.iq + offset, a.wiq, a.out_iq + offset, a.epsilon,
                                        coefficients, lane, live);
    } else if (live && task < 30) {
        if constexpr (Storage == KvCacheStorage::Nvfp4Group16 ||
                      Storage == KvCacheStorage::Fp8KeyNvfp4Value) {
            __shared__ float scratch[2][256];
            prepare_kv<Storage>(a, task - 28, t, b, lane, coefficients[lane], scratch[task - 28]);
        } else {
            prepare_kv<Storage>(a, task - 28, t, b, lane, coefficients[lane]);
        }
    } else if (live && task == 30) {
        prepare_raw<BoundedPhase>(a, t, b, lane, m);
    }
}

} // namespace ninfer::ops::detail::qsa
