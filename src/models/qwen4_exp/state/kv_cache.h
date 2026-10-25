#pragma once

#include "core/paged_kv_cache.h"
#include "ninfer/ops/qsa.h"
#include "models/qwen4_exp/config.h"

namespace ninfer::models::qwen4_exp {
struct QsaCacheLayout {
    DeviceKVPagePoolLayout pages;
    KVExecutionTableLayout tables;
    PagedKVStorageLayout storage;
    std::uint32_t layers;
    std::int32_t head_dim, kv_heads;
};

[[nodiscard]] QsaCacheLayout plan_qsa_cache(LayoutBuilder&, const QsaConfig&, std::uint32_t layers,
                                            std::uint32_t max_context, std::uint32_t pages,
                                            std::int32_t table_rows, KvCacheStorage);

class QsaCache {
public:
    QsaCache(MemorySpan, const QsaCacheLayout&);
    [[nodiscard]] ops::QsaCacheView layer(std::uint32_t) const;

    [[nodiscard]] DeviceKVPagePool& pages() { return pages_; }

    [[nodiscard]] const DeviceKVPagePool& pages() const { return pages_; }

    [[nodiscard]] KVExecutionTablePool& tables() { return tables_; }
private:
    QsaCacheLayout layout_;
    DeviceKVPagePool pages_;
    KVExecutionTablePool tables_;
};
} // namespace ninfer::models::qwen4_exp
