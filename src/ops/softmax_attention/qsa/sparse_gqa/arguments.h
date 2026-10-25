#pragma once

#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cstdint>

namespace ninfer::ops::detail::qsa {

enum class SparseGqaLoad { Direct, Staged, InPlace };

struct SparseGqaPage {
    const std::uint8_t* data;
    std::int64_t row, head, page; // Byte strides for both data and scale planes.
};

struct SparseGqaWork {
    int count, complete, size, tiles, splits;
};

struct SparseGqaArguments {
    const __nv_bfloat16 *query, *gate;
    __nv_bfloat16* out;
    SparseGqaPage key, value, key_scale, value_scale;
    const int *positions, *valid, *table_rows, *tables, *ids, *counts;
    std::int64_t table_stride;
    float *numerator, *maximum, *sum;
    int tokens, batch, splits, key_rows;
    std::int64_t begin;

    __device__ SparseGqaWork work(std::int64_t query_id) const {
        const int b = int(query_id / tokens), t = int(query_id % tokens);
        if (t >= valid[b]) return {};
        const int p = positions[query_id], c = counts[query_id];
        const int n     = c * 4 + (p + 1) % 4;
        const int tiles = (n + key_rows - 1) / key_rows;
        return {c, (p + 1) / 4, n, tiles, tiles < splits ? tiles : splits};
    }

    __device__ std::int64_t partial_row(std::int64_t q, int split, int h) const {
        return ((std::int64_t(split) * batch * tokens + q) * 24 + h);
    }
};
} // namespace ninfer::ops::detail::qsa
