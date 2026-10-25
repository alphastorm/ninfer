#pragma once

#include "core/layout.h"
#include "ops/softmax_attention/qsa/sparse_gqa/arguments.h"
#include "ops/common/host/operands.h"
#include "ninfer/ops/qsa.h"

#include <algorithm>

namespace ninfer::ops::detail::qsa {

inline constexpr int kSparseGqaDirectQueries = 256;
enum class SparseGqaInstance { Mma, Simt, FullPrefix };

struct SparseGqaPlan {
    int key_rows, splits;
    SparseGqaInstance instance;
    bool partial;
};

inline SparseGqaPlan select_sparse_gqa_plan(int tokens, int batch,
                                            QsaSparseGqaExecutionEnvelope envelope, int sms) {
    const auto queries = std::int64_t(tokens) * batch;
    const auto visible = envelope.max_visible_tokens;
    if (queries >= kSparseGqaDirectQueries)
        return visible <= 2051 ? SparseGqaPlan{64, 1, SparseGqaInstance::FullPrefix, false}
                               : SparseGqaPlan{32, 1, SparseGqaInstance::Mma, false};
    // Budget parallel partitions independently of history. The envelope only bounds the
    // selected support and admits a short-context arithmetic schedule.
    const auto target =
        std::max<std::int64_t>(1, (std::int64_t(sms) * 3 / 4 + queries - 1) / queries);
    int splits = 1;
    while (splits < target && splits < 32) splits *= 2;
    const bool simt = visible <= 64;
    if (simt) splits = std::max(1, splits / 12);
    const int tiles = std::max(1, (int(std::min(visible, 2051u)) + 31) / 32);
    return {32, std::min(splits, tiles), simt ? SparseGqaInstance::Simt : SparseGqaInstance::Mma,
            true};
}

struct SparseGqaLaunchPlan {
    SparseGqaLoad load;
    bool vector_cache;
};

inline SparseGqaLaunchPlan select_sparse_gqa_launch(const PagedKVBatchLayerView& cache,
                                                    SparseGqaPlan plan, int queries, int sms) {
    const auto aligned = [](const Tensor& t) {
        return common::operand::aligned(t.data, 16) && t.nb[1] % 16 == 0 && t.nb[2] % 16 == 0 &&
               t.nb[3] % 16 == 0;
    };
    const bool vector = aligned(cache.k_pages) && aligned(cache.v_pages);
    if (cache.storage == KvCacheStorage::BFloat16 || !vector ||
        plan.instance == SparseGqaInstance::Simt)
        return {SparseGqaLoad::Direct, vector};
    if (plan.instance == SparseGqaInstance::FullPrefix) return {SparseGqaLoad::InPlace, vector};
    // In-place expansion adds barriers but retains three CTAs/SM. Choose it only when
    // that residency removes a wave; otherwise use a separate packed staging slot.
    const auto blocks  = std::int64_t(queries) * 2 * plan.splits;
    const bool inplace = (blocks + 3 * sms - 1) / (3 * sms) < (blocks + 2 * sms - 1) / (2 * sms);
    return {inplace ? SparseGqaLoad::InPlace : SparseGqaLoad::Staged, vector};
}

struct SparseGqaWorkspace {
    Tensor numerator, maximum, sum;
};

template <class Allocator>
SparseGqaWorkspace allocate_sparse_gqa_workspace(Allocator& allocator, int tokens, int batch,
                                                 SparseGqaPlan plan) {
    if (!plan.partial) return {};
    return {allocator.alloc(DType::FP32, {256, 24, tokens, batch * plan.splits}, 256),
            allocator.alloc(DType::FP32, {24, tokens, batch * plan.splits}, 256),
            allocator.alloc(DType::FP32, {24, tokens, batch * plan.splits}, 256)};
}

void launch_sparse_gqa(const Tensor& query, const Tensor& gate, const PagedKVBatchLayerView& cache,
                       const QsaBatchView& batch, const QsaBlockSelection& selected,
                       SparseGqaPlan plan, const SparseGqaWorkspace& scratch, Tensor& out,
                       CudaExecutionView execution);

} // namespace ninfer::ops::detail::qsa
