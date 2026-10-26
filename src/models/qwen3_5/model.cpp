#include "models/qwen3_5/model.h"

#include <stdexcept>
#include <utility>

namespace ninfer::models::qwen3_5 {

Model::Model(Config config, LoadOptions options, ModelWeights weights,
             std::vector<BoundWeight> bound, std::vector<ExpertBankSource> banks,
             qwen::FrontendResources resources, InstanceInfo info,
             artifact::MaterializedArtifact backing)
    : backing_(std::move(backing)), config_(std::move(config)), options_(options),
      weights_(std::move(weights)), bound_(std::move(bound)), banks_(std::move(banks)),
      resources_(std::move(resources)), info_(std::move(info)) {}

Model::~Model() = default;

ops::WeightInput Model::input(WeightUseId id) const {
    return ops::effective_weight_input(weight_input(bound_, id), options_.native_prefill_policy);
}

ops::WeightInput Model::input(WeightId id) const {
    return ops::effective_weight_input(weight_input(bound_, id), options_.native_prefill_policy);
}

} // namespace ninfer::models::qwen3_5
