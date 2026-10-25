#include "models/qwen4_exp/program/graph_preparation.h"
#include "models/qwen4_exp/program/internal.h"
#include "core/startup.h"

#include <algorithm>

namespace ninfer::models::qwen4_exp {

GraphPreparationPlan describe_graph_preparation(const EngineOptions& options,
                                                const moe::Plan* plan) {
    GraphPreparationPlan out;
    if (!options.use_cuda_graph) return out;
    if (!plan) throw std::logic_error("Qwen4Exp graphs require a Main expert plan");
    for (std::uint32_t batch = 1; batch <= options.max_concurrency; ++batch)
        for (const auto& layer : plan->layers) {
            out.ordinary.push_back(moe::graph_plan(*plan, layer.spec.bank, batch));
            if (options.speculative.backend == SpeculativeBackend::Mtp)
                out.verify.push_back(moe::graph_plan(
                    *plan, layer.spec.bank, batch * (options.speculative.draft_tokens + 1)));
        }
    return out;
}

GraphCalibration calibrate_graphs(const execution::Parameters& parameters, DeviceContext& device,
                                  const EngineOptions& options,
                                  std::shared_ptr<const moe::Plan> moe) {
    GraphCalibration result;
    result.plan = describe_graph_preparation(options, moe.get());
    if (!options.use_cuda_graph) return result;
    StartupPhaseScope phase(options.startup_observer, StartupPhase::CudaGraphCalibration);
    auto plan  = plan_graph_probe(parameters.profile, parameters.model.ple_table(), options, device,
                                  std::move(moe));
    auto probe = std::make_unique<detail::ProgramImpl>(parameters, std::move(plan), device);
    const auto before = device_memory_snapshot(device.device());
    probe->initialize_graphs();
    const auto ready = device_memory_snapshot(device.device());
    probe->release_graphs();
    const auto released                = device_memory_snapshot(device.device());
    result.preparation_increment_bytes = device_memory_growth(before, ready);
    result.retained_increment_bytes    = device_memory_growth(before, released);
    result.rebuild_bytes =
        static_cast<std::size_t>(std::max<std::int64_t>(0, device_memory_growth(released, ready)));
    probe.reset();
    device.synchronize();
    phase.complete();
    return result;
}

} // namespace ninfer::models::qwen4_exp
