#include "models/qwen3_5/startup.h"
#include "models/qwen3_5/execution/profile.h"
#include "models/qwen3_5/program/planning/moe.h"

#include <algorithm>

namespace ninfer::models::qwen3_5 {
namespace {
WeightPlacement describe_placement(const LoadPlan& source, const execution::ModelProfile& profile,
                                   Device device, const EngineOptions& options) {
    WeightPlacement out;
    out.resident_bytes = source.placement(device, device).device_capacity_bytes;
    auto offload       = detail::describe_moe_plan(source, profile, options.moe);
    if (source.config().text.architecture != Architecture::Qwen3_5Moe) return out;
    out.fixed_bytes = source.placement(device, Device::cpu()).device_capacity_bytes;
    out.offload     = std::move(offload);
    for (const auto& bank : source.expert_banks())
        out.banks.push_back({bank.gate_up.layout, bank.down.layout, true});
    for (const auto& layer : out.offload->layers) out.banks[layer.spec.bank.index].resident = false;
    return out;
}

} // namespace

Startup::Startup(const artifact::Reader& reader, const EngineOptions& options, Device device)
    : reader_(reader), selection_(load_options(options)), source_(plan_load(reader, selection_)),
      architecture_(source_->config().text.architecture) {
    profile = execution::describe_model(*source_);
    weights = describe_placement(*source_, *profile, device, options);
}

std::optional<Startup::ExecutionPlan> Startup::plan(DeviceContext& device,
                                                    const EngineOptions& options, bool host_experts,
                                                    std::size_t available) const {
    using runtime::resolve_kv_capacity;
    auto trial       = options;
    const auto chunk = options.prefill_chunk ? options.prefill_chunk : host_experts ? 4096U : 1024U;
    trial.prefill_chunk             = chunk;
    auto policy                     = options.kv_capacity;
    policy.automatic_headroom_bytes = 0;
    auto prototype   = host_experts ? std::make_shared<moe::Plan>(*weights.offload) : nullptr;
    auto planner     = make_sequence_planner(profile, device, trial, prototype);
    const auto curve = planner.capacity_curve();
    const auto pages = policy.mode == KvCapacityMode::Explicit
                           ? static_cast<std::uint32_t>((std::uint64_t(policy.explicit_tokens) +
                                                         curve.primary_page_tokens - 1) /
                                                        curve.primary_page_tokens)
                           : curve.minimum_primary_page_groups;
    if (curve.reservation_bytes(pages) > available) return std::nullopt;
    if (!host_experts) {
        auto resolution = resolve_kv_capacity(policy, curve, available);
        return ExecutionPlan{std::move(planner).finalize(resolution.primary_page_groups),
                             resolution,
                             false,
                             chunk,
                             {}};
    }
    auto limited          = KvCapacityPolicy::explicit_capacity(curve.resolved_tokens(pages));
    auto base             = resolve_kv_capacity(limited, curve, available);
    const auto max_tokens = static_cast<std::int32_t>(
        std::max(std::min(chunk, options.max_context),
                 options.max_concurrency * (options.speculative.draft_tokens + 1)));
    CpuTopology topology;
    auto placement = moe::resolve_plan(*weights.offload, weights.banks,
                                       available - base.runtime_reservation_bytes, max_tokens,
                                       topology, options.moe.cpu_workers);
    if (!placement) return std::nullopt;
    auto moe   = std::make_shared<moe::Plan>(std::move(*placement));
    auto final = make_sequence_planner(profile, device, trial, moe);
    if (final.capacity_curve().reservation_bytes(pages) > available) return std::nullopt;
    auto resolution = resolve_kv_capacity(limited, final.capacity_curve(), available);
    resolution.mode = policy.mode;
    return ExecutionPlan{std::move(final).finalize(resolution.primary_page_groups), resolution,
                         true, chunk, std::move(moe)};
}

std::unique_ptr<Model> Startup::materialize(DeviceContext& device, bool host_experts,
                                            const StartupObserver& observer) {
    if (!source_) source_.emplace(plan_load(reader_, selection_));
    auto resolved = resolve_load(std::move(*source_), device.device(),
                                 host_experts ? Device::cpu() : device.device());
    source_.reset();
    return materialize_model(std::move(resolved), device, &observer);
}

qwen::Frontend Startup::frontend(const Model& model, const EngineOptions& options) const {
    return qwen::make_frontend(model.resources(),
                               {.chat_template_path       = options.chat_template_path,
                                .grammar_cache_bytes      = options.grammar_cache_bytes,
                                .vision_enabled           = options.enable_vision,
                                .max_context              = options.max_context,
                                .media_cache_bytes        = options.media_cache_bytes,
                                .media_live_bytes         = options.media_live_bytes,
                                .media_preprocess_threads = options.media_preprocess_threads},
                               default_sampling(architecture_));
}

GraphPreparationPlan Startup::graph_plan(const EngineOptions& options,
                                         const moe::Plan* plan) const {
    return describe_graph_preparation(options, plan);
}

Startup::Calibration Startup::calibrate(const execution::Parameters& parameters,
                                        DeviceContext& device, const EngineOptions& options,
                                        std::shared_ptr<const moe::Plan> plan) const {
    return calibrate_graphs(parameters, device, options, std::move(plan));
}

} // namespace ninfer::models::qwen3_5
