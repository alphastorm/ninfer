#pragma once

#include "ninfer/ops/qsa.h"
#include "ops/op_tester.h"
#include "ops/softmax_attention/oracle.h"
#include "sparse_gqa_cache_fixture.h"

#include <bit>
#include <memory>
#include <numeric>

namespace ninfer::test::qsa::sparse {

inline std::uint32_t hash(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    return x ^ (x >> 16);
}

inline float sample(std::uint32_t i) { return (int(hash(i) % 10001) - 5000) / 3000.0F; }

inline std::uint16_t half(float x) { return std::bit_cast<std::uint16_t>(_Float16(x)); }

inline float unhalf(std::uint16_t x) { return float(std::bit_cast<_Float16>(x)); }

struct Plane {
    int leading;
    std::int64_t row, head, page;
    std::vector<std::uint16_t> host;
    GuardedDeviceBuffer device;
    Tensor view;

    Plane(int pages, DType dtype, int padding)
        : leading(padding ? 1 : 0), row(256 + padding), head(64 * row + padding),
          page(2 * head + padding), host(leading + page * pages), device(host.size() * 2),
          view(device.span().subspan(leading * 2), dtype, {256, 64, 2, pages}) {
        view.nb[1] = row * 2;
        view.nb[2] = head * 2;
        view.nb[3] = page * 2;
    }

    std::size_t offset(int d, int h, int r, int p) const {
        return leading + d + h * head + r * row + p * page;
    }
};

struct Fixture {
    int T, B, history, table_width, table_stride, pages;
    int live_limit = std::numeric_limits<int>::max();
    std::vector<std::uint16_t> qhost, ghost;
    std::vector<int> positions, valid, selectors, tables, ids, counts;
    std::vector<std::vector<int>> visible;
    Plane keys, values;
    KvCacheStorage storage;
    std::unique_ptr<EncodedVector> encoded_key, encoded_value;
    GuardedDeviceBuffer q, gate, out, pos, lengths, rows, table, selections, sizes;
    Tensor query, output_gate, output;
    PagedKVBatchLayerView cache;
    ops::QsaBatchView batch;
    ops::QsaBlockSelection selected;
    ops::QsaSparseGqaExecutionEnvelope envelope;

    Fixture(int tokens, int batch_size, int max_history, bool padded,
            KvCacheStorage format = KvCacheStorage::BFloat16, bool padded_scales = false)
        : T(tokens), B(batch_size), history(max_history), table_width((history + 63) / 64),
          table_stride(table_width + (padded ? 3 : 0)), pages(std::min(table_width, 521) * B + 2),
          qhost(std::size_t(T) * B * 6144), ghost(qhost.size()), positions(T * B), valid(B),
          selectors(B), tables(table_stride * (B + 1)), ids(std::size_t(T) * B * 512),
          counts(T * B), visible(T * B), keys(pages, DType::BF16, padded ? 1 : 0),
          values(pages, DType::FP16, padded ? 3 : 0), storage(format), q(qhost.size() * 2),
          gate(ghost.size() * 2), out(qhost.size() * 2), pos(positions.size() * 4), lengths(B * 4),
          rows(B * 4), table(tables.size() * 4), selections(ids.size() * 4),
          sizes(counts.size() * 4) {
        envelope.max_visible_tokens = history;
        query                       = Tensor(q.span(), DType::BF16, {256, 24, T, B});
        output_gate                 = Tensor(gate.span(), DType::BF16, {256, 24, T, B});
        output                      = Tensor(out.span(), DType::BF16, {256, 24, T, B});
        Tensor bt(table.span(), DType::I32, {table_width, B + 1});
        bt.nb[1] = table_stride * 4;
        cache    = {keys.view, values.view, {}, {}, bt, 256, 2, KvCacheStorage::BFloat16};
        if (storage != KvCacheStorage::BFloat16) {
            const auto formats = prepare::cache_encoding(storage);
            encoded_key =
                std::make_unique<EncodedVector>(formats.key, pages, padded, padded_scales);
            encoded_value =
                std::make_unique<EncodedVector>(formats.value, pages, padded, padded_scales);
            cache.k_pages       = encoded_key->data.view;
            cache.v_pages       = encoded_value->data.view;
            cache.k_scale_pages = encoded_key->scales.view;
            cache.v_scale_pages = encoded_value->scales.view;
            cache.storage       = storage;
        }
        batch    = {Tensor(pos.span(), DType::I32, {T, B}), Tensor(lengths.span(), DType::I32, {B}),
                    Tensor(rows.span(), DType::I32, {B})};
        selected = {Tensor(selections.span(), DType::I32, {512, T, B}),
                    Tensor(sizes.span(), DType::I32, {T, B})};
    }

