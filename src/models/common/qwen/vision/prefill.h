#pragma once

#include "models/common/qwen/vision/vision.h"

namespace ninfer::models::qwen {

// Select contiguous encoder columns whose prompt positions overlap an embedding window.
struct VisualEmbeddingSlice {
    std::size_t source_begin = 0;
    std::vector<std::int32_t> destination_columns;

    [[nodiscard]] std::size_t size() const noexcept { return destination_columns.size(); }

    [[nodiscard]] bool empty() const noexcept { return destination_columns.empty(); }
};

[[nodiscard]] VisualEmbeddingSlice
visual_embedding_slice(std::span<const std::int32_t> scatter_indices, std::uint32_t prompt_tokens,
                       std::uint32_t embedding_begin, std::uint32_t columns);

void scatter_visual_embeddings(Tensor& input, const Tensor& visual,
                               const VisualEmbeddingSlice& slice, Tensor& destination_indices,
                               CudaExecutionView execution);

class VisionPrefillSession;

// One state per Program workspace. Only Vision encoding can replace the handoff;
// Text and decode use the disjoint general workspace and preserve its owner.
class VisionHandoffState {
    friend class VisionPrefillSession;

    const VisionPrefillSession* owner_ = nullptr;
    std::uint64_t generation_          = 0;
};

struct VisionUseSpan {
    std::uint32_t begin               = 0;
    std::uint32_t end                 = 0;
    std::uint32_t prepared_item_index = 0;
    std::uint32_t control_index       = 0;
};

struct VisionPrefillPlan {
    std::shared_ptr<const VisionControlPlan> control_plan;
    std::shared_ptr<const VisionControl> control;
    std::vector<VisionUseSpan> uses;
    std::size_t max_merged_count = 0;

    [[nodiscard]] std::uint32_t window_end(std::uint32_t begin, std::uint32_t end) const;
};

struct VisionChunk {
    std::int32_t length              = 0;
    const VisionItemControl* control = nullptr;
    Tensor embeddings;
    std::size_t encode_workspace_bytes = 0;
};

class VisionPrefillSession {
public:
    VisionPrefillSession(DeviceContext& device, const VisionConfig& config,
                         const VisionParameters& parameters, MemorySpan workspace,
                         const VisionWorkspacePlan& workspace_plan,
                         const PreparedPromptData& prompt, const VisionPrefillPlan& plan,
                         VisionHandoffState& handoff, std::size_t& handoff_peak_bytes);
    ~VisionPrefillSession();

    VisionPrefillSession(const VisionPrefillSession&)            = delete;
    VisionPrefillSession& operator=(const VisionPrefillSession&) = delete;

    [[nodiscard]] VisionChunk prepare_chunk(std::uint32_t begin, std::uint32_t nominal_length);
    void retire_handoff() noexcept;
    [[nodiscard]] double elapsed_seconds() const;

    [[nodiscard]] std::size_t active_handoff_bytes() const noexcept {
        return owns_handoff() ? active_handoff_bytes_ : 0;
    }

private:
    [[nodiscard]] bool owns_handoff() const noexcept {
        return handoff_.owner_ == this && handoff_.generation_ == active_generation_;
    }

    DeviceContext& device_;
    MemorySpan workspace_;
    const VisionWorkspacePlan& workspace_plan_;
    const PreparedPromptData& prompt_;
    const VisionPrefillPlan& plan_;
    VisionHandoffState& handoff_;
    std::size_t& handoff_peak_bytes_;
    VisionContext context_;
    std::optional<std::uint32_t> active_item_;
    std::uint64_t active_generation_  = 0;
    std::size_t active_handoff_bytes_ = 0;
    std::vector<CudaEventTimer> timers_;
};

} // namespace ninfer::models::qwen
