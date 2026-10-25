#include "core/cyclic_kv_cache.h"

#include <limits>
#include <stdexcept>

namespace ninfer {

Device CyclicKVCacheLayerView::device() const {
    if (k.device != v.device) { throw std::invalid_argument("cyclic KV spans different devices"); }
    return k.device;
}

CyclicKVCacheLayout plan_cyclic_kv_cache(StateImageBuilder& builder, std::uint32_t layers,
                                         std::uint32_t capacity, std::int32_t num_kv_heads,
                                         std::int32_t head_dim) {
    const std::uint64_t padded = (static_cast<std::uint64_t>(capacity) + 127U) & ~127ULL;
    if (layers == 0 || capacity == 0 || padded > INT32_MAX || num_kv_heads <= 0 || head_dim <= 0) {
        throw std::invalid_argument("Cyclic KV geometry is invalid");
    }
    const auto k =
        builder.add(DType::BF16, {head_dim, static_cast<std::int32_t>(padded), num_kv_heads},
                    layers, "Cyclic K");
    const auto v =
        builder.add(DType::FP16, {head_dim, static_cast<std::int32_t>(padded), num_kv_heads},
                    layers, "Cyclic V");
    return {capacity,
            static_cast<std::uint32_t>(padded),
            num_kv_heads,
            head_dim,
            layers,
            k,
            v,
            builder.group(k, v)};
}

CyclicKVCacheView::CyclicKVCacheView(const StateImageStorage& storage,
                                     const CyclicKVCacheLayout& layout)
    : layer0_{storage.field(layout.k), storage.field(layout.v), layout.capacity,
              layout.padded_capacity,  layout.num_kv_heads,     layout.head_dim,
              storage.slot_count()},
      layers_(layout.layers), k_layer_stride_(storage.layer_stride(layout.k)),
      v_layer_stride_(storage.layer_stride(layout.v)) {
    if (layers_ == 0 || layout.capacity == 0 || layout.padded_capacity < layout.capacity ||
        layout.head_dim <= 0 || layout.num_kv_heads <= 0 ||
        storage.layout().fields.at(layout.k.index).layers != layers_ ||
        storage.layout().fields.at(layout.v.index).layers != layers_) {
        throw std::invalid_argument("Cyclic KV field geometry is inconsistent");
    }
    for (const Tensor* tensor : {&layer0_.k, &layer0_.v}) {
        if (tensor->ne[0] != layout.head_dim || tensor->ne[1] != layout.padded_capacity ||
            tensor->ne[2] != layout.num_kv_heads || tensor->ne[3] != storage.slot_count()) {
            throw std::invalid_argument("Cyclic KV field shape is inconsistent");
        }
    }
    if (layer0_.k.dtype != DType::BF16 || layer0_.v.dtype != DType::FP16) {
        throw std::invalid_argument("Cyclic KV requires BF16 K and FP16 V");
    }
}

CyclicKVCacheLayerView CyclicKVCacheView::layer_view(std::uint32_t layer) const {
    if (layer >= layers_) { throw std::out_of_range("Cyclic KV layer is out of range"); }
    auto result   = layer0_;
    result.k.data = static_cast<std::byte*>(result.k.data) + layer * k_layer_stride_;
    result.v.data = static_cast<std::byte*>(result.v.data) + layer * v_layer_stride_;
    return result;
}

} // namespace ninfer
