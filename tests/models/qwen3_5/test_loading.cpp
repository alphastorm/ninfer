#include "artifact/binder.h"
#include "artifact/fixture.h"
#include "artifact/formats.h"
#include "models/qwen3_5/load.h"
#include "models/qwen3_5/load/bindings.h"
#include "ops/quantized_weight.h"

#include <algorithm>
#include <array>
#include <iostream>

namespace {

using namespace ninfer;
using namespace ninfer::models;
namespace q35 = ninfer::models::qwen3_5;
using namespace ninfer::test::artifact_fixture;

Json added_token(int id, const std::string& content) {
    return {{"id", id},        {"content", content}, {"special", true},    {"single_word", false},
            {"lstrip", false}, {"rstrip", false},    {"normalized", false}};
}

struct ModelFixture {
    Fixture file;

    void tensor(const std::string& id, std::vector<std::uint64_t> shape, QType format = QType::BF16,
                QuantLayout layout = QuantLayout::Contiguous) {
        const auto geometry = weight_geometry(format, layout, shape);
        const auto offset   = (file.payload.size() + 255) / 256 * 256;
        file.payload.resize(offset + geometry.bytes);
        if (format == QType::NVFP4) {
            put_word(file.payload, offset + geometry.divisor_offset, 0x3f800000, 4);
        }
        file.root["objects"].push_back({{"id", id},
                                        {"kind", "tensor"},
                                        {"shape", shape},
                                        {"format", artifact::format_name(format)},
                                        {"layout", artifact::layout_name(layout)},
                                        {"offset", offset},
                                        {"bytes", geometry.bytes}});
    }

    void resource(const std::string& role, const std::string& bytes) {
        const auto offset = file.payload.size();
        for (const auto byte : bytes) { file.payload.push_back(std::byte(byte)); }
        file.root["objects"].push_back({{"id", role},
                                        {"kind", "resource"},
                                        {"encoding", "raw_bytes_v1"},
                                        {"offset", offset},
                                        {"bytes", bytes.size()}});
        file.root["components"]["text"]["resources"][role] = role;
    }

    void use(const std::string& name, const std::string& input, std::optional<int> divisor = {}) {
        Json value{{"parameter", name}, {"input", input}, {"activation_policy", "AllowA4"}};
        if (divisor) {
            value["auxiliaries"] = {
                {"activation_input_divisor",
                 {{"parts", Json::array({{{"object", "calibration"},
                                          {"range", {*divisor, *divisor + 1}}}})}}}};
        }
        file.root["uses"].push_back(value);
    }

    void parameter(const std::string& name, std::vector<std::uint64_t> shape,
                   const std::string& input = {}) {
        tensor(name, shape);
        file.root["bindings"][name] = {{"object", name}};
        if (!input.empty()) { use(name, input); }
    }