    void reset(int phase = 0, int cap = 512, float magnitude = 1.0F) {
        std::fill(qhost.begin(), qhost.end(), 0x7fc1);
        std::fill(ghost.begin(), ghost.end(), 0x7fc1);
        std::fill(keys.host.begin(), keys.host.end(), 0x7fc1);
        std::fill(values.host.begin(), values.host.end(), 0x7e01);
        std::fill(positions.begin(), positions.end(), -1);
        std::fill(ids.begin(), ids.end(), -1);
        std::fill(counts.begin(), counts.end(), -1);
        for (auto& row : visible) row.clear();
        for (int b = 0; b < B; ++b) {
            int end = history, live = std::min(T, end);
            if (phase == 1 || B > 1) {
                switch ((b + phase) % 4) {
                case 1:
                    end  = std::min(history, 2051);
                    live = std::min(T, end);
                    break;
                case 2:
                    end  = std::min(history, 3);
                    live = std::min(T, end);
                    break;
                case 3:
                    live = 0;
                    break;
                }
            }
            live = std::min(live, live_limit);
            if (phase == 2) live = 0;
            valid[b]     = live;
            selectors[b] = live ? (b + phase) % B + 1 : -1;
            if (!live) continue;
            for (int p = 0; p < table_width; ++p)
                tables[selectors[b] * table_stride + p] =
                    (pages - 1 - (b * table_width + p) % pages + phase * 7) % pages;
            for (int t = 0; t < live; ++t) {
                const int m = t + T * b, p = end - live + t, C = (p + 1) / 4;
                positions[m] = p;
                counts[m]    = std::min(C, cap);
                int step     = C > 0 ? (hash(m + phase * 71) % C) | 1 : 1;
                while (C > 0 && std::gcd(step, C) != 1) step += 2;
                for (int i = 0; i < counts[m]; ++i) {
                    const int g = (std::int64_t(i) * step + hash(m + 31 + phase)) % C;
                    ids[std::size_t(m) * 512 + i] = g;
                    for (int j = 0; j < 4; ++j) visible[m].push_back(g * 4 + j);
                }
                for (int j = C * 4; j <= p; ++j) visible[m].push_back(j);
                std::sort(visible[m].begin(), visible[m].end());
                if (visible[m].empty()) continue;
                for (int i = 0; i < 6144; ++i) {
                    const auto index = std::size_t(m) * 6144 + i;
                    qhost[index]     = f32_to_bf16(sample(index + 711 + phase) * magnitude);
                    ghost[index]     = f32_to_bf16(sample(index + 129 + phase) * 4);
                }
                for (int j : visible[m]) {
                    const int page = tables[selectors[b] * table_stride + j / 64];
                    for (int h = 0; h < 2; ++h)
                        for (int d = 0; d < 256; ++d) {
                            const auto seed =
                                std::uint32_t((page * 128 + h * 64 + j % 64) * 256 + d);
                            keys.host[keys.offset(d, h, j % 64, page)] =
                                f32_to_bf16(sample(seed + 701) * magnitude);
                            values.host[values.offset(d, h, j % 64, page)] =
                                half(sample(seed + 1311));
                        }
                }
            }
        }
        upload();
    }

