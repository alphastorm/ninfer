#pragma once

#include "models/common/startup.h"
#include "models/qwen3_5/load.h"
#include "models/qwen3_5/program/runtime_types.h"
#include "models/qwen3_5/program/graph_preparation.h"

namespace ninfer::models::qwen3_5 {

// Native loading and execution planning. Runtime owns the budget, calibration loop and retries.
class Startup {
public:
    static constexpr bool supports_graph = true;
    using Types                          = RuntimeTypes;
    using ExecutionPlan                  = models::ExecutionPlan<SequencePlan>;
    using Calibration                    = GraphCalibration;

    Startup(const artifact::Reader&, const EngineOptions&, Device);

    [[nodiscard]] Architecture architecture() const noexcept { return architecture_; }

    [[nodiscard]] std::optional<ExecutionPlan> plan(DeviceContext&, const EngineOptions&,
                                                    bool host_experts, std::size_t available) const;
    [[nodiscard]] std::unique_ptr<Model> materialize(DeviceContext&, bool host_experts,
                                                     const StartupObserver&);
    [[nodiscard]] qwen::Frontend frontend(const Model&, const EngineOptions&) const;
    [[nodiscard]] GraphPreparationPlan graph_plan(const EngineOptions&, const moe::Plan*) const;
    [[nodiscard]] Calibration calibrate(const execution::Parameters&, DeviceContext&,
                                        const EngineOptions&,
                                        std::shared_ptr<const moe::Plan>) const;

    WeightPlacement weights;
    std::shared_ptr<const execution::ModelProfile> profile;

private:
    const artifact::Reader& reader_;
    LoadOptions selection_;
    std::optional<LoadPlan> source_;
    Architecture architecture_;
};

} // namespace ninfer::models::qwen3_5
