#include "models/qwen3_5/state/state_image.h"

#include <stdexcept>

namespace ninfer::models::qwen3_5 {

StateImageDeviceLayout plan_state_image_device_pool(LayoutBuilder& backing,
                                                    const StateImageSpec& spec) {
    StateImageBuilder builder;
    StateImageDeviceLayout layout;
    layout.linear              = plan_linear_attention_state(builder, spec.linear);
    layout.continuation_hidden = builder.add(DType::BF16, {spec.hidden}, 1, "Continuation hidden");
    if (spec.dflash_local) {
        const auto& local   = *spec.dflash_local;
        layout.dflash_local = plan_cyclic_kv_cache(builder, local.layers, local.capacity,
                                                   local.kv_heads, local.head_dim);
    }
    layout.images = builder.finish(backing, spec.slot_count);
    return layout;
}

StateImageViews::StateImageViews(MemorySpan backing, const StateImageDeviceLayout& layout)
    : storage_(backing, layout.images), linear_(storage_, layout.linear),
      hidden_(storage_.field(layout.continuation_hidden)) {
    if (layout.dflash_local) {
        dflash_local_.emplace(storage_, *layout.dflash_local);
        dflash_group_ = layout.dflash_local->group;
    }
}

Tensor StateImageViews::continuation_hidden_slot(std::int32_t slot) const {
    return hidden_.slice(1, slot, 1).view({hidden_.ne[0]});
}

CyclicKVCacheView* StateImageViews::dflash_local() noexcept {
    return dflash_local_ ? &*dflash_local_ : nullptr;
}

const CyclicKVCacheView* StateImageViews::dflash_local() const noexcept {
    return dflash_local_ ? &*dflash_local_ : nullptr;
}

TransferWork StateImageViews::dflash_local_transfer_work() const {
    if (!dflash_group_) { throw std::logic_error("StateImage has no DFlash local state"); }
    return {.payload_bytes = dflash_group_->bytes, .copy_operations = 1};
}

} // namespace ninfer::models::qwen3_5
