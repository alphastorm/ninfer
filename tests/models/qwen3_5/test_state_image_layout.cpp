#include "core/layout.h"
#include "models/qwen3_5/state/state_image.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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

nlohmann::json read_json(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) { throw std::runtime_error("cannot open " + path.string()); }
    return nlohmann::json::parse(input);
}

void test_packaged_native_state_images() {
    const std::filesystem::path source = NINFER_SOURCE_DIR;
    // Configs extracted from the pinned artifact's v3 directory, without tensor payloads.
    const auto fixture = read_json(source / "tests/fixtures/qwen3_8_27b_config.json");
    const auto& components = fixture.at("components");
    const auto& text = components.at("text").at("config");
    const auto dimension = [](const auto& config, const char* name) {
        return config.at(name).template get<std::int32_t>();
    };
    // Schema-4 RTX 5090 is experimental, records a historical budget and is not
    // accepted by the native schema-5 packager.
    for (const auto* lane : {"rtx3090", "rtx4090"}) {
        const auto lane_dir = source / "packaging/windows/lanes" / lane;
        const auto release = read_json(lane_dir / "release-spec.json");
        const auto server  = read_json(lane_dir / "server-config.json");
        const std::string label = std::string(lane) + ": ";
        expect(release.at("model") == fixture.at("model"),
               label + "layout fixture matches the packaged artifact identity");
        const auto backend = server.at("speculative").at("backend").get<std::string>();
        expect(release.at("runtime_shape").at("speculative_backend") == backend,
               label + "specification and server select the same backend");
        if (backend != "mtp" && backend != "dflash2") {
            throw std::runtime_error(label + "unsupported packaged speculative backend");
        }
        // Same field inventory as program/planning/startup.cpp::persistent_layout.
        // The draft-token window sizes replay records separately, not the native
        // image: masked drafts retain the entire model-configured sliding window.
        q36::StateImageSpec spec{
            .slot_count = server.at("engine").at("max_concurrency").get<std::int32_t>() +
                          server.at("context_cache").at("device_state_slots").get<std::int32_t>(),
            .linear =
                {
                    .layers = static_cast<std::uint32_t>(
                        std::count(text.at("layer_types").begin(), text.at("layer_types").end(),
                                   "linear_attention")),
                    .conv_channels = 2 * dimension(text, "linear_num_key_heads") *
                                         dimension(text, "linear_key_head_dim") +
                                     dimension(text, "linear_num_value_heads") *
                                         dimension(text, "linear_value_head_dim"),
                    .conv_width     = dimension(text, "linear_conv_kernel_dim") - 1,
                    .value_heads    = dimension(text, "linear_num_value_heads"),
                    .value_head_dim = dimension(text, "linear_value_head_dim"),
                    .key_head_dim   = dimension(text, "linear_key_head_dim"),
                    .conv_dtype     = ninfer::DType::BF16,
                },
            .hidden = dimension(text, "hidden_size"),
        };
        if (backend == "dflash2") {
            const auto& draft = components.at(backend).at("config");
            spec.dflash_local = q36::DFlashLocalStateSpec{
                .layers = static_cast<std::uint32_t>(
                    std::count(draft.at("layer_types").begin(), draft.at("layer_types").end(),
                               "sliding_attention")),
                .capacity = draft.at("sliding_window").get<std::uint32_t>(),
                .kv_heads = dimension(draft, "num_key_value_heads"),
                .head_dim = dimension(draft, "head_dim"),
            };
        }
        ninfer::LayoutBuilder builder;
        const auto layout = q36::plan_state_image_device_pool(builder, spec);
        const auto expected = release.at("host_context_translation")
                                  .at("native_state_image_bytes").get<std::uint64_t>();
        expect(layout.images.image_stride == expected,
               label + "computed native state image is " +
                   std::to_string(layout.images.image_stride) + " bytes, specification declares " +
                   std::to_string(expected));
    }
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
    try {
        test_packaged_native_state_images();
    } catch (const std::exception& error) {
        expect(false, error.what());
    }
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
