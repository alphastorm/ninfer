#include "models/qwen3_5/load/bindings.h"

#include <cuda_fp16.h>

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>

namespace ninfer::models::qwen3_5::loading {

void requantize_draft_mlp_rows(std::uint64_t columns, std::uint64_t rows,
                               std::span<const std::byte> source_codes,
                               std::span<const std::byte> source_scales,
                               std::span<std::byte> target_codes,
                               std::span<std::byte> target_scales) {
    const auto padded = artifact::align_up(columns, 128, "draft MLP row");
    const auto elements = artifact::checked_mul(rows, padded, "draft MLP tile");
    if (!columns || !rows || source_codes.size() != elements ||
        source_scales.size() != elements / 16 ||
        target_codes.size() != elements / 2 ||
        target_scales.size() != elements / 32 || std::fegetround() != FE_TONEAREST) {
        throw artifact::ArtifactError("draft MLP requantization requires complete rows and RNE");
    }
    std::fill(target_codes.begin(), target_codes.end(), std::byte{});
    std::fill(target_scales.begin(), target_scales.end(), std::byte{});
    for (std::uint64_t row = 0; row < rows; ++row) {
        for (std::uint64_t begin = 0; begin < columns; begin += 64) {
            std::array<float, 64> values{};
            const auto count = std::min<std::uint64_t>(64, columns - begin);
            float maximum = 0;
            for (std::uint64_t half = 0; half < count; half += 32) {
                const auto column = begin + half;
                const auto offset = row * padded / 16 + column / 32 * 2;
                const auto word = std::to_integer<unsigned>(source_scales[offset]) |
                                  (std::to_integer<unsigned>(source_scales[offset + 1]) << 8);
                if (word >= 0x7c00U) {
                    throw artifact::ArtifactError("draft MLP Q8 scale is not nonnegative finite");
                }
                const __half_raw scale{static_cast<unsigned short>(word)};
                const float multiplier = __half2float(__half(scale));
                for (std::uint64_t i = half; i < std::min(count, half + 32); ++i) {
                    const auto code =
                        std::to_integer<unsigned>(source_codes[row * padded + begin + i]);
                    if (code == 128U) {
                        throw artifact::ArtifactError("draft MLP Q8 code is outside [-127,127]");
                    }
                    const auto value = code < 128U ? int(code) : int(code) - 256;
                    if (!word && value) {
                        throw artifact::ArtifactError("draft MLP Q8 zero scale has nonzero code");
                    }
                    values[i] = float(value) * multiplier;
                    maximum = std::max(maximum, std::abs(values[i]));
                }
            }
            if (!maximum) continue;
            auto scale = static_cast<__half_raw>(__float2half_rn(maximum / 7.0F));
            if (!scale.x) scale.x = 1; // Canonical binary16 underflow rescue.
            if (scale.x >= 0x7c00U) {
                throw artifact::ArtifactError("draft MLP Q4 scale overflows binary16");
            }
            const float reciprocal = 1.0F / __half2float(__half(scale));
            const auto scale_offset = row * padded / 32 + begin / 64 * 2;
            target_scales[scale_offset] = std::byte(scale.x & 255U);
            target_scales[scale_offset + 1] = std::byte(scale.x >> 8);
            for (std::uint64_t i = 0; i < count; ++i) {
                const auto code = std::clamp(int(std::nearbyint(values[i] * reciprocal)), -8, 7);
                const auto column = begin + i;
                target_codes[row * padded / 2 + column / 2] |=
                    std::byte((unsigned(code) & 15U) << ((column & 1U) * 4));
            }
        }
    }
}

DraftWeights bind_draft(Bindings& b, const DraftConfig& config, const TextConfig& target,
                        const TextWeights& weights, const std::string& component) {
    const auto h = target.hidden_size;
    const auto q = config.attention.query_width();
    const auto k = config.attention.key_width();
    const auto d = config.attention.head_dim;
    DraftWeights out;
    out.feature_projection = b.parameter(
        component + "/feature_projection",
        {h, artifact::checked_mul(config.target_layer_ids.size(), h, "target features")},
        {component + "/target_features"});
    out.context_norm    = b.direct(component + "/context_norm", {h});
    out.final_norm      = b.direct(component + "/final_norm", {h});
    out.token_embedding = weights.token_embedding;
    out.output_head     = weights.output_head;
    out.layers.reserve(config.num_hidden_layers);
    for (std::uint32_t i = 0; i < config.num_hidden_layers; ++i) {
        const auto p = component + "/layers/" + std::to_string(i) + "/";
        DraftBlockWeights layer;
        layer.input_norm          = b.direct(p + "input_norm", {h});
        layer.post_attention_norm = b.direct(p + "post_attention_norm", {h});
        layer.attention.query =
            b.parameter(p + "attention/query", {q, h}, {p + "query_projection_input"});
        layer.attention.key =
            b.parameter(p + "attention/key", {k, h}, {p + "query_projection_input"});
        layer.attention.value =
            b.parameter(p + "attention/value", {k, h}, {p + "query_projection_input"});
        layer.attention.context_key =
            b.parameter(p + "attention/context_key", {k, h}, {component + "/context_input"});
        layer.attention.context_value =
            b.parameter(p + "attention/context_value", {k, h}, {component + "/context_input"});
        layer.attention.query_norm = b.direct(p + "attention/query_norm", {d});
        layer.attention.key_norm   = b.direct(p + "attention/key_norm", {d});
        layer.attention.output =
            b.parameter(p + "attention/output", {h, q}, {p + "attention_output"});
        layer.mlp = bind_dense(b, h, config.intermediate_size, p, true);
        out.layers.push_back(std::move(layer));
    }
    if (config.dflash2) { bind_dflash2(b, out, config, target); }
    return out;
}

} // namespace ninfer::models::qwen3_5::loading
