#include "models/qwen4_exp/model.h"

namespace ninfer::models::qwen4_exp {
Model::Model(Config config, LoadOptions options, ModelWeights weights,
             std::vector<BoundWeight> bound, std::vector<ExpertBankSource> banks,
             qwen::FrontendResources resources, std::string name,
             artifact::MaterializedArtifact backing, std::unique_ptr<ple::disk::Table> ple)
    : backing_(std::move(backing)), ple_(std::move(ple)), config_(std::move(config)),
      options_(options), weights_(std::move(weights)), bound_(std::move(bound)),
      banks_(std::move(banks)), resources_(std::move(resources)), info_{std::move(name)} {}
} // namespace ninfer::models::qwen4_exp
