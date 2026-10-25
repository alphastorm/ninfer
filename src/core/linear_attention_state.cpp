#include "core/linear_attention_state.h"

#include <stdexcept>

namespace ninfer {

LinearAttentionStateLayout plan_linear_attention_state(StateImageBuilder& builder,
                                                       const LinearAttentionStateSpec& spec) {
    if (spec.layers == 0 || spec.conv_channels <= 0 || spec.conv_width <= 0 ||
        spec.value_heads <= 0 || spec.value_head_dim <= 0 || spec.key_head_dim <= 0 ||
        (spec.conv_dtype != DType::BF16 && spec.conv_dtype != DType::FP32)) {
        throw std::invalid_argument("Linear Attention state geometry is invalid");
    }
    return {
        spec,
        builder.add(spec.conv_dtype, {spec.conv_channels, spec.conv_width}, spec.layers,
                    "Linear Attention convolution"),
        builder.add(DType::FP32, {spec.key_head_dim, spec.value_head_dim, spec.value_heads},
                    spec.layers, "Linear Attention recurrence"),
    };
}

LinearAttentionStateView::LinearAttentionStateView(const StateImageStorage& storage,
                                                   const LinearAttentionStateLayout& layout)
    : view_{storage.field(layout.conv), storage.field(layout.recurrent),
            storage.layer_stride(layout.conv), storage.layer_stride(layout.recurrent),
            layout.spec} {
    const auto& conv      = view_.conv_layer0;
    const auto& recurrent = view_.recurrent_layer0;
    const auto& spec      = layout.spec;
    if (conv.dtype != spec.conv_dtype || conv.ne[0] != spec.conv_channels ||
        conv.ne[1] != spec.conv_width || conv.ne[3] != 1 || recurrent.dtype != DType::FP32 ||
        recurrent.ne[0] != spec.key_head_dim || recurrent.ne[1] != spec.value_head_dim ||
        recurrent.ne[2] != spec.value_heads || recurrent.ne[3] != conv.ne[2] ||
        storage.layout().fields.at(layout.conv.index).layers != spec.layers ||
        storage.layout().fields.at(layout.recurrent.index).layers != spec.layers) {
        throw std::invalid_argument("Linear Attention state fields do not match their geometry");
    }
}

LinearAttentionStateLayerView LinearAttentionStateView::layer_view(std::uint32_t layer) const {
    if (layer >= layer_count()) { throw std::out_of_range("Linear Attention layer out of range"); }
    Tensor conv      = view_.conv_layer0;
    Tensor recurrent = view_.recurrent_layer0;
    conv.data        = static_cast<std::byte*>(conv.data) + layer * view_.conv_layer_stride_bytes;
    recurrent.data =
        static_cast<std::byte*>(recurrent.data) + layer * view_.recurrent_layer_stride_bytes;
    return {conv, recurrent};
}

Tensor LinearAttentionStateView::conv_slot(std::uint32_t layer, std::int32_t slot) const {
    return layer_view(layer).conv.slice(2, slot, 1).view({spec().conv_channels, spec().conv_width});
}

Tensor LinearAttentionStateView::recurrent_slot(std::uint32_t layer, std::int32_t slot) const {
    return layer_view(layer)
        .recurrent.slice(3, slot, 1)
        .view({spec().key_head_dim, spec().value_head_dim, spec().value_heads});
}

} // namespace ninfer