    ModelFixture() {
        file.payload.clear();
        file.root = {{"components",
                      {{"text",
                        {{"config",
                          {{"architectures", {"Qwen3_5ForCausalLM"}},
                           {"model_type", "qwen3_5_text"},
                           {"hidden_size", 128},
                           {"vocab_size", 272},
                           {"num_hidden_layers", 1},
                           {"max_position_embeddings", 128},
                           {"tie_word_embeddings", true},
                           {"rms_norm_eps", 1e-6},
                           {"layer_types", {"full_attention"}},
                           {"num_attention_heads", 2},
                           {"num_key_value_heads", 1},
                           {"head_dim", 8},
                           {"intermediate_size", 24},
                           {"rope_parameters",
                            {{"rope_theta", 10000},
                             {"partial_rotary_factor", 0.5},
                             {"mrope_section", {1, 1, 0}}}}}}}}}},
                     {"objects", Json::array()},
                     {"bindings", Json::object()},
                     {"uses", Json::array()},
                     {"metadata", {{"name", "a-user-trained-model"}}}};
        tensor("embedding-storage", {272, 128});
        file.root["bindings"]["text/token_embedding"] = {{"object", "embedding-storage"}};
        file.root["bindings"]["text/output_head"]     = {{"object", "embedding-storage"}};
        use("text/output_head", "text/final_hidden");
        parameter("text/final_norm", {128});
        const std::string p = "text/layers/0/";
        parameter(p + "input_norm", {128});
        parameter(p + "post_attention_norm", {128});
        tensor("mixer-storage", {128, 128}, QType::NVFP4, QuantLayout::BlockScaleK16M128x4);
        tensor("calibration", {2}, QType::FP32);
        const auto calibration = file.root["objects"].back()["offset"].get<std::size_t>();
        put_word(file.payload, calibration, 0x40000000, 4);
        put_word(file.payload, calibration + 4, 0x40400000, 4);
        const std::array roles = {"query", "key", "gate", "value"};
        const std::array rows  = {0, 16, 24, 40, 48};
        for (std::size_t i = 0; i < roles.size(); ++i) {
            const auto name             = p + "attention/" + roles[i];
            file.root["bindings"][name] = {
                {"parts", Json::array({{{"object", "mixer-storage"},
                                        {"range", {rows[i] * 128, rows[i + 1] * 128}}}})}};
            use(name, p + "mixer_input", i < 2 ? 0 : 1);
        }
        parameter(p + "attention/query_norm", {8});
        parameter(p + "attention/key_norm", {8});
        parameter(p + "attention/output", {128, 16}, p + "attention/gated_output");
        parameter(p + "mlp/gate", {24, 128}, p + "ffn_input");
        parameter(p + "mlp/up", {24, 128}, p + "ffn_input");
        tensor("down-storage", {128, 24}, QType::Q6_G64_FP16, QuantLayout::RowSplit);
        file.root["bindings"][p + "mlp/down"] = {{"object", "down-storage"}};
        use(p + "mlp/down", p + "mlp/product");

        Json vocab    = Json::object();
        unsigned next = 256;
        for (unsigned byte = 0; byte < 256; ++byte) {
            const bool visible =
                (byte >= 33 && byte <= 126) || (byte >= 161 && byte <= 172) || byte >= 174;
            const unsigned cp = visible ? byte : next++;
            std::string token;
            if (cp < 128) {
                token.push_back(static_cast<char>(cp));
            } else {
                token.push_back(static_cast<char>(0xc0 | (cp >> 6)));
                token.push_back(static_cast<char>(0x80 | (cp & 63)));
            }
            vocab[token] = byte;
        }
        Json added                = Json::array();
        Json decoder              = Json::object();
        const std::array specials = {"<|endoftext|>",  "<|im_start|>",  "<|im_end|>",
                                     "<think>",        "</think>",      "<|vision_start|>",
                                     "<|vision_end|>", "<|image_pad|>", "<|video_pad|>"};
        for (std::size_t i = 0; i < specials.size(); ++i) {
            const auto token = added_token(static_cast<int>(256 + i), specials[i]);
            if (i < 8) { added.push_back(token); }
            decoder[std::to_string(256 + i)] = token;
        }
        resource("tokenizer.json",
                 Json{{"model", {{"type", "BPE"}, {"vocab", vocab}, {"merges", Json::array()}}},
                      {"added_tokens", added}}
                     .dump());
        resource("tokenizer_config.json", Json{{"added_tokens_decoder", decoder}}.dump());
        resource("generation_config.json", Json{{"eos_token_id", {256, 258}}}.dump());
        resource("chat_template.jinja", "a user-provided template, preserved as data");
        file.root["files"] =
            Json::array({{{"path", nullptr}, {"payload_bytes", file.payload.size()}}});
    }
};

void logical_data_and_instances() {
    ModelFixture fixture;
    fixture.file.root["components"]["vision"] = {{"config", Json::object()}, {"target", "text"}};
    fixture.file.root["components"]["text"]["config"]["rope_parameters"]["partial_rotary_factor"] =
        0.4999999999;
    fixture.file.write();
    const auto capacity = [&] {
        artifact::Reader reader(fixture.file.entry);
        auto plan = q35::plan_load(reader);
        require(plan.config().text.rope_parameters->partial_rotary_factor == 0.5F &&
                    plan.config().text.rope_parameters->rotary_dim == 4,
                "rotary width was derived before normalizing the PositiveF32 config value");
        require(plan.config().text.architecture == Architecture::Qwen3_5 &&
                    plan.config().text.hidden_size == 128,
                "binding chose a checkpoint-specific geometry");
        require(plan.resources().public_token_count == 265 &&
                    plan.resources().tokenizer->encode("<|video_pad|>") == std::vector<int>{264},
                "tokenizer_config added tokens were not included in the public domain");
        const auto& attention = std::get<q35::AttentionWeights>(plan.weights().text.layers[0].mixer);
        const auto& query     = plan.parameter(attention.query);
        const auto& gate      = plan.parameter(attention.gate);
        require(query.binding.parts[0].object == gate.binding.parts[0].object &&
                    query.binding.parts[0].begin == 0 && gate.binding.parts[0].begin == 24 * 128,
                "Q/gate logical row correspondence changed");
        require(plan.uses(attention.query)[0].activation_input_divisor == 2.0F &&
                    plan.uses(attention.gate)[0].activation_input_divisor == 3.0F,
                "shared-parent Uses contaminated one another");
        require(plan.parameter(plan.weights().text.token_embedding).binding.parts[0].object ==
                    plan.parameter(plan.weights().text.output_head).binding.parts[0].object,
                "explicit shared embedding/head did not bind the same parent");
        require(!plan.weights().vision && plan.resources().preprocessor_config_json.empty(),
                "unused Vision was required");
        rejects([&] { (void)q35::plan_load(reader, {.vision = true}); },
                "incomplete selected Vision was accepted");
        rejects([&] { (void)q35::plan_load(reader, {.speculative = SpeculativeBackend::Mtp}); },
                "absent MTP was accepted");

        const auto resolved = q35::resolve_load(std::move(plan), Device::cuda(0));
        return resolved.materialization().device_capacity_bytes;
    }();
    fixture.file.root["metadata"]["name"] = "another-training-run-and-recipe";
    fixture.file.root["components"]["text"]["config"]["num_hidden_layers"] = 2;
    fixture.file.root["components"]["text"]["config"]["layer_types"]       = {"full_attention",
                                                                              "full_attention"};
    const auto original_bindings = fixture.file.root["bindings"];
    for (const auto& [name, binding] : original_bindings.items()) {
        if (name.starts_with("text/layers/0/")) {
            auto second = name;
            second.replace(12, 1, "1");
            fixture.file.root["bindings"][second] = binding;
        }
    }
    const auto original_uses = fixture.file.root["uses"];
    for (auto use : original_uses) {
        auto parameter = use["parameter"].get<std::string>();
        if (parameter.starts_with("text/layers/0/")) {
            auto input = use["input"].get<std::string>();
            parameter.replace(12, 1, "1");
            input.replace(12, 1, "1");
            use["parameter"] = parameter;
            use["input"]     = input;
            fixture.file.root["uses"].push_back(use);
        }
    }
    fixture.file.write();
    artifact::Reader changed(fixture.file.entry);
    auto expanded = q35::plan_load(changed);
    require(expanded.weights().text.layers.size() == 2 &&
                expanded.config().text.compact_layer_indices == std::vector<std::uint32_t>{0, 1},
            "config did not drive layer geometry and compact indices");
    require(q35::resolve_load(std::move(expanded), Device::cuda(0))
                    .materialization()
                    .device_capacity_bytes == capacity,
            "cross-layer sharing duplicated parent storage");
}

void native_uses() {
    const std::array<std::uint64_t, 2> shape{128, 64};
    const auto geometry = weight_geometry(QType::NVFP4, QuantLayout::BlockScaleK16M128x4, shape);
    std::vector<std::byte> bytes(geometry.bytes);
    const std::array divisors{8.0F};
    const WeightParent parent{geometry, Device::cpu(), bytes.data(), divisors};
    const WeightView gate{{64, 64}, {{&parent, 0, 4096}}};
    const WeightView up{{64, 64}, {{&parent, 4096, 8192}}};
    const auto joined = ops::prepare_linear_swiglu_weight({gate, ops::LinearPolicy::AllowA4, 2.0F},
                                                          {up, ops::LinearPolicy::AllowA8, 2.0F});
    require(joined.policy == ops::LinearPolicy::AllowA8 && joined.weight.payload == bytes.data() &&
                joined.weight.n == 128 && joined.weight.weight_scale_divisor == 8.0F &&
                joined.weight.input_scale_divisor == 2.0F,
            "shared quantization did not intersect Uses or preserve the complete parent");
    const WeightView full{{128, 64}, {{&parent, 0, 8192}}};
    for (const auto policy : {ops::LinearPolicy::A16Only, ops::LinearPolicy::AllowA8}) {
        const auto no_aux = ops::prepare_linear_weight({full, policy});
        require(no_aux.policy == policy && no_aux.weight.payload == bytes.data(),
                "non-A4 NVFP4 use required an unused activation divisor");
        const auto mixed = ops::prepare_linear_swiglu_weight(
            {gate, ops::LinearPolicy::AllowA4, 2.0F}, {up, policy});
        require(mixed.policy == policy, "combined use required an unused child auxiliary");
        rejects<std::invalid_argument>(
            [&] { (void)ops::prepare_linear_weight({full, policy, 0.0F}); },
            "present activation divisor was accepted without value validation");
    }
    rejects<std::invalid_argument>(
        [&] { (void)ops::prepare_linear_weight({full, ops::LinearPolicy::AllowA4}); },
        "A4 NVFP4 use accepted a missing activation divisor");
    const auto a16 = ops::prepare_linear_swiglu_weight({gate, ops::LinearPolicy::A16Only, 2.0F},
                                                       {up, ops::LinearPolicy::AllowA4, 3.0F});
    require(a16.policy == ops::LinearPolicy::A16Only && a16.weight.input_scale_divisor == 2.0F,
            "A16-only use intersection rejected independent unused activation divisors");
    const auto second = ops::prepare_linear_weight({full, ops::LinearPolicy::A16Only, 3.0F});
    const auto first  = ops::prepare_linear_weight({full, ops::LinearPolicy::AllowA4, 2.0F});
    require(first.weight.input_scale_divisor == 2.0F && second.weight.input_scale_divisor == 3.0F &&
                first.policy == ops::LinearPolicy::AllowA4 &&
                second.policy == ops::LinearPolicy::A16Only && parent.weight_divisors[0] == 8.0F,
            "preparing one use modified another use or its shared parent");
    rejects<std::invalid_argument>(
        [&] {
            (void)ops::prepare_linear_swiglu_weight({gate, ops::LinearPolicy::AllowA4, 2.0F},
                                                    {up, ops::LinearPolicy::AllowA4, 3.0F});
        },
        "one native activation silently used differing divisors");
    rejects<std::invalid_argument>(
        [&] {
            (void)ops::prepare_linear_swiglu_weight({up, ops::LinearPolicy::A16Only, 2.0F},
                                                    {gate, ops::LinearPolicy::A16Only, 2.0F});
        },
        "reordered parent regions silently changed fused gate/up row order");
    const WeightView short_gate{{64, 64}, {{&parent, 0, 4032}}};
    const WeightView long_up{{64, 64}, {{&parent, 4032, 8192}}};
    rejects<std::invalid_argument>(
        [&] {
            (void)ops::prepare_linear_swiglu_weight({short_gate, ops::LinearPolicy::A16Only, 2.0F},
                                                    {long_up, ops::LinearPolicy::A16Only, 2.0F});
        },
        "combined coverage hid an incorrect gate/up boundary");
}

void invalid_model_data() {
    ModelFixture fixture;
    const Json original = fixture.file.root;
    const auto bad      = [&](auto mutate) {
        fixture.file.root = original;
        mutate(fixture.file.root);
        fixture.file.write();
        rejects(
            [&] {
                artifact::Reader reader(fixture.file.entry);
                (void)q35::plan_load(reader);
            },
            "invalid model data was accepted");
    };
    bad([](Json& root) { root["components"]["text"]["config"]["layer_types"] = Json::array(); });
    bad([](Json& root) { root["components"]["text"]["config"]["num_key_value_heads"] = 3; });
    bad([](Json& root) {
        root["components"]["text"]["config"]["rope_parameters"]["mrope_section"] = {0, 2, 0};
    });
    bad([](Json& root) {
        root["bindings"]["text/layers/0/attention/query"]["parts"][0]["range"] = {0, 8 * 128};
    });
    bad([](Json& root) { root["uses"][1].erase("activation_policy"); });
    bad([](Json& root) { root["components"]["text"]["config"]["vocab_size"] = 264; });
}

void cold_matrix_profile() {
    const auto geometry = weight_geometry(QType::NVFP4, QuantLayout::BatchedBlockScaleK16M128x4,
                                          std::array<std::uint64_t, 3>{2, 256, 128});
    const WeightParent parent{geometry};
    const WeightView matrix{{128, 128}, {{&parent, 256 * 128, 384 * 128}}};
    const std::array inputs{ops::WeightInput{matrix, ops::LinearPolicy::AllowA4, 2.0F}};
    const auto profile = ops::describe_linear_weight(inputs);
    require(profile.qtype == QType::NVFP4 && profile.n == 128 && profile.k == 128 &&
                profile.layout == QuantLayout::BlockScaleK16M128x4 &&
                profile.policy == ops::LinearPolicy::AllowA4,
            "cold matrix profile did not preserve the local NVFP4 matrix layout");
}

void native_prefill_policy() {
    using ops::LinearPolicy;
    using ops::NativePrefillPolicy;
    for (const auto format : {QType::Q4_G64_FP16, QType::Q5_G64_FP16, QType::Q8_G32_FP16}) {
        const auto geometry = weight_geometry(format, QuantLayout::RowSplit,
                                              std::array<std::uint64_t, 2>{128, 5120});
        std::vector<std::byte> bytes(geometry.bytes);
        const WeightParent parent{geometry, Device::cpu(), bytes.data()};
        const WeightView view{{128, 5120}, {{&parent, 0, geometry.elements}}};
        for (const auto stored : {LinearPolicy::A16Only, LinearPolicy::AllowA8,
                                  LinearPolicy::AllowA4}) {
            const ops::WeightInput input{view, stored, 2.0F};
            const auto original =
                ops::effective_weight_input(input, NativePrefillPolicy::ArtifactUse);
            const auto promoted =
                ops::effective_weight_input(input, NativePrefillPolicy::Group64A8);
            const auto expected = format != QType::Q8_G32_FP16 && stored == LinearPolicy::A16Only
                                      ? LinearPolicy::AllowA8
                                      : stored;
            require(original.policy == stored && promoted.policy == expected &&
                        promoted.activation_input_divisor == 2.0F &&
                        &promoted.weight == &view && input.policy == stored,
                    "native prefill changed stored Use, auxiliary, or ineligible policy");
            const std::array inputs{promoted};
            require(ops::describe_linear_weight(inputs).policy ==
                        ops::prepare_linear_weight(promoted).policy,
                    "native prefill planned and bound policies differ");
        }
        rejects<std::invalid_argument>(
            [&] {
                (void)ops::effective_weight_input({view},
                                                  static_cast<NativePrefillPolicy>(255));
            },
            "invalid native prefill profile was accepted");
    }
    const auto q4_geometry = weight_geometry(QType::Q4_G64_FP16, QuantLayout::RowSplit,
                                             std::array<std::uint64_t, 2>{128, 5120});
    const auto q5_geometry = weight_geometry(QType::Q5_G64_FP16, QuantLayout::RowSplit,
                                             std::array<std::uint64_t, 2>{128, 5120});
    std::vector<std::byte> q4_bytes(q4_geometry.bytes), q5_bytes(q5_geometry.bytes);
    const WeightParent q4{q4_geometry, Device::cpu(), q4_bytes.data()};
    const WeightParent q5{q5_geometry, Device::cpu(), q5_bytes.data()};
    const WeightView first{{128, 5120}, {{&q4, 0, q4_geometry.elements}}};
    const WeightView second{{128, 5120}, {{&q5, 0, q5_geometry.elements}}};
    for (const auto permission : {LinearPolicy::A16Only, LinearPolicy::AllowA8, LinearPolicy::AllowA4}) {
        const std::array<ops::WeightInput, 2> inputs{{{first, LinearPolicy::AllowA4},
                                                     {second, permission}}};
        const auto planned = ops::describe_projection_weights(inputs);
        const auto bound = ops::projection_profile(ops::prepare_projection_weights(inputs));
        require(planned == bound && planned.first.policy == permission &&
                    planned.second->policy == permission,
                "paired projection failed to intersect planned/bound effective Uses");
    }
}

void draft_gate_up_requantization() {
    namespace oracle = ninfer::test::quantized_weight;
    for (const auto columns : {1, 31, 32, 33, 63, 64, 65, 127, 128, 129, 5120}) {
        constexpr int rows = 5;
        std::vector<float> values(rows * columns);
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column) {
                const auto signed_code = (column * 73 + row * 19) % 255 - 127;
                values[row * columns + column] = row == 0 ? 0.0F
                    : row == 1 ? std::ldexp(float(signed_code), -24)
                    : row == 2 ? float(signed_code) * 0.5F
                    : row == 3 ? float(signed_code) * 0.0078125F
                               : std::sin(float(column)) * 13.0F;
            }
        }
        const auto source = oracle::pack_q8_g32_row_split(values, rows, columns);
        const auto expected = oracle::pack_q4_row_split(source.dequant, rows, columns);
        const auto bytes = std::as_bytes(std::span(source.payload));
        std::vector<std::byte> codes(expected.code_plane_bytes + 2, std::byte{0xa5});
        std::vector<std::byte> scales(expected.scale_plane_bytes + 2, std::byte{0x5a});
        q35::loading::requantize_draft_mlp_rows(
            columns, rows, bytes.first(source.code_plane_bytes),
            bytes.subspan(source.scale_plane_offset, source.scale_plane_bytes),
            std::span(codes).subspan(1, expected.code_plane_bytes),
            std::span(scales).subspan(1, expected.scale_plane_bytes));
        const auto reference = std::as_bytes(std::span(expected.payload));
        require(std::equal(codes.begin() + 1, codes.end() - 1, reference.begin()) &&
                    std::equal(scales.begin() + 1, scales.end() - 1,
                               reference.begin() + expected.scale_plane_offset),
                "draft Q8->Q4 differs from independent grouped_absmax oracle");
        require(codes.front() == std::byte{0xa5} && codes.back() == std::byte{0xa5} &&
                    scales.front() == std::byte{0x5a} && scales.back() == std::byte{0x5a},
                "draft conversion changed guards");
    }
    std::vector<std::byte> codes(128), scales(8), target(64), target_scales(4);
    const auto convert = [&] {
        q35::loading::requantize_draft_mlp_rows(128, 1, codes, scales, target, target_scales);
    };
    codes[0] = std::byte{0x80};
    rejects(convert, "draft conversion accepted forbidden Q8 -128");
    codes[0] = std::byte{1};
    rejects(convert, "draft conversion accepted nonzero code with zero scale");
    scales[1] = std::byte{0x80};
    rejects(convert, "draft conversion accepted negative-zero scale");
    scales[1] = std::byte{0x7c};
    rejects(convert, "draft conversion accepted infinite scale");
    scales[0] = std::byte{0xff};
    scales[1] = std::byte{0x7b};
    codes[0] = std::byte{127};
    rejects(convert, "draft conversion silently clipped an overflowing Q4 scale");
}

