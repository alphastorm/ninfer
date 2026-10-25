#pragma once

#include "serve/automatic_checkpoint_queue.h"
#include "serve/checkpoint_policy.h"
#include "serve/session_checkpoint_store.h"
#include "serve/serve_options.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace ninfer::serve {

struct ShutdownCheckpointSummary {
    std::uint64_t saved = 0;
    std::uint64_t skipped = 0;
    std::uint64_t refused = 0;
    [[nodiscard]] bool complete() const noexcept { return refused == 0; }
};

// Engine owns all native transactions. This coordinator owns only disk/Responses ordering,
// automatic-save work and the authenticated, once-resolved startup fingerprint.
class SessionCheckpointService final : public runtime::PressureCheckpointHandler,
                                       public runtime::ReclaimableSessionOracle {
public:
    SessionCheckpointService(Engine& engine, const ServeOptions& options,
                              OpenAIResponsesStore& responses);
    ~SessionCheckpointService();
    SessionCheckpointService(const SessionCheckpointService&) = delete;
    SessionCheckpointService& operator=(const SessionCheckpointService&) = delete;

    [[nodiscard]] runtime::DurableImportResult restore(
        std::string_view session, std::optional<std::string_view> required_response);
    [[nodiscard]] nlohmann::json save(std::string_view session);
    [[nodiscard]] nlohmann::json status(std::string_view session) const;
    [[nodiscard]] SessionCheckpointEraseResult erase(std::string_view session);
    [[nodiscard]] bool erase_response(const std::string& id, const std::string& session);
    void response_stored(std::string_view session, std::uint64_t frontier_tokens) noexcept;
    void publication_changed() noexcept;
    [[nodiscard]] bool final_save() noexcept;

    [[nodiscard]] std::shared_ptr<const runtime::ContinuationCheckpointReader>
    pin_recoverable(std::string_view session, std::string_view tag) const noexcept override;
    void save_before_eviction(std::span<runtime::PressureCheckpointVictim> victims) noexcept override;

private:
    struct SaveOutcome {
        std::optional<SessionCheckpointSaveResult> saved;
        runtime::SessionCheckpointSkipDetail skip;
        std::string tag;
        bool covered = false;
    };
    [[nodiscard]] SaveOutcome save_session(std::string_view session,
                                           std::optional<std::string_view> expected_tag = {},
                                           bool allow_covered = true);
    [[nodiscard]] std::shared_ptr<std::mutex> session_operation(std::string_view session);
    void automatic_save(std::string_view session) noexcept;
    void receipt(const nlohmann::json& value) const noexcept;
    [[nodiscard]] bool write_shutdown_report(const ShutdownCheckpointSummary& summary) const noexcept;

    Engine& engine_;
    OpenAIResponsesStore& responses_;
    const ServeOptions& options_;
    const runtime::RuntimeFingerprintV2 fingerprint_;
    const nlohmann::json fingerprint_json_;
    const runtime::DurableLimits limits_;
    mutable SessionCheckpointStore store_;
    AutomaticCheckpointQueue automatic_;
    std::atomic<bool> stopping_{false};
    std::mutex operations_mutex_;
    std::unordered_map<std::string, std::weak_ptr<std::mutex>> operations_;
    struct PressureSave {
        std::string tag;
        std::uint64_t attempted_progress = 0;
        bool settled = false;
    };
    std::atomic<std::uint64_t> progress_{1};
    std::mutex pressure_mutex_;
    std::unordered_map<std::string, PressureSave> pressure_saves_;
    std::mutex final_mutex_;
    std::optional<bool> final_result_;
};

} // namespace ninfer::serve
