#pragma once

#include "serve/automatic_checkpoint_queue.h"
#include "serve/generation_service.h"
#include "serve/response_store.h"
#include "serve/request_log.h"
#include "serve/serve_options.h"

#include <httplib.h>

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace ninfer::serve {

// cpp-httplib invokes the error handler for every application response with status >= 400. Only
// an empty 413 is its own pre-routing payload-limit rejection; application-authored errors must be
// left untouched.
httplib::Server::HandlerResponse handle_unrendered_http_error(const ServeOptions& options,
                                                              const httplib::Request& request,
                                                              httplib::Response& response);

// Parses the exact authenticated checkpoint-save body. Throws ApiException on malformed, missing,
// extra, non-string, or non-digest input.
[[nodiscard]] std::string parse_checkpoint_save_request_body(std::string_view body);

// Parses the optional session credential header shared by bodyless Responses routes. A duplicate,
// malformed, or unauthenticated value is rejected without echoing it.
[[nodiscard]] std::optional<std::string>
parse_client_session_header(const httplib::Request& request, bool authentication_configured);

// Responses create accepts the session credential on either surface: the header alone binds the
// request to that session, and a header that disagrees with the body's ninfer_session is
// rejected rather than silently choosing one of them.
void apply_client_session_header(const httplib::Request& request, bool authentication_configured,
                                 GenerationRequest& generation);

// Checkpoint status/delete resolve the session credential from the URL path parameter
// (the released OMP client's addressing: /v1/ninfer/checkpoints/<sha256>...) or from
// X-NInfer-Session on the collection routes; disagreement between the two is rejected.
[[nodiscard]] std::optional<std::string>
checkpoint_session_path_argument(const httplib::Request& request);
[[nodiscard]] std::string
require_checkpoint_session_identity(const std::optional<std::string>& path_value,
                                    const httplib::Request& request,
                                    bool authentication_configured);

// Renders the authenticated DELETE result without echoing the session identity or stored content.
void write_checkpoint_delete_response(httplib::Response& response,
                                      SessionCheckpointEraseResult result);

// What the shutdown flush managed to do. A session with nothing durable to export is not a
// loss; a session whose live state the engine refused to export is, and a stop that reports
// success while losing it would be a false durability claim.
struct ShutdownCheckpointSummary {
    std::size_t saved   = 0;
    std::size_t skipped = 0; // nothing durable to export
    std::size_t refused = 0; // live state that could not be exported

    [[nodiscard]] bool complete() const noexcept { return refused == 0; }
};

class HttpServer {
public:
    explicit HttpServer(ServeOptions options);

    // Reserves the configured address before model loading. The service is attached only after its
    // Engine is ready, then listen() enters the blocking accept loop on the already-bound socket.
    bool bind();
    void attach(GenerationService& service);
    bool listen();
    // Requests the accept loop to exit: in-flight requests finish, then listen() returns. A request
    // that arrives before listen() is remembered, and listen() then returns at once instead of
    // serving. Returns whether the loop was running, i.e. whether the request could take effect
    // now; a caller on another thread re-asserts it until it does (StopEventWatcher).
    bool stop() noexcept;
    // Saves every live session after the listener has closed. Reports what it could not save so
    // the exit status can tell an operator that a deliberate stop lost state.
    ShutdownCheckpointSummary save_all_checkpoints() noexcept;

    [[nodiscard]] const std::string& public_model_id() const noexcept { return public_model_id_; }

private:
    void register_routes();
    void handle_chat_completions(const httplib::Request& req, httplib::Response& res);
    void handle_messages(const httplib::Request& req, httplib::Response& res);
    void handle_count_tokens(const httplib::Request& req, httplib::Response& res);
    void handle_responses(const httplib::Request& req, httplib::Response& res);
    void handle_response_input_tokens(const httplib::Request& req, httplib::Response& res);
    void handle_response_get(const httplib::Request& req, httplib::Response& res);
    void handle_response_delete(const httplib::Request& req, httplib::Response& res);
    void handle_response_input_items(const httplib::Request& req, httplib::Response& res);
    void handle_response_cancel(const httplib::Request& req, httplib::Response& res);
    void handle_response_compact(const httplib::Request& req, httplib::Response& res);
    void handle_models(const httplib::Request& req, httplib::Response& res) const;
    void handle_model(const httplib::Request& req, httplib::Response& res) const;
    void handle_status(const httplib::Request& req, httplib::Response& res) const;
    void handle_checkpoint_get(const httplib::Request& req, httplib::Response& res) const;
    void handle_checkpoint_save(const httplib::Request& req, httplib::Response& res);
    void handle_checkpoint_delete(const httplib::Request& req, httplib::Response& res);
    void maybe_checkpoint_completed_turn(const std::optional<std::string>& session_sha256,
                                         const GenerationOutcome& outcome) noexcept;
    void save_automatic_checkpoint(std::string_view session_sha256) noexcept;

    // The process-wide console logger serializes lines from request and reporter threads.
    void log_line(const std::string& line);
    void log_request_start(const RequestLogContext& context);
    void log_request_rejected(const RequestRejectionLogContext& context);
    void log_request_done(const RequestLogContext& context, const GenerationOutcome& outcome);
    void log_request_error(const RequestLogContext& context, const std::string& message);
    void log_throughput(const ThroughputReport& report);
    void run_stats_reporter();
    void stop_stats_reporter();

    GenerationService* service_ = nullptr;
    ServeOptions options_;
    std::string public_model_id_;
    ResponseStore response_store_;
    // Immutable startup values are captured once so status polling only snapshots published
    // counters and never contends with execution for a live memory summary.
    ninfer::EngineOptions status_engine_options_;
    ninfer::LoadSummary status_load_;
    ninfer::MemorySummary status_memory_;
    JsonlRequestLog request_jsonl_;
    httplib::Server server_;
    std::atomic<std::uint64_t> request_seq_{0};
    std::atomic<bool> stop_requested_{false};
    std::mutex stats_mutex_;
    std::condition_variable stats_cv_;
    std::thread stats_thread_;
    bool stats_stopping_ = false;
    // Declared last so it drains and joins before the service pointer and ResponseStore disappear.
    std::unique_ptr<AutomaticCheckpointQueue> automatic_checkpoints_;
};

} // namespace ninfer::serve
