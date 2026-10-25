#include "core/layout.h"
#include "models/qwen3_5/state/state_image.h"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

namespace q36 = ninfer::models::qwen3_5;

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (condition) { return; }
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

q36::StateImageDeviceLayout plan(bool dflash) {
    q36::StateImageSpec spec{
        .slot_count = 2,
        .linear =
            {
                .layers         = 2,
                .conv_channels  = 5,
                .conv_width     = 3,
                .value_heads    = 2,
                .value_head_dim = 4,
                .key_head_dim   = 3,
                .conv_dtype     = ninfer::DType::BF16,
            },
        .hidden = 7,
    };
    if (dflash) {
        spec.dflash_local =
            q36::DFlashLocalStateSpec{.layers = 2, .capacity = 17, .kv_heads = 2, .head_dim = 4};
    }
    ninfer::LayoutBuilder builder;
    return q36::plan_state_image_device_pool(builder, spec);
}

} // namespace

int main() {
    const auto common      = plan(false);
    const auto common_work = common.images.transfer_work();
    expect(common_work.payload_bytes == common.images.image_stride &&
               common_work.copy_operations == 1,
           "complete image work includes actual alignment bytes in one copy");
    const auto dflash      = plan(true);
    const auto full_work   = dflash.images.transfer_work();
    const auto local_bytes = dflash.dflash_local->group.bytes;
    expect(local_bytes == 2ULL * 2 * 4 * 128 * 2 * 2,
           "local group includes both K/V for every layer");
    expect(full_work.payload_bytes == common_work.payload_bytes + local_bytes &&
               full_work.copy_operations == 1,
           "full image copy includes common and local fields in one contiguous range");
    expect(dflash.images.storage.bytes == 2 * full_work.payload_bytes,
           "Device capacity uses full image stride");
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
