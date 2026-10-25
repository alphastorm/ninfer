#pragma once

#include "core/state_image.h"

namespace ninfer {

// Absolute position p resides at p % capacity. Frontiers are supplied by each consumer.
// Each slot's [head_dim, padded_capacity, num_kv_heads] is dense; slot stride is explicit.
struct CyclicKVCacheLayerView {
    [[nodiscard]] Device device() const;
    Tensor k;
    Tensor v;
    std::uint32_t capacity        = 0;
    std::uint32_t padded_capacity = 0;
    std::int32_t num_kv_heads     = 0;
    std::int32_t head_dim         = 0;
    std::int32_t slot_count       = 0;
};

struct CyclicKVCacheLayout {
    std::uint32_t capacity        = 0;
    std::uint32_t padded_capacity = 0;
    std::int32_t num_kv_heads     = 0;
    std::int32_t head_dim         = 0;
    std::uint32_t layers          = 0;
    StateField k;
    StateField v;
    LayoutRegion group;
};

[[nodiscard]] CyclicKVCacheLayout plan_cyclic_kv_cache(StateImageBuilder& builder,
                                                       std::uint32_t layers, std::uint32_t capacity,
                                                       std::int32_t num_kv_heads,
                                                       std::int32_t head_dim);

class CyclicKVCacheView {
public:
    CyclicKVCacheView(const StateImageStorage& storage, const CyclicKVCacheLayout& layout);

    [[nodiscard]] std::uint32_t layer_count() const noexcept { return layers_; }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return layer0_.capacity; }

    [[nodiscard]] std::uint32_t padded_capacity() const noexcept { return layer0_.padded_capacity; }

    [[nodiscard]] std::int32_t slot_count() const noexcept { return layer0_.slot_count; }

    [[nodiscard]] CyclicKVCacheLayerView layer_view(std::uint32_t layer) const;

private:
    CyclicKVCacheLayerView layer0_;
    std::uint32_t layers_;
    std::int64_t k_layer_stride_;
    std::int64_t v_layer_stride_;
};

} // namespace ninfer
