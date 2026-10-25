#pragma once

#include "core/state_image.h"

namespace ninfer {

struct LinearAttentionStateSpec {
    std::uint32_t layers        = 0;
    std::int32_t conv_channels  = 0;
    std::int32_t conv_width     = 0;
    std::int32_t value_heads    = 0;
    std::int32_t value_head_dim = 0;
    std::int32_t key_head_dim   = 0;
    DType conv_dtype            = DType::BF16;
};

struct LinearAttentionStateLayout {
    LinearAttentionStateSpec spec;
    StateField conv;
    StateField recurrent;
};

struct LinearAttentionStateAllLayersView {
    Tensor conv_layer0;
    Tensor recurrent_layer0;
    std::int64_t conv_layer_stride_bytes      = 0;
    std::int64_t recurrent_layer_stride_bytes = 0;
    LinearAttentionStateSpec spec;
};

struct LinearAttentionStateLayerView {
    Tensor conv;
    Tensor recurrent;
};

[[nodiscard]] LinearAttentionStateLayout
plan_linear_attention_state(StateImageBuilder& builder, const LinearAttentionStateSpec& spec);

// Typed consumers of StateImage fields; slot ownership and transfers belong to the image store.
class LinearAttentionStateView {
public:
    LinearAttentionStateView(const StateImageStorage& storage,
                             const LinearAttentionStateLayout& layout);

    [[nodiscard]] const LinearAttentionStateSpec& spec() const noexcept { return view_.spec; }

    [[nodiscard]] std::uint32_t layer_count() const noexcept { return view_.spec.layers; }

    [[nodiscard]] std::int32_t slot_count() const noexcept { return view_.conv_layer0.ne[2]; }

    [[nodiscard]] LinearAttentionStateLayerView layer_view(std::uint32_t layer) const;

    [[nodiscard]] LinearAttentionStateAllLayersView all_layers_view() const { return view_; }

    [[nodiscard]] Tensor conv_slot(std::uint32_t layer, std::int32_t slot) const;
    [[nodiscard]] Tensor recurrent_slot(std::uint32_t layer, std::int32_t slot) const;

private:
    LinearAttentionStateAllLayersView view_;
};

} // namespace ninfer
