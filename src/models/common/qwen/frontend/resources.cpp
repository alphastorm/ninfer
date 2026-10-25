#include "models/common/qwen/frontend/resources.h"

#include "artifact/schema.h"

namespace ninfer::models::qwen {

void parse_resources(FrontendResources& resources, std::uint32_t embedding_rows,
                     std::optional<MediaGeometry> vision) {
    resources.tokenizer = std::make_shared<const frontend::Tokenizer>(
        frontend::TokenizerResources{resources.tokenizer_json, resources.tokenizer_config_json,
                                     resources.generation_config_json});
    const auto count = resources.tokenizer->vocab_size();
    if (!count || count > embedding_rows || !resources.tokenizer->has_exact_token_domain(count)) {
        throw artifact::ArtifactError(
            "tokenizer must expose a contiguous public domain within embedding rows");
    }
    resources.public_token_count = static_cast<std::uint32_t>(count);
    for (const auto token : resources.tokenizer->default_stop_token_ids()) {
        if (!resources.tokenizer->is_valid_token(token)) {
            throw artifact::ArtifactError("stop token is outside the public token domain");
        }
    }
    if (vision) {
        for (const auto bytes :
             {resources.preprocessor_config_json, resources.video_preprocessor_config_json}) {
            const auto value = artifact::parse_json(bytes, "Vision processor");
            if (!value.is_object() || !value.contains("patch_size") ||
                !value.contains("temporal_patch_size") || !value.contains("merge_size")) {
                throw artifact::ArtifactError("Vision processor is missing patch geometry");
            }
            if (artifact::require_u64(value.at("patch_size"), "patch_size", true) !=
                    vision->patch_size ||
                artifact::require_u64(value.at("temporal_patch_size"), "temporal_patch_size",
                                      true) != vision->temporal_patch_size ||
                artifact::require_u64(value.at("merge_size"), "merge_size", true) !=
                    vision->spatial_merge_size) {
                throw artifact::ArtifactError(
                    "Vision processor geometry differs from component config");
            }
        }
    }
}

} // namespace ninfer::models::qwen