    void upload() {
        q.copy_from_host(qhost.data(), qhost.size() * 2);
        gate.copy_from_host(ghost.data(), ghost.size() * 2);
        keys.device.copy_from_host(keys.host.data(), keys.host.size() * 2);
        values.device.copy_from_host(values.host.data(), values.host.size() * 2);
        pos.copy_from_host(positions.data(), positions.size() * 4);
        lengths.copy_from_host(valid.data(), valid.size() * 4);
        rows.copy_from_host(selectors.data(), selectors.size() * 4);
        table.copy_from_host(tables.data(), tables.size() * 4);
        selections.copy_from_host(ids.data(), ids.size() * 4);
        sizes.copy_from_host(counts.data(), counts.size() * 4);
        if (encoded_key) {
            encoded_key->encode(keys);
            encoded_value->encode(values);
        }
        out.fill(0xa5);
        // Fixture transfers/memset use the default stream; the Op's stream is nonblocking.
        CUDA_CHECK(cudaStreamSynchronize(nullptr));
    }

    void launch(MemoryArena& arena, CudaExecutionView execution) {
        ops::qsa_sparse_gqa(query, output_gate, cache, batch, selected, envelope, arena, output,
                            execution);
    }

    std::vector<double> reference() const {
        std::vector<double> result(qhost.size());
        const auto kv = [&](const Plane& plane, int d, int h, int ordinal, int m) {
            const int token    = visible[m][ordinal];
            const int physical = tables[selectors[m / T] * table_stride + token / 64];
            if (encoded_key)
                return (&plane == &keys ? encoded_key : encoded_value)
                    ->decode(d, h, token % 64, physical);
            const auto word = plane.host[plane.offset(d, h, token % 64, physical)];
            return plane.view.dtype == DType::BF16 ? double(bf16_to_f32(word))
                                                   : double(unhalf(word));
        };
        const auto run = [&](int first, int last) {
            for (int m = first; m < last; ++m) {
                if (visible[m].empty()) continue;
                std::array<double, 6144> transformed_query;
                for (int h = 0; h < 24; ++h) {
                    std::array<double, 256> row;
                    for (int d = 0; d < 256; ++d)
                        row[d] = bf16_to_f32(qhost[(std::size_t(m) * 24 + h) * 256 + d]);
                    if (encoded_key) prepare::hadamard(row);
                    std::copy(row.begin(), row.end(), transformed_query.begin() + h * 256);
                }
                naive_dense_softmax_attention(
                    {256, 24, 2}, 1, int(visible[m].size()), 1.0 / 16,
                    [&](int d, int h, int) { return transformed_query[h * 256 + d]; },
                    [&](int d, int h, int j) { return kv(keys, d, h, j, m); },
                    [&](int d, int h, int j) { return kv(values, d, h, j, m); },
                    [](int, int) { return true; },
                    [&](int d, int h, int, double x) {
                        const auto i = (std::size_t(m) * 24 + h) * 256 + d;
                        result[i]    = x;
                    },
                    T * B == 1 ? 16 : 1);
                for (int h = 0; h < 24; ++h) {
                    const auto base = (std::size_t(m) * 24 + h) * 256;
                    std::array<double, 256> row;
                    std::copy_n(result.begin() + base, 256, row.begin());
                    if (encoded_value && encoded_value->format.rotate) prepare::hadamard(row);
                    for (int d = 0; d < 256; ++d)
                        result[base + d] =
                            row[d] / (1.0 + std::exp(-double(bf16_to_f32(ghost[base + d]))));
                }
            }
        };
        const int workers = std::min(16, T * B);
        {
            std::vector<std::jthread> threads;
            for (int i = 1; i < workers; ++i)
                threads.emplace_back(run, T * B * i / workers, T * B * (i + 1) / workers);
            run(0, T * B / workers);
        }
        return result;
    }
};
} // namespace ninfer::test::qsa::sparse
