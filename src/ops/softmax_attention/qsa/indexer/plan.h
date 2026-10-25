#pragma once

#include "core/layout.h"
#include "ops/softmax_attention/qsa/indexer/arguments.h"
#include "ninfer/ops/qsa.h"

#include <algorithm>

namespace ninfer::ops::detail::qsa {

inline constexpr int kIndexerWindowQueries = 64;
inline constexpr int kIndexerPartition     = 4096;
static_assert(kIndexerWindowQueries <= kIndexerMaxWindowQueries);

struct IndexerPlan {
    int window_tokens, columns, partitions;
};

inline IndexerPlan select_indexer_plan(int tokens, int batch, std::uint32_t visible_tokens) {
    const int blocks = visible_tokens / 4;
    return {std::min(tokens, kIndexerWindowQueries / batch),
            (blocks + kIndexerKeyTile - 1) / kIndexerKeyTile * kIndexerKeyTile,
            (blocks + kIndexerPartition - 1) / kIndexerPartition};
}

struct IndexerWindowPlan {
    int query_tile, score_workers, select_workers;
};

inline IndexerWindowPlan select_indexer_window(IndexerPlan plan, int width, int batch, int sms) {
    const int tile         = width <= 4 ? 4 : width <= 8 ? 8 : width <= 16 ? 16 : 32;
    const int tasks        = batch * ((width + tile - 1) / tile) * (plan.columns / kIndexerKeyTile);
    const int score_waves  = width <= 2 || width > 32 ? 4 : 2;
    const int select_waves = width > 32 ? 4 : 2;
    return {tile, std::max(1, std::min(score_waves * sms, tasks)),
            std::max(1, std::min(select_waves * sms, width * batch * plan.partitions))};
}

struct IndexerWorkspace {
    Tensor scores, candidates;
};

template <class Allocator>
IndexerWorkspace allocate_indexer_workspace(Allocator& allocator, IndexerPlan plan, int batch) {
    IndexerWorkspace out;
    if (plan.columns <= kIndexerSelected) return out;
    const int rows = plan.window_tokens * batch;
    out.scores     = allocator.alloc(DType::FP32, {plan.columns, rows}, 256);
    out.candidates = allocator.alloc(DType::I64, {kIndexerSelected, plan.partitions, rows}, 256);
    return out;
}

void launch_indexer(const Tensor& query, const QsaIndexCacheView& cache, const QsaBatchView& batch,
                    IndexerPlan plan, const IndexerWorkspace& scratch, QsaBlockSelection& out,
                    CudaExecutionView execution);

} // namespace ninfer::ops::detail::qsa
