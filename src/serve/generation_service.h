#pragma once

// Product-side adapter between HTTP protocol requests and the public NInfer
// engine. It owns one Engine and keeps protocol concerns (aliases, usage,
// streaming callbacks, and tool-call parsing) outside the target package.

#include "ninfer/engine.h"
#include "serve/request.h"
#include "serve/session_checkpoint_store.h"
#include "serve/serve_options.h"
#include "serve/tool_call_parser.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ninfer::serve {

struct RequestLifetime;
struct RequestCapacity;
struct MediaInputCapacity;
struct ClientSessionLocks;
struct ClientSessionLease;

struct GenerationMetrics {
    double prepare_seconds = 0.0;
    double ttft_seconds    = 0.0;
    double vision_seconds  = 0.0;
    double prefill_seconds = 0.0;
    double decode_seconds  = 0.0;
    double total_seconds   = 0.0;

    SpeculativeBackend speculative_backend    = SpeculativeBackend::None;
    std::uint32_t speculative_draft_window    = 0;
    std::uint64_t speculative_rounds          = 0;
    std::uint64_t speculative_draft_tokens    = 0;
    std::uint64_t speculative_accepted_tokens = 0;
    std::uint64_t speculative_fallback_steps  = 0;
    std::vector<std::uint64_t> speculative_accepted_per_position;
    std::uint32_t prefix_cache_hit_tokens     = 0;
    ninfer::PrefixReusePath prefix_reuse_path = ninfer::PrefixReusePath::FullReset;
};

struct GenerationOutcome {
    std::string text;
    std::string reasoning;
    std::vector<ToolCall> tool_calls;
    int prompt_tokens                  = 0;
    int completion_tokens              = 0;
    int reasoning_tokens               = 0;
    std::size_t streamed_content_bytes = 0;
    ninfer::FinishReason finish_reason = ninfer::FinishReason::OutputLimit;
    GenerationMetrics metrics;
};

struct StreamSink {
    std::function<void(const std::string& delta_text)> on_content;
    std::function<void(const std::string& delta_text)> on_reasoning;
    std::function<bool()> is_cancelled;
};

// Preparation ends by synchronously submitting the owning prompt to the Engine FIFO. The returned
// request keeps its ingress/response lifetime reservation until the HTTP response is released and
// is consumed exactly once by run().
struct PreparedRequest {
    ninfer::GenerationHandle generation;
    ninfer::ResolvedSamplingParameters sampling;
    double prepare_seconds                 = 0.0;
    int prompt_tokens                      = 0;
    bool include_usage                     = false;
    bool tool_capable                      = false;
    std::size_t tool_name_max_length       = 64;
    ToolArgumentTypeContracts tool_argument_types;
    bool enable_thinking                   = true;
    std::optional<ninfer::ReasoningEffort> reasoning_effort;
    bool preserve_thinking                 = false;
    bool preserve_thinking_semantic_change = false;
    std::shared_ptr<RequestLifetime> lifetime;
    std::shared_ptr<ClientSessionLease> client_session;
};

class GenerationService {
public:
    explicit GenerationService(ServeOptions options, LoadProgress load_progress = {});

    [[nodiscard]] const ServeOptions& options() const noexcept { return options_; }

    [[nodiscard]] ninfer::LoadSummary load_summary() const { return engine_->load_summary(); }

    [[nodiscard]] ninfer::MemorySummary memory_summary() const { return engine_->memory_summary(); }

    [[nodiscard]] ninfer::RuntimeStats runtime_stats() const { return engine_->runtime_stats(); }

    [[nodiscard]] ninfer::ModelSamplingDefaults sampling_defaults() const {
        return engine_->sampling_defaults();
    }

    [[nodiscard]] PreparedRequest prepare(const GenerationRequest& req,
                                          std::function<bool()> is_cancelled = {},
                                          std::string checkpoint_tag = {}) const;
    [[nodiscard]] int count_prompt_tokens(const GenerationRequest& req,
                                          std::function<bool()> is_cancelled = {}) const;

    // Consumes prepared.generation. A PreparedRequest is single-use.
    GenerationOutcome run(PreparedRequest& prepared, const StreamSink* sink,
                          std::function<bool()> is_cancelled = {});

    [[nodiscard]] bool checkpoint_enabled() const noexcept;
    [[nodiscard]] SessionCheckpointSaveOutcome
    save_checkpoint(std::string_view session_sha256, std::string_view required_response_id,
                    ResponseStore& responses);
    [[nodiscard]] SessionCheckpointRestoreState
    restore_checkpoint(std::string_view session_sha256, std::string_view required_response_id,
                       ResponseStore& responses);
    [[nodiscard]] nlohmann::json checkpoint_status(std::string_view session_sha256);
    // True when the catalogued checkpoint already records response_id as the session's
    // newest stored response under the current runtime fingerprint (redundant-save skip).
    [[nodiscard]] bool checkpoint_covers(std::string_view session_sha256,
                                         std::string_view response_id) const;
    [[nodiscard]] SessionCheckpointEraseResult erase_checkpoint(std::string_view session_sha256);
    [[nodiscard]] SessionCheckpointEraseResult
    erase_checkpoint_response(std::string_view session_sha256, std::string_view response_id,
                              ResponseStore& responses);

    void warmup();

private:
    friend class HttpServer;

    [[nodiscard]] std::shared_ptr<RequestLifetime> acquire_request_lifetime() const;
    [[nodiscard]] HostInputLease
    acquire_media_input(std::chrono::steady_clock::time_point deadline,
                        const std::function<bool()>& is_cancelled) const;
    [[nodiscard]] std::shared_ptr<ClientSessionLease>
    acquire_client_session(std::string_view session_sha256,
                           std::chrono::steady_clock::time_point deadline,
                           const std::function<bool()>& is_cancelled) const;

    ServeOptions options_;
    std::unique_ptr<ninfer::Engine> engine_;
    ninfer::PromptCapabilities prompt_capabilities_;
    std::shared_ptr<RequestCapacity> request_capacity_;
    std::shared_ptr<MediaInputCapacity> media_input_capacity_;
    std::shared_ptr<ClientSessionLocks> client_session_locks_;
    std::string session_tenant_sha256_;
    std::unique_ptr<SessionCheckpointManager> checkpoint_manager_;
};

} // namespace ninfer::serve