void native_draft_plan() {
    ModelFixture fixture;
    fixture.file.root["components"]["dflash2"] = {
        {"target", "text"},
        {"config", {{"architectures", {"DFlash2DraftModel"}}, {"model_type", "qwen3"},
                    {"intermediate_size", 24}, {"num_attention_heads", 2},
                    {"num_key_value_heads", 1}, {"head_dim", 8}, {"num_hidden_layers", 1},
                    {"max_position_embeddings", 128}, {"rms_norm_eps", 1e-6},
                    {"rope_parameters", {{"rope_theta", 10000}}},
                    {"layer_types", {"full_attention"}},
                    {"dflash_config", {{"target_layer_ids", {0}}, {"mask_token_id", 256},
                                       {"conv_kernel_size", 3}, {"conv_group_size", 32},
                                       {"selector_rank", 8}, {"selector_top_k", 16}}}}}};
    fixture.parameter("dflash2/feature_projection", {128, 128}, "dflash2/target_features");
    fixture.parameter("dflash2/context_norm", {128});
    fixture.parameter("dflash2/final_norm", {128});
    fixture.use("text/output_head", "dflash2/final_hidden");
    const std::string p = "dflash2/layers/0/";
    fixture.parameter(p + "input_norm", {128});
    fixture.parameter(p + "post_attention_norm", {128});
    fixture.parameter(p + "attention/query", {16, 128}, p + "query_projection_input");
    fixture.parameter(p + "attention/key", {8, 128}, p + "query_projection_input");
    fixture.parameter(p + "attention/value", {8, 128}, p + "query_projection_input");
    fixture.parameter(p + "attention/context_key", {8, 128}, "dflash2/context_input");
    fixture.parameter(p + "attention/context_value", {8, 128}, "dflash2/context_input");
    fixture.parameter(p + "attention/query_norm", {8});
    fixture.parameter(p + "attention/key_norm", {8});
    fixture.parameter(p + "attention/output", {128, 16}, p + "attention_output");
    fixture.tensor("draft-gate-up", {48, 128}, QType::Q8_G32_FP16, QuantLayout::RowSplit);
    for (int i = 0; i < 2; ++i) {
        const auto name = p + "mlp/" + (i ? "up" : "gate");
        fixture.file.root["bindings"][name] = {
            {"parts", Json::array({{{"object", "draft-gate-up"},
                                    {"range", {i * 24 * 128, (i + 1) * 24 * 128}}}})}};
        fixture.use(name, p + "mlp_input");
        fixture.file.root["uses"].back()["activation_policy"] = "A16Only";
    }
    fixture.tensor("draft-down", {128, 24}, QType::Q8_G32_FP16, QuantLayout::RowSplit);
    fixture.file.root["bindings"][p + "mlp/down"] = {{"object", "draft-down"}};
    fixture.use(p + "mlp/down", p + "mlp_product");
    for (const std::string name : {"attention_conv", "mlp_conv"}) {
        fixture.parameter(p + name + "/base_kernel", {2, 3, 128});
        fixture.parameter(p + name + "/kernel_projection", {24, 128}, p + name + "_input");
    }
    fixture.parameter("dflash2/candidate_selector/hidden_projection", {8, 128},
                       "dflash2/final_hidden");
    fixture.parameter("dflash2/candidate_selector/predecessor_codebook", {272, 8});
    fixture.parameter("dflash2/candidate_selector/successor_codebook", {272, 8});
    fixture.file.root["files"][0]["payload_bytes"] = fixture.file.payload.size();
    fixture.file.write();
    artifact::Reader reader(fixture.file.entry);
    auto plan = q35::plan_load(reader, {.speculative = SpeculativeBackend::DFlash2,
                                       .native_prefill_policy = ops::NativePrefillPolicy::Group64A8});
    const auto& mlp = plan.weights().draft->layers[0].mlp;
    const auto gate = plan.input(mlp.gate), up = plan.input(mlp.up);
    const auto& parent = *gate.weight.parts.front().parent;
    require(&parent == up.weight.parts.front().parent &&
                gate.weight.parts.front().end == up.weight.parts.front().begin,
            "draft effective plan lost shared gate/up parent or logical ranges");
    require(plan.input(mlp.down).weight.parts.front().parent->geometry.format == QType::Q8_G32_FP16,
            "draft fit changed the Q8-only down projection");
    const auto storage = plan.placement(Device::cuda(0), Device::cuda(0));
#if defined(NINFER_SM86) || defined(NINFER_SM89)
    require(parent.geometry.format == QType::Q4_G64_FP16 &&
                gate.policy == ops::LinearPolicy::AllowA8 &&
                plan.converted_device_bytes() == parent.geometry.bytes,
            "effective Q4 geometry did not precede policy or capacity planning");
    const auto gate_up = reader.find("draft-gate-up");
    for (const auto& item : storage.device_objects)
        require(item.object != gate_up, "original Q8 draft parent was still reserved on Device");
    require(storage.registered_host_objects.size() == 1 &&
                storage.registered_host_objects.front().object == reader.find("embedding-storage"),
            "mapped embedding did not reserve pinned Host backing");
#else
    require(parent.geometry.format == QType::Q8_G32_FP16 &&
                gate.policy == ops::LinearPolicy::A16Only && !plan.converted_device_bytes() &&
                storage.registered_host_objects.empty(),
            "native fit changed a non-native build");
#endif
    require(plan.device_capacity_bytes(Device::cuda(0), Device::cuda(0)) ==
                storage.device_capacity_bytes + plan.converted_device_bytes(),
            "startup omitted effective draft backing");
}

} // namespace

int main() {
    try {
        logical_data_and_instances();
        native_uses();
        cold_matrix_profile();
        native_prefill_policy();
        draft_gate_up_requantization();
        native_draft_plan();
        invalid_model_data();
        std::cout << "model config, binding, resources and Use checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
