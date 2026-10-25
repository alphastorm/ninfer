#include "models/qwen3_5/program/storage/draft_context.h"
#include <stdexcept>

namespace ninfer::models::qwen3_5::detail {

DFlashPersistentState::DFlashPersistentState(MemorySpan backing,
                                             const DFlashPersistentLayout& layout,
                                             CyclicKVCacheView& local_state)
    : local(local_state), prefill_features(layout.prefill_features.bind(backing)),
      prefill_positions(layout.prefill_positions.bind(backing)),
      pending_features(layout.pending_features.bind(backing)) {
    if (layout.full) { full.emplace(backing, *layout.full); }
}

CyclicKVCacheLayerView DFlashPersistentState::local_layer(std::uint32_t layer) const {
    return local.layer_view(layer);
}

PagedKVBatchLayerView DFlashPersistentState::full_batch_layer(std::uint32_t layer) const {
    if (!full) { throw std::logic_error("masked draft has no full KV layer"); }
    return full->batch_layer_view(layer);
}

} // namespace ninfer::models::qwen3_5::detail
