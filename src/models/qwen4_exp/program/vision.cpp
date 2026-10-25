#include "models/qwen4_exp/program/internal.h"
#include "core/transfer.h"

#include <algorithm>

namespace ninfer::models::qwen4_exp::detail {

qwen::VisionPrefillPlan ProgramImpl::plan_vision(const qwen::PreparedPromptData& prompt) const {
    if (!parameters.vision) throw std::invalid_argument("Vision is disabled for this Engine");
    const auto& config    = *parameters.model.config().vision;
    const auto& workspace = *resources.plan.workspace.vision;
    qwen::VisionPrefillPlan out;
    out.control_plan =
        std::make_shared<qwen::VisionControlPlan>(qwen::plan_vision_control(prompt, config));
    out.control = std::make_shared<qwen::VisionControl>(
        qwen::build_vision_control(prompt, *out.control_plan, 0));
    for (std::uint32_t i = 0; i < out.control_plan->items.size(); ++i) {
        const auto& item    = out.control_plan->items[i];
        const auto patches  = prompt.vision_items[i].patch_count;
        const auto& payload = prompt.media_payloads[i];
        if (!payload || payload->patch_elements != patches * config.patch_width())
            throw std::invalid_argument("Vision item has an invalid patch payload");
        if (item.merged_count > workspace.max_merged_tokens ||
            qwen::VisionContext::workspace_bytes(config, *parameters.vision, patches,
                                                 item.merged_count,
                                                 workspace) > workspace.encode_peak_bytes)
            throw std::invalid_argument("Vision item exceeds the Program workspace envelope");
        out.uses.push_back({item.token_begin, item.token_end, i, i});
        out.max_merged_count = std::max(out.max_merged_count, item.merged_count);
    }
    return out;
}

std::optional<execution::VisualEmbeddingView> ProgramImpl::prepare_vision(Sequence& s,
                                                                          std::uint32_t tokens) {
    const auto& request = *s.request;
    const auto begin    = s.continuation.metadata.main_frontier;
    if (!request.vision || begin >= request.summary.prompt_tokens) return {};
    const auto& plan = *request.vision;
    const auto item  = std::lower_bound(
        plan.uses.begin(), plan.uses.end(), begin,
        [](const auto& use, std::uint32_t position) { return use.end <= position; });
    if (item == plan.uses.end() || item->begin >= begin + tokens) return {};
    if (!s.vision)
        s.vision = std::make_unique<qwen::VisionPrefillSession>(
            device, *parameters.model.config().vision, *parameters.vision,
            resources.workspace_storage(), *resources.plan.workspace.vision, request.prompt, plan,
            vision_handoff, vision_handoff_peak_bytes);
    const auto chunk = s.vision->prepare_chunk(begin, tokens);
    if (chunk.length != tokens || !chunk.control)
        throw std::logic_error("Vision execution differs from the reserved input window");
    vision_workspace_peak_bytes =
        std::max(vision_workspace_peak_bytes, chunk.encode_workspace_bytes);
    const auto slice = qwen::visual_embedding_slice(chunk.control->scatter_indices,
                                                    request.summary.prompt_tokens, begin, tokens);
    if (slice.empty()) return {};
    const auto count   = static_cast<std::int32_t>(slice.size());
    const auto& region = *resources.plan.persistent.buffers.batch.visual_indices;
    auto indices       = device_tensor(region).slice(0, 0, count);
    auto host          = host_tensor(region).slice(0, 0, count);
    std::copy(slice.destination_columns.begin(), slice.destination_columns.end(),
              static_cast<std::int32_t*>(host.data));
    copy_bytes_async({indices.data, indices.bytes(), cuda.device()},
                     {host.data, host.bytes(), Device::cpu(), true}, cuda);
    return execution::VisualEmbeddingView{
        chunk.embeddings.slice(1, static_cast<std::int32_t>(slice.source_begin), count), indices};
}

double ProgramImpl::collect_vision_timing(Sequence& s) {
    if (!s.vision) return 0;
    const auto elapsed        = s.vision->elapsed_seconds();
    const auto increment      = elapsed - s.observed_vision_seconds;
    s.observed_vision_seconds = elapsed;
    s.timings.vision_seconds += increment;
    if (s.continuation.metadata.main_frontier >= s.request->vision->uses.back().end)
        retire_vision(s);
    return increment;
}

void ProgramImpl::retire_vision(Sequence& s) noexcept {
    s.vision.reset();
    s.observed_vision_seconds = 0;
}

std::size_t ProgramImpl::active_vision_bytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& s : sequences)
        if (s && s->vision) bytes = std::max(bytes, s->vision->active_handoff_bytes());
    return bytes;
}

} // namespace ninfer::models::qwen4_exp::detail
