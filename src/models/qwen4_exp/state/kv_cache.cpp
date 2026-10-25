#include "models/qwen4_exp/state/kv_cache.h"

namespace ninfer::models::qwen4_exp {
QsaCacheLayout plan_qsa_cache(LayoutBuilder& builder, const QsaConfig& config, std::uint32_t layers,
                              std::uint32_t max_context, std::uint32_t pages,
                              std::int32_t table_rows, KvCacheStorage storage) {
    const auto head_dim = dimension(config.head_dim), kv_heads = dimension(config.kv_heads);
    const auto schema = paged_kv_storage_layout(storage, head_dim);
    KVPageGeometry geometry;
    for (std::uint32_t i = 0; i < layers; ++i) {
        geometry.planes.push_back(
            {schema.key.data_dtype, schema.key.data_leading_extent, kv_heads, 256, 1});
        geometry.planes.push_back(
            {schema.value.data_dtype, schema.value.data_leading_extent, kv_heads, 256, 1});
        if (schema.key.has_scale())
            geometry.planes.push_back(
                {schema.key.scale_dtype, schema.key.scale_leading_extent, kv_heads, 256, 1});
        if (schema.value.has_scale())
            geometry.planes.push_back(
                {schema.value.scale_dtype, schema.value.scale_leading_extent, kv_heads, 256, 1});
        geometry.planes.push_back({DType::BF16, dimension(config.index_dim),
                                   dimension(config.index_kv_heads), 256, config.compression});
    }
    return {plan_device_kv_page_pool(builder, {pages, std::move(geometry)}),
            plan_kv_execution_tables(builder, {(max_context + 63) / 64, table_rows}),
            schema,
            layers,
            head_dim,
            kv_heads};
}

QsaCache::QsaCache(MemorySpan backing, const QsaCacheLayout& layout)
    : layout_(layout), pages_(backing, layout.pages), tables_(backing, layout.tables, pages_) {}

ops::QsaCacheView QsaCache::layer(std::uint32_t layer) const {
    if (layer >= layout_.layers) throw std::out_of_range("QSA cache layer");
    const auto& s     = layout_.storage;
    const auto base   = layer * (s.planes_per_layer() + 1);
    const auto tables = tables_.matrix();
    return {{pages_.plane(base), pages_.plane(base + 1),
             s.key.has_scale() ? pages_.plane(base + 2) : Tensor{},
             s.value.has_scale() ? pages_.plane(base + 2 + s.key.has_scale()) : Tensor{}, tables,
             layout_.head_dim, layout_.kv_heads, s.storage},
            {pages_.plane(base + s.planes_per_layer()), tables}};
}
} // namespace ninfer::models::qwen4_exp
