#include "ops/softmax_attention/qsa/indexer/plan.h"
#include "ops/common/host/launch.h"
#include "ops/common/host/operands.h"
#include "ops/softmax_attention/qsa/indexer/score.cuh"
#include "ops/softmax_attention/qsa/indexer/select.cuh"

namespace ninfer::ops::detail::qsa {
namespace {
template <int QueryTile, bool VectorCache>
void launch_score(IndexerArguments a, IndexerWindowPlan plan, cudaStream_t stream) {
    constexpr int threads = QueryTile > 16 ? QueryTile * 8 : 128;
    constexpr int bytes   = (QueryTile * 4 + kIndexerKeyTile) * 128 * sizeof(__nv_bfloat16);
    constexpr auto kernel = indexer_score_mma_kernel<QueryTile, VectorCache>;
    const int shared      = common::prepare_shared<bytes, kernel, true>();
    kernel<<<plan.score_workers, threads, shared, stream>>>(a);
}

template <bool VectorCache>
void launch_score(IndexerArguments a, IndexerWindowPlan plan, cudaStream_t stream) {
    if (plan.query_tile == 4)
        launch_score<4, VectorCache>(a, plan, stream);
    else if (plan.query_tile == 8)
        launch_score<8, VectorCache>(a, plan, stream);
    else if (plan.query_tile == 16)
        launch_score<16, VectorCache>(a, plan, stream);
    else
        launch_score<32, VectorCache>(a, plan, stream);
}
} // namespace

void launch_indexer(const Tensor& query, const QsaIndexCacheView& cache, const QsaBatchView& batch,
                    IndexerPlan plan, const IndexerWorkspace& scratch, QsaBlockSelection& out,
                    CudaExecutionView execution) {
    const auto i32 = [](const Tensor& t) { return static_cast<const std::int32_t*>(t.data); };
    IndexerArguments a{static_cast<const __nv_bfloat16*>(query.data),
                       static_cast<const __nv_bfloat16*>(cache.keys.data),
                       i32(batch.positions),
                       i32(batch.valid_columns),
                       i32(batch.table_rows),
                       i32(cache.block_tables),
                       cache.keys.nb[1] / 2,
                       cache.keys.nb[3] / 2,
                       cache.block_tables.nb[1] / 4,
                       static_cast<float*>(scratch.scores.data),
                       static_cast<std::uint64_t*>(scratch.candidates.data),
                       static_cast<std::int32_t*>(out.block_ids.data),
                       static_cast<std::int32_t*>(out.block_counts.data),
                       query.ne[2],
                       query.ne[3],
                       0,
                       plan.window_tokens,
                       plan.columns,
                       plan.partitions};
    const bool vector_cache = common::operand::aligned(cache.keys.data, 16) &&
                              cache.keys.nb[1] % 16 == 0 && cache.keys.nb[3] % 16 == 0;
    for (std::int64_t begin = 0; begin < a.tokens; begin += plan.window_tokens) {
        a.begin = int(begin);
        a.width = std::min(plan.window_tokens, a.tokens - a.begin);
        const auto window =
            select_indexer_window(plan, a.width, a.batch, execution.multiprocessor_count());
        // Every capture has the same three stages per window, including all-select and inert
        // replays. Actual per-row context affects only Device work maps and masked reads.
        if (vector_cache)
            launch_score<true>(a, window, execution.stream());
        else
            launch_score<false>(a, window, execution.stream());
        indexer_partition_kernel<kIndexerPartition>
            <<<window.select_workers, 256, 0, execution.stream()>>>(a);
        indexer_finish_kernel<kIndexerPartition>
            <<<a.width * a.batch, 256, 0, execution.stream()>>>(a);
    }
    CUDA_CHECK(cudaGetLastError());
}
} // namespace ninfer::ops::detail::qsa
