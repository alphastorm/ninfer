#pragma once

#include "models/qwen3_5/expert_weights.h"
#include "models/qwen3_5/model.h"

#include <filesystem>
#include <memory>
#include <span>

namespace ninfer::artifact {
class Reader;
struct ParameterReference;
} // namespace ninfer::artifact

namespace ninfer::models::qwen3_5 {

class ResolvedLoadPlan;

// Cold load plan borrows its Reader until materialization. Its selected Host bytes and parsed
// resources already have owners; no Program or device allocation is needed for plan_load.
class LoadPlan {
public:
    ~LoadPlan();
    LoadPlan(LoadPlan&&) noexcept;
    LoadPlan& operator=(LoadPlan&&) noexcept;
    LoadPlan(const LoadPlan&)            = delete;
    LoadPlan& operator=(const LoadPlan&) = delete;

    [[nodiscard]] const Config& config() const;
    [[nodiscard]] const LoadOptions& options() const;
    // Geometry and Use only; no backing or execution address until materialization.
    [[nodiscard]] ops::WeightInput input(WeightId) const;
    [[nodiscard]] ops::WeightInput input(WeightUseId) const;
    [[nodiscard]] artifact::MaterializationPlan placement(Device cuda, Device main_experts) const;
    [[nodiscard]] const ModelWeights& weights() const;
    [[nodiscard]] const qwen::FrontendResources& resources() const;
    [[nodiscard]] std::span<const moe::BankDescription> expert_banks() const;
    [[nodiscard]] const artifact::ParameterReference& parameter(WeightId id) const;
    [[nodiscard]] std::span<const WeightUse> uses(WeightId id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit LoadPlan(std::unique_ptr<Impl> impl);
    friend LoadPlan plan_load(const artifact::Reader&, LoadOptions);
    friend ResolvedLoadPlan resolve_load(LoadPlan&&, Device, std::optional<Device>);
    friend std::unique_ptr<Model> materialize_model(ResolvedLoadPlan&&, DeviceContext&,
                                                    const StartupObserver*);
};

class ResolvedLoadPlan {
public:
    [[nodiscard]] const LoadPlan& logical() const noexcept { return logical_; }

    [[nodiscard]] const artifact::MaterializationPlan& materialization() const noexcept {
        return materialization_;
    }

private:
    friend ResolvedLoadPlan resolve_load(LoadPlan&&, Device, std::optional<Device>);
    friend std::unique_ptr<Model> materialize_model(ResolvedLoadPlan&&, DeviceContext&,
                                                    const StartupObserver*);
    ResolvedLoadPlan(LoadPlan&& logical, artifact::MaterializationPlan&& materialization,
                     std::vector<Device>&& devices, std::vector<Device>&& bank_devices);
    artifact::MaterializationPlan materialization_;
    LoadPlan logical_;
    std::vector<Device> devices_;
    std::vector<Device> bank_devices_;
};

[[nodiscard]] LoadPlan plan_load(const artifact::Reader& reader, LoadOptions options = {});
[[nodiscard]] ResolvedLoadPlan resolve_load(LoadPlan&& plan, Device cuda,
                                            std::optional<Device> main_experts = {});
[[nodiscard]] std::unique_ptr<Model> materialize_model(ResolvedLoadPlan&& plan,
                                                       DeviceContext& device,
                                                       const StartupObserver* observer = nullptr);
[[nodiscard]] std::unique_ptr<Model> load_model(const std::filesystem::path& path,
                                                LoadOptions options, DeviceContext& device,
                                                const StartupObserver* observer = nullptr);

} // namespace ninfer::models::qwen3_5
