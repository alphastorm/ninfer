#include "models/qwen3_5/program/storage/checkpoint_codec.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace ninfer::runtime;
namespace codec = ninfer::models::qwen3_5::detail::checkpoint_codec;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
DurableCheckpointSet fixture() {
    DurableCheckpointSet set;
    set.architecture = "qwen3_5";
    set.native_codec = "qwen3_5-checkpoint-set/1";
    set.state_layout_sha256 = std::string(64, 'a');
    set.endpoint = 2;
    DurableIdentity identity;
    identity.ledger.resize(65, 198);
    identity.token_types.resize(65, 0);
    for (auto& axis : identity.positions) for (int i = 0; i < 65; ++i) axis.push_back(i);
    identity.execution_frontiers = {32, 48};
    identity.vision_items.push_back({.modality = "video", .temporal = 1, .height = 1, .width = 1,
        .patch_count = 1, .content_sha256 = std::string(64, 'b'),
        .timestamps_bits = {"3ff0000000000000"}, .token_spans = {{8, 4}}});
    set.identities.push_back(std::move(identity));
    set.payloads.push_back({"engine/kv/0.bin", 512, std::string(64, 'c'), 256});
    DurablePool pool;
    pool.layout = {.pool_id = 0, .name = "text", .layout_sha256 = std::string(64, 'd'),
        .storage_type = "bf16", .layers = 1, .tokens_per_page = 64, .kv_heads = 1,
        .head_dim = 1, .page_bytes = 256, .host_serialization_stride = 256};
    pool.pages = {{0, 0, 256, 64}, {0, 256, 256, 1}};
    set.pools.push_back(std::move(pool));
    set.histories.push_back({0, {0, 1}, 65});
    const std::array frontiers{32U, 48U, 65U};
    const std::array roles{DurablePointRole::InputReplay, DurablePointRole::LongAnchor,
                           DurablePointRole::Continuation};
    for (std::uint32_t i = 0; i < 3; ++i) {
        set.payloads.push_back({"engine/state/" + std::to_string(i) + ".bin", 256, std::string(64, 'e'), 256});
        set.states.push_back({set.state_layout_sha256, i + 1, 0, 256});
        set.points.push_back({.role = roles[i], .frontier = frontiers[i], .text_kv_frontier = frontiers[i],
            .rope_delta = -3, .tail_hidden_valid = true, .dflash_context_frontier = frontiers[i],
            .state = i, .histories = {{0, frontiers[i]}}});
    }
    return set;
}
void rejects(const std::function<void(DurableCheckpointSet&)>& mutate) {
    auto set = fixture(); mutate(set);
    bool rejected = false;
    try { codec::validate(set, {}); } catch (const std::exception&) { rejected = true; }
    require(rejected, "invalid checkpoint set accepted");
}
void rejects_encoded(std::span<const std::uint8_t> bytes) {
    bool rejected = false;
    try { (void)codec::decode(std::as_bytes(bytes), {}); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid checkpoint CBOR was not classified as invalid_argument");
}
void rejects_cbor(const nlohmann::json& document) {
    rejects_encoded(nlohmann::json::to_cbor(document));
}
} // namespace
int main() {
    try {
        const auto set = fixture();
        codec::validate(set, {});
        const auto bytes = codec::encode(set);
        const auto decoded = codec::decode(std::as_bytes(std::span(bytes)), {});
        require(codec::encode(decoded) == bytes && decoded.histories.size() == 1 && decoded.points.size() == 3 &&
                decoded.points[2].dflash_context_frontier == 65 && decoded.points[0].rope_delta == -3 &&
                decoded.identities[0].vision_items[0].timestamps_bits[0] == "3ff0000000000000",
                "multi-point identity/ring frontier round-trip mismatch");
        rejects([](auto& x) { x.points[0].frontier = 66; });
        rejects([](auto& x) { x.endpoint = 0; });
        rejects([](auto& x) { x.points[0].role = DurablePointRole::Continuation; });
        rejects([](auto& x) { x.histories[0].pages[1] = 0; });
        rejects([](auto& x) { x.pools[0].pages[1].offset = UINT64_MAX; });
        rejects([](auto& x) { x.pools[0].pages[1].offset = 0; });
        rejects([](auto& x) { x.histories[0].committed_tokens = 66; });
        rejects([](auto& x) { x.identities[0].positions[2].pop_back(); });
        rejects([](auto& x) { x.identities[0].execution_frontiers = {48, 32}; });
        rejects([](auto& x) { x.identities[0].vision_items[0].timestamps_bits[0] = "7ff0000000000000"; });
        rejects([](auto& x) { x.identities[0].vision_items[0].token_spans[0] = {64, 2}; });
        rejects([](auto& x) { x.states.push_back(x.states[0]); });
        rejects([](auto& x) { x.payloads[0].path = "../engine/kv/0.bin"; });
        rejects([](auto& x) { x.payloads[0].alignment = 3; });
        rejects([](auto& x) { x.payloads[0].sha256[0] = 'A'; });
        rejects_encoded({});
        rejects_encoded(std::array<std::uint8_t, 1>{0xff});
        rejects_encoded(std::span(bytes).first(bytes.size() - 1));
        auto document = nlohmann::json::from_cbor(bytes);
        document.erase("format"); rejects_cbor(document);
        document = nlohmann::json::from_cbor(bytes);
        document.erase("points"); rejects_cbor(document);
        document = nlohmann::json::from_cbor(bytes);
        document["points"] = false; rejects_cbor(document);
        document = nlohmann::json::from_cbor(bytes);
        document["unknown"] = true; rejects_cbor(document);
        document = nlohmann::json::from_cbor(bytes);
        document["points"][0]["frontier"] = UINT64_MAX; rejects_cbor(document);
        document = nlohmann::json::from_cbor(bytes);
        document["points"][0]["frontier"] = 32.0; rejects_cbor(document);
        document = nlohmann::json::from_cbor(bytes);
        document["points"][0]["origin"] = "future_origin"; rejects_cbor(document);
        bool bounded = false;
        try { (void)codec::decode(std::as_bytes(std::span(bytes)), {.manifest_bytes = bytes.size() - 1}); }
        catch (const std::exception&) { bounded = true; }
        require(bounded, "index decode ignored metadata bound");
        std::cout << "OK checkpoint canonical codec, multi-point sharing and malformed bounds\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL checkpoint codec: " << error.what() << '\n'; return 1;
    }
}
