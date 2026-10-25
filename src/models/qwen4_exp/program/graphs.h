#pragma once

#include "models/qwen4_exp/program/text_graph.h"

namespace ninfer::models::qwen4_exp::detail {
class ProgramImpl;

class Graphs {
public:
    explicit Graphs(ProgramImpl&);
    void prepare();
    [[nodiscard]] TextGraph& main(std::uint32_t batch, bool verify, std::uint32_t visible);
    void draft(std::uint32_t batch, std::uint32_t step, std::uint32_t visible);
    void sample(std::uint32_t batch);
    void accept(std::uint32_t batch);

    [[nodiscard]] std::size_t workspace_peak() const { return workspace_peak_; }

    void reset_workspace_peak() { workspace_peak_ = 0; }

private:
    struct ContextCall {
        std::vector<DecodeGraphDefinition> definitions;
        std::vector<std::size_t> workspace_bytes;
        DecodeGraphExecutable executable;
        std::size_t installed = 0;
    };

    struct Batch {
        TextGraph ordinary, verify;
        GraphSegment sample, accept;
        std::vector<ContextCall> drafts;
        std::size_t sample_workspace = 0, accept_workspace = 0;
    };

    void prepare_main(Batch&, std::uint32_t batch, bool verify);
    void prepare_drafts(Batch&, std::uint32_t batch);
    ProgramImpl& owner_;
    std::vector<std::uint32_t> bounds_;
    std::vector<Batch> batches_;
    std::size_t workspace_peak_ = 0;
};

} // namespace ninfer::models::qwen4_exp::detail
