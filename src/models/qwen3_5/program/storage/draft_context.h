#pragma once
#include "models/qwen3_5/program/internal.h"

#include "core/cyclic_kv_cache.h"
#include "models/qwen3_5/program/planning/startup.h"

#include <cstdint>

namespace ninfer::models::qwen3_5::detail {

struct DFlashPersistentState {
    CyclicKVCacheView& local;
    std::optional<qwen3_5::PagedKVCache> full;
    Tensor prefill_features;
    Tensor prefill_positions;
    Tensor pending_features;

    DFlashPersistentState(MemorySpan backing, const DFlashPersistentLayout& layout,
                          CyclicKVCacheView& local_state);

    [[nodiscard]] CyclicKVCacheLayerView local_layer(std::uint32_t layer) const;
    [[nodiscard]] PagedKVBatchLayerView full_batch_layer(std::uint32_t layer) const;
};

} // namespace ninfer::models::qwen3_5::detail
