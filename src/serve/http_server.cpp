#include "serve/http_server.h"

#include "serve/anthropic_schema.h"
#include "serve/client_identity.h"
#include "serve/credential_compare.h"
#include "serve/console_log.h"
#include "serve/openai_schema.h"
#include "serve/request_log.h"
#include "serve/translate.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <string_view>
#include <utility>

namespace ninfer::serve {
namespace {

struct StreamingRequest {
    explicit StreamingRequest(PreparedRequest request) : prepared(std::move(request)) {}

    PreparedRequest prepared;
    std::atomic<bool> cancelled{false};
    bool started = false;
};

class ClientDisconnected final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override { return "client disconnected"; }
};

void write_stream_item(httplib::DataSink& sink, StreamingRequest& request,
                       const std::string& item) {
    if (request.cancelled.load(std::memory_order_acquire) ||
        !sink.write(item.data(), item.size())) {
        request.cancelled.store(true, std::memory_order_release);
        throw ClientDisconnected();
    }
}

void set_owned_content(httplib::Response& response, std::string body,
                       std::shared_ptr<RequestLifetime> lifetime) {
    response.set_content(std::move(body), "application/json");
    response.hold_resource(std::move(lifetime));
}

void write_error(httplib::Response& res, const ApiError& error) {
    res.status = error.status;
    res.set_content(make_error_body(error), "application/json");
}

// Anthropic-shaped error body ({"type":"error","error":{...}}), used by the
// /v1/messages endpoints so Claude clients see the error format they expect.
void write_messages_error(httplib::Response& res, const ApiError& error) {
    res.status = error.status;
    res.set_content(make_messages_error_body(error), "application/json");
}

void write_exception(httplib::Response& res, const std::exception& ex) {
    ApiError error;
    error.status  = 500;
    error.type    = "internal_error";
    error.message = ex.what();
    write_error(res, error);
}

std::string sse_error_event(const ApiError& error) {
    return "data: " + make_error_body(error) + "\n\n";
}

ThroughputReport make_throughput_report(const ninfer::RuntimeStats& previous,
                                        const ninfer::RuntimeStats& current,
                                        double interval_seconds) {
    return ThroughputReport{
        .interval_seconds = interval_seconds,
        .computed_prefill_tokens =
            current.computed_prefill_tokens - previous.computed_prefill_tokens,
        .committed_decode_tokens =
            current.committed_decode_tokens - previous.committed_decode_tokens,
        .decode_rounds     = current.decode_rounds - previous.decode_rounds,
        .decode_row_rounds = current.decode_row_rounds - previous.decode_row_rounds,
        .previous          = previous,
        .current           = current,
    };
}

bool report_has_activity(const ThroughputReport& report) {
    return report.computed_prefill_tokens != 0 || report.committed_decode_tokens != 0 ||
           report.decode_rounds != 0 || report.current.running_requests != 0 ||
           report.current.waiting_requests != 0 || report.current.materializing_requests != 0 ||
           report.current.capture_pending_requests != 0 ||
           report.current.terminal_pending_requests != 0 ||
           report.current.active_captures_completed != report.previous.active_captures_completed ||
           report.current.active_captures_aborted != report.previous.active_captures_aborted ||
           report.current.root_selections != report.previous.root_selections ||
           report.current.private_endpoint_selections !=
               report.previous.private_endpoint_selections ||
           report.current.private_turn_closure_selections !=
               report.previous.private_turn_closure_selections ||
           report.current.private_response_replay_selections !=
               report.previous.private_response_replay_selections ||
           report.current.private_long_anchor_selections !=
               report.previous.private_long_anchor_selections ||
           report.current.shared_stable_prefix_selections !=
               report.previous.shared_stable_prefix_selections ||
           report.current.state_moves != report.previous.state_moves ||
           report.current.state_forks != report.previous.state_forks ||
           report.current.state_restores != report.previous.state_restores ||
           report.current.state_d2h_count != report.previous.state_d2h_count ||
           report.current.state_h2d_count != report.previous.state_h2d_count ||
           report.current.state_d2d_count != report.previous.state_d2d_count ||
           report.current.main_kv_d2h_pages != report.previous.main_kv_d2h_pages ||
           report.current.main_kv_h2d_pages != report.previous.main_kv_h2d_pages ||
           report.current.main_kv_d2d_pages != report.previous.main_kv_d2d_pages ||
           report.current.backend_kv_d2h_pages != report.previous.backend_kv_d2h_pages ||
           report.current.backend_kv_h2d_pages != report.previous.backend_kv_h2d_pages ||
           report.current.backend_kv_d2d_pages != report.previous.backend_kv_d2d_pages ||
           report.current.pressure_spill_pages != report.previous.pressure_spill_pages ||
           report.current.partial_tail_cow_pages != report.previous.partial_tail_cow_pages ||
           report.current.pressure_private_owners_degraded !=
               report.previous.pressure_private_owners_degraded ||
           report.current.pressure_private_owners_evicted !=
               report.previous.pressure_private_owners_evicted ||
           report.current.pressure_shared_owners_degraded !=
               report.previous.pressure_shared_owners_degraded ||
           report.current.pressure_shared_owners_evicted !=
               report.previous.pressure_shared_owners_evicted ||
           report.current.pressure_checkpoints_dropped !=
               report.previous.pressure_checkpoints_dropped ||
           report.current.pressure_searches != report.previous.pressure_searches ||
           report.current.pressure_search_budget_exhaustions !=
               report.previous.pressure_search_budget_exhaustions ||
           report.current.pressure_maximal_fallback_selections !=
               report.previous.pressure_maximal_fallback_selections ||
           report.current.historical_fork_hits != report.previous.historical_fork_hits ||
           report.current.device_state_occupied_slots !=
               report.previous.device_state_occupied_slots ||
           report.current.host_state_occupied_slots != report.previous.host_state_occupied_slots ||
           report.current.device_main_kv_occupied_pages !=
               report.previous.device_main_kv_occupied_pages ||
           report.current.device_backend_kv_occupied_pages !=
               report.previous.device_backend_kv_occupied_pages ||
           report.current.host_kv_occupied_bytes != report.previous.host_kv_occupied_bytes ||
           report.current.shared_active_references != report.previous.shared_active_references ||
           report.current.host_work.engine_boundary_ns !=
               report.previous.host_work.engine_boundary_ns ||
           report.current.host_work.program_submit_ns !=
               report.previous.host_work.program_submit_ns ||
           report.current.host_work.program_post_ns != report.previous.host_work.program_post_ns ||
           report.current.host_work.engine_commit_output_ns !=
               report.previous.host_work.engine_commit_output_ns ||
           report.current.host_work.engine_maintenance_ns !=
               report.previous.host_work.engine_maintenance_ns ||
           report.current.host_work.device_wait_ns != report.previous.host_work.device_wait_ns;
}

std::string_view unstreamed_content(const GenerationOutcome& outcome) {
    if (outcome.streamed_content_bytes > outcome.text.size()) {
        throw std::logic_error("streamed content exceeds terminal content");
    }
    return std::string_view(outcome.text).substr(outcome.streamed_content_bytes);
}

bool valid_session_digest(std::string_view digest) {
    return digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](unsigned char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

[[noreturn]] void throw_checkpoint_request_error(std::string message, std::string code = {}) {
    ApiError error;
    error.status  = 400;
    error.code    = std::move(code);
    error.message = std::move(message);
    throw ApiException(std::move(error));
}

} // namespace

std::optional<std::string> parse_client_session_header(const httplib::Request& request,
                                                       bool authentication_configured) {
    constexpr const char* header = "X-NInfer-Session";
    const std::size_t count      = request.get_header_value_count(header);
    if (count == 0) { return std::nullopt; }
    if (count != 1) {
        ApiError error;
        error.status  = 400;
        error.message = "X-NInfer-Session must occur exactly once";
        error.param   = "ninfer_session";
        error.code    = "invalid_ninfer_identity";
        throw ApiException(std::move(error));
    }
    GenerationRequest identity;
    identity.client_session_sha256 =
        parse_client_identity_sha256(request.get_header_value(header), "ninfer_session");
    require_authenticated_client_identity(identity, authentication_configured);
    return identity.client_session_sha256;
}

std::optional<std::string> checkpoint_session_path_argument(const httplib::Request& request) {
    if (request.matches.size() > 1) { return request.matches[1].str(); }
    return std::nullopt;
}

// Resolves the checkpoint session digest from either surface the product ships:
// the released OMP client addresses checkpoints by URL path parameter
// (GET /v1/ninfer/checkpoints/<sha256>/status, DELETE /v1/ninfer/checkpoints/<sha256>),
// while the header form (X-NInfer-Session on the collection routes) remains for
// operator tooling. A path parameter and a header that disagree are rejected.
std::string require_checkpoint_session_identity(const std::optional<std::string>& path_value,
                                                const httplib::Request& request,
                                                bool authentication_configured) {
    std::optional<std::string> path_digest;
    if (path_value) {
        GenerationRequest identity;
        identity.client_session_sha256 =
            parse_client_identity_sha256(*path_value, "ninfer_session");
        require_authenticated_client_identity(identity, authentication_configured);
        path_digest = identity.client_session_sha256;
    }
    std::optional<std::string> header_digest =
        parse_client_session_header(request, authentication_configured);
    if (path_digest && header_digest && *path_digest != *header_digest) {
        ApiError error;
        error.status  = 400;
        error.message = "checkpoint session path and X-NInfer-Session disagree";
        error.param   = "ninfer_session";
        error.code    = "invalid_session";
        throw ApiException(std::move(error));
    }
    if (path_digest) { return std::move(*path_digest); }
    if (header_digest) { return std::move(*header_digest); }
    ApiError error;
    error.status  = 400;
    error.message = "checkpoint session identity is required in the URL path or X-NInfer-Session";
    error.param   = "ninfer_session";
    error.code    = "invalid_session";
    throw ApiException(std::move(error));
}

std::string parse_checkpoint_save_request_body(std::string_view body) {
    const nlohmann::json request = nlohmann::json::parse(body, nullptr, false);
    if (request.is_discarded()) {
        throw_checkpoint_request_error("request body is not valid JSON");
    }
    if (!request.is_object()) {
        throw_checkpoint_request_error("request body must be a JSON object");
    }
    for (const auto& [field, value] : request.items()) {
        (void)value;
        if (field != "session_sha256") {
            throw_checkpoint_request_error("unsupported checkpoint field '" + field + "'");
        }
    }
    if (!request.contains("session_sha256")) {
        throw_checkpoint_request_error("session_sha256 is required");
    }
    if (!request.at("session_sha256").is_string()) {
        throw_checkpoint_request_error("session_sha256 must be a string", "invalid_session");
    }
    std::string digest = request.at("session_sha256").get<std::string>();
    if (!valid_session_digest(digest)) {
        throw_checkpoint_request_error("checkpoint session must be a lowercase SHA-256",
                                       "invalid_session");
    }
    return digest;
}

void write_checkpoint_delete_response(httplib::Response& response,
                                      SessionCheckpointEraseResult result) {
    const char* state = nullptr;
    switch (result) {
    case SessionCheckpointEraseResult::Erased:
        response.status = 200;
        state           = "deleted";
        break;
    case SessionCheckpointEraseResult::Missing:
        response.status = 404;
        state           = "missing";
        break;
    case SessionCheckpointEraseResult::Conflict:
        response.status = 409;
        state           = "conflict";
        break;
    }
    response.set_content(
        nlohmann::json{{"artifact_type", "ninfer_session_checkpoint_status"}, {"state", state}}
            .dump(),
        "application/json");
}

httplib::Server::HandlerResponse handle_unrendered_http_error(const ServeOptions& options,
                                                              const httplib::Request& request,
                                                              httplib::Response& response) {
    if (response.status != 413 || !response.body.empty()) {
        return httplib::Server::HandlerResponse::Unhandled;
    }

    ApiError error;
    error.status  = 413;
    error.type    = "invalid_request_error";
    error.code    = "request_too_large";
    error.message = "request body exceeds the configured payload limit of " +
                    std::to_string(options.max_request_bytes) + " bytes";
    if (request.path.rfind("/v1/messages", 0) == 0) {
        write_messages_error(response, error);
    } else {
        write_error(response, error);
    }
    return httplib::Server::HandlerResponse::Handled;
}

HttpServer::HttpServer(ServeOptions options)
    : options_(std::move(options)),
      response_store_(options_.response_store_max_records, options_.response_store_max_bytes),
      request_jsonl_(options_.request_log_jsonl, options_.artifact_path) {
    const std::size_t queued_requests =
        static_cast<std::size_t>(options_.max_concurrency) + options_.max_pending_requests;
    const std::size_t worker_count = queued_requests + 1;
    server_.new_task_queue         = [queued_requests, worker_count] {
        return new httplib::ThreadPool(worker_count, queued_requests);
    };
    server_.set_payload_max_length(options_.max_request_bytes);
    register_routes();
}

void HttpServer::log_line(const std::string& line) {
    write_console_log(ConsoleLogLevel::Info, line);
}

void HttpServer::log_request_start(const RequestLogContext& context) {
    log_line(format_request_start(context));
    request_jsonl_.write_request_start(context);
}

void HttpServer::log_request_rejected(const RequestRejectionLogContext& context) {
    log_line(format_request_rejected(context));
    request_jsonl_.write_request_rejected(context);
}

void HttpServer::log_request_done(const RequestLogContext& context,
                                  const GenerationOutcome& outcome) {
    log_line(format_request_done(context, outcome));
    request_jsonl_.write_request_done(context, outcome);
}

void HttpServer::log_request_error(const RequestLogContext& context, const std::string& message) {
    log_line(format_request_error(context, message));
    request_jsonl_.write_request_error(context, message);
}

void HttpServer::log_throughput(const ThroughputReport& report) {
    log_line(format_throughput(report));
    request_jsonl_.write_throughput(report);
}

void HttpServer::run_stats_reporter() {
    using Clock                     = std::chrono::steady_clock;
    ninfer::RuntimeStats previous   = service_->runtime_stats();
    Clock::time_point previous_time = Clock::now();
    const auto interval             = std::chrono::milliseconds(options_.log_stats_interval_ms);

    for (;;) {
        {
            std::unique_lock lock(stats_mutex_);
            if (stats_cv_.wait_for(lock, interval, [this] { return stats_stopping_; })) { break; }
        }

        const ninfer::RuntimeStats current = service_->runtime_stats();
        const Clock::time_point now        = Clock::now();
        const ThroughputReport report      = make_throughput_report(
            previous, current, std::chrono::duration<double>(now - previous_time).count());
        if (report_has_activity(report)) { log_throughput(report); }
        previous      = current;
        previous_time = now;
    }

    const ninfer::RuntimeStats current = service_->runtime_stats();
    const Clock::time_point now        = Clock::now();
    const ThroughputReport tail        = make_throughput_report(
        previous, current, std::chrono::duration<double>(now - previous_time).count());
    if (report_has_activity(tail)) { log_throughput(tail); }
}

void HttpServer::stop_stats_reporter() {
    if (!stats_thread_.joinable()) { return; }
    {
        std::lock_guard lock(stats_mutex_);
        stats_stopping_ = true;
    }
    stats_cv_.notify_one();
    stats_thread_.join();
}

void HttpServer::register_routes() {
    server_.set_error_handler([this](const httplib::Request& request, httplib::Response& response) {
        return handle_unrendered_http_error(options_, request, response);
    });
    if (options_.enable_cors) {
        server_.set_default_headers(
            {{"Access-Control-Allow-Origin", "*"},
             {"Access-Control-Allow-Headers", "Authorization, Content-Type, X-NInfer-Session"},
             {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"}});
        // CORS preflight: browsers send OPTIONS with no credentials before the real
        // request; answer it without auth so the actual GET/POST can carry the key.
        server_.Options(R"(.*)",
                        [](const httplib::Request&, httplib::Response& res) { res.status = 204; });
    }

    server_.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        if (options_.api_key.empty() || req.path == "/health" || req.method == "OPTIONS") {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        // Accept both the OpenAI-style bearer token and the Anthropic-style
        // x-api-key header so OpenAI clients and Claude Code (ANTHROPIC_API_KEY
        // -> x-api-key, ANTHROPIC_AUTH_TOKEN -> Authorization: Bearer) both work.
        // Credential equality is digest-based constant time (alphastorm/ninfer#22).
        const std::string& authorization = req.get_header_value("Authorization");
        constexpr std::string_view kBearerPrefix{"Bearer "};
        const bool bearer_ok =
            authorization.size() > kBearerPrefix.size() &&
            std::string_view(authorization).substr(0, kBearerPrefix.size()) == kBearerPrefix &&
            credential_equal(std::string_view(authorization).substr(kBearerPrefix.size()),
                             options_.api_key);
        const bool x_api_key_ok =
            credential_equal(req.get_header_value("x-api-key"), options_.api_key);
        if (!bearer_ok && !x_api_key_ok) {
            ApiError error;
            error.status  = 401;
            error.type    = "invalid_request_error";
            error.code    = "invalid_api_key";
            error.message = "missing or invalid API key";
            // Render the 401 in the shape the target endpoint speaks.
            if (req.path.rfind("/v1/messages", 0) == 0) {
                write_messages_error(res, error);
            } else {
                write_error(res, error);
            }
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server_.set_exception_handler(
        [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const ApiException& e) {
                write_error(res, e.error());
            } catch (const std::exception& e) { write_exception(res, e); } catch (...) {
                ApiError error;
                error.status  = 500;
                error.type    = "internal_error";
                error.message = "unknown error";
                write_error(res, error);
            }
        });

    server_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(nlohmann::json{{"status", "ok"}}.dump(), "application/json");
    });
    server_.Get("/v1/ninfer/status", [this](const httplib::Request& req, httplib::Response& res) {
        handle_status(req, res);
    });
    if (!options_.session_checkpoint_root.empty()) {
        server_.Post("/v1/ninfer/checkpoints",
                     [this](const httplib::Request& req, httplib::Response& res) {
                         handle_checkpoint_save(req, res);
                     });
        server_.Get("/v1/ninfer/checkpoints/status",
                    [this](const httplib::Request& req, httplib::Response& res) {
                        handle_checkpoint_get(req, res);
                    });
        server_.Get(R"(/v1/ninfer/checkpoints/([0-9a-f]{64})/status)",
                    [this](const httplib::Request& req, httplib::Response& res) {
                        handle_checkpoint_get(req, res);
                    });
        server_.Delete("/v1/ninfer/checkpoints",
                       [this](const httplib::Request& req, httplib::Response& res) {
                           handle_checkpoint_delete(req, res);
                       });
        server_.Delete(R"(/v1/ninfer/checkpoints/([0-9a-f]{64}))",
                       [this](const httplib::Request& req, httplib::Response& res) {
                           handle_checkpoint_delete(req, res);
                       });
    }
    server_.Get("/v1/models", [this](const httplib::Request& req, httplib::Response& res) {
        handle_models(req, res);
    });
    server_.Get(R"(/v1/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model(req, res);
    });
    server_.Post("/v1/chat/completions",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     handle_chat_completions(req, res);
                 });
    server_.Post("/v1/responses", [this](const httplib::Request& req, httplib::Response& res) {
        handle_responses(req, res);
    });
    server_.Post("/v1/responses/input_tokens",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     handle_response_input_tokens(req, res);
                 });
    server_.Post("/v1/responses/compact",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     handle_response_compact(req, res);
                 });
    server_.Post(R"(/v1/responses/([^/]+)/cancel)",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     handle_response_cancel(req, res);
                 });
    server_.Get(R"(/v1/responses/([^/]+)/input_items)",
                [this](const httplib::Request& req, httplib::Response& res) {
                    handle_response_input_items(req, res);
                });
    server_.Get(R"(/v1/responses/([^/]+))",
                [this](const httplib::Request& req, httplib::Response& res) {
                    handle_response_get(req, res);
                });
    server_.Delete(R"(/v1/responses/([^/]+))",
                   [this](const httplib::Request& req, httplib::Response& res) {
                       handle_response_delete(req, res);
                   });
    server_.Post("/v1/messages/count_tokens",
                 [this](const httplib::Request& req, httplib::Response& res) {
                     handle_count_tokens(req, res);
                 });
    server_.Post("/v1/messages", [this](const httplib::Request& req, httplib::Response& res) {
        handle_messages(req, res);
    });
}

void HttpServer::handle_checkpoint_get(const httplib::Request& req, httplib::Response& res) const {
    std::string digest;
    try {
        digest = require_checkpoint_session_identity(checkpoint_session_path_argument(req), req,
                                                     !options_.api_key.empty());
    } catch (const ApiException& error) {
        write_error(res, error.error());
        return;
    }
    if (service_ == nullptr) {
        res.status = 503;
        res.set_content(nlohmann::json{{"state", "loading"}}.dump(), "application/json");
        return;
    }
    res.set_content(service_->checkpoint_status(digest).dump(), "application/json");
}

void HttpServer::handle_checkpoint_save(const httplib::Request& req, httplib::Response& res) {
    std::string digest;
    try {
        digest = parse_checkpoint_save_request_body(req.body);
    } catch (const ApiException& error) {
        write_error(res, error.error());
        return;
    }
    if (service_ == nullptr) {
        ApiError error;
        error.status  = 503;
        error.code    = "service_unavailable";
        error.message = "generation service is loading";
        write_error(res, error);
        return;
    }
    runtime::SessionCheckpointSkipDetail skip;
    const std::optional<SessionCheckpointSaveResult> saved =
        service_->save_checkpoint(digest, response_store_, &skip);
    if (!saved) {
        // The HTTP body keeps its released closed vocabulary; the named gate and the
        // attempted response id go to the server log only (alphastorm/ninfer#31).
        try {
            std::string refusal = "checkpoint save refused: ";
            refusal += runtime::session_checkpoint_skip_reason_name(skip.reason);
            if (!skip.attempted_tag.empty()) {
                refusal += " (attempted ";
                refusal += skip.attempted_tag;
                refusal += ")";
            }
            write_console_log(ConsoleLogLevel::Warning, refusal);
        } catch (...) {}
        ApiError error;
        error.status  = 409;
        error.code    = "checkpoint_unavailable";
        error.message = "session has no complete checkpointable response";
        write_error(res, error);
        return;
    }
    res.set_content(nlohmann::json{{"artifact_type", "ninfer_session_checkpoint_status"},
                                   {"state", "available"},
                                   {"generation", saved->generation},
                                   {"bytes", saved->bytes},
                                   {"frontier_tokens", saved->engine.frontier_tokens},
                                   {"restored_tokens", saved->engine.restored_tokens}}
                        .dump(),
                    "application/json");
}

void HttpServer::handle_checkpoint_delete(const httplib::Request& req, httplib::Response& res) {
    std::string digest;
    try {
        digest = require_checkpoint_session_identity(checkpoint_session_path_argument(req), req,
                                                     !options_.api_key.empty());
    } catch (const ApiException& error) {
        write_error(res, error.error());
        return;
    }
    if (service_ == nullptr) {
        ApiError error;
        error.status  = 503;
        error.code    = "service_unavailable";
        error.message = "generation service is loading";
        write_error(res, error);
        return;
    }
    write_checkpoint_delete_response(res, service_->erase_checkpoint(digest));
}

void HttpServer::handle_models(const httplib::Request&, httplib::Response& res) const {
    res.set_content(make_models_list(public_model_id_, unix_time_now()), "application/json");
}

void HttpServer::handle_model(const httplib::Request& req, httplib::Response& res) const {
    const std::string id = req.matches.size() > 1 ? req.matches[1].str() : std::string();
    if (id != public_model_id_) {
        ApiError error;
        error.status  = 404;
        error.type    = "invalid_request_error";
        error.code    = "model_not_found";
        error.message = "model '" + id + "' not found";
        write_error(res, error);
        return;
    }
    res.set_content(make_model_object(public_model_id_, unix_time_now()), "application/json");
}

void HttpServer::handle_status(const httplib::Request&, httplib::Response& res) const {
    if (options_.api_key.empty()) {
        ApiError error;
        error.status  = 401;
        error.code    = "authentication_required";
        error.message = "server status requires API authentication";
        write_error(res, error);
        return;
    }
    if (service_ == nullptr) {
        res.status = 503;
        res.set_content(nlohmann::json{{"status", "loading"}}.dump(), "application/json");
        return;
    }
    res.set_content(format_status_json(options_, status_engine_options_, public_model_id_,
                                       status_load_, status_memory_, service_->runtime_stats()),
                    "application/json");
}

void HttpServer::handle_chat_completions(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
        ApiError error;
        error.status  = 400;
        error.message = "request body is not valid JSON";
        write_error(res, error);
        return;
    }

    GenerationRequest request;
    try {
        RequestLimits limits;
        limits.default_max_tokens = options_.default_max_tokens;
        request                   = parse_chat_completion_request(body, limits);
        if (request.model != public_model_id_) {
            ApiError error;
            error.status  = 404;
            error.type    = "invalid_request_error";
            error.code    = "model_not_found";
            error.message = "model '" + request.model + "' not found";
            throw ApiException(std::move(error));
        }
    } catch (const ApiException& e) {
        write_error(res, e.error());
        return;
    }

    const std::uint64_t req_id = ++request_seq_;
    PreparedRequest prepared;
    try {
        prepared = service_->prepare(
            request, [&req] { return req.is_connection_alive && !req.is_connection_alive(); });
    } catch (const ApiException& e) {
        log_request_rejected(make_request_rejection_log_context(req_id, "openai_chat_completions",
                                                                request, e.error()));
        write_error(res, e.error());
        return;
    } catch (const std::exception& e) {
        ApiError error;
        error.status  = 500;
        error.type    = "internal_error";
        error.message = e.what();
        log_request_rejected(
            make_request_rejection_log_context(req_id, "openai_chat_completions", request, error));
        write_error(res, error);
        return;
    }

    const std::string id       = new_chat_completion_id();
    const std::int64_t created = unix_time_now();
    const std::string model    = request.model;

    const RequestLogContext log_context =
        make_request_log_context(req_id, "openai_chat_completions", request, prepared);
    log_request_start(log_context);

    if (!request.stream) {
        try {
            const GenerationOutcome outcome = service_->run(prepared, nullptr, [&req] {
                return req.is_connection_alive && !req.is_connection_alive();
            });
            log_request_done(log_context, outcome);
            const CompletionUsage usage{outcome.prompt_tokens, outcome.completion_tokens};
            std::string response_body;
            if (!outcome.tool_calls.empty()) {
                response_body = make_chat_completion_tool_response(
                    id, model, created, outcome.text, outcome.reasoning, outcome.tool_calls, usage);
            } else {
                response_body = make_chat_completion_response(
                    id, model, created, outcome.text, outcome.reasoning,
                    finish_reason_wire(outcome.finish_reason), usage);
            }
            set_owned_content(res, std::move(response_body), prepared.lifetime);
        } catch (const std::exception& e) {
            log_request_error(log_context, e.what());
            throw;
        }
        return;
    }

    auto stream              = std::make_shared<StreamingRequest>(std::move(prepared));
    const bool include_usage = stream->prepared.include_usage;
    const bool tool_capable  = stream->prepared.tool_capable;

    // SSE hints: disable client/proxy caching and reverse-proxy response buffering
    // so tokens flush immediately. Content-Type is set by the chunked provider.
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");

    res.set_chunked_content_provider(
        "text/event-stream",
        [this, stream, id, created, model, include_usage, tool_capable,
         log_context](std::size_t, httplib::DataSink& sink) -> bool {
            if (stream->started) {
                sink.done();
                return true;
            }
            stream->started = true;
            try {
                write_stream_item(sink, *stream,
                                  make_chat_chunk_role(id, model, created, include_usage));
                StreamSink output;
                output.on_content = [&](const std::string& text) {
                    write_stream_item(
                        sink, *stream,
                        make_chat_chunk_content(id, model, created, text, include_usage));
                };
                output.on_reasoning = [&](const std::string& text) {
                    write_stream_item(
                        sink, *stream,
                        make_chat_chunk_reasoning(id, model, created, text, include_usage));
                };
                output.is_cancelled = [&] {
                    return stream->cancelled.load(std::memory_order_acquire) ||
                           (sink.is_writable && !sink.is_writable());
                };

                const GenerationOutcome outcome = service_->run(stream->prepared, &output);
                log_request_done(log_context, outcome);
                const std::string_view remaining = unstreamed_content(outcome);
                if (!outcome.tool_calls.empty()) {
                    if (!remaining.empty()) {
                        write_stream_item(sink, *stream,
                                          make_chat_chunk_content(id, model, created,
                                                                  std::string(remaining),
                                                                  include_usage));
                    }
                    write_stream_item(sink, *stream,
                                      make_chat_chunk_tool_calls(
                                          id, model, created, outcome.tool_calls, include_usage));
                    write_stream_item(
                        sink, *stream,
                        make_chat_chunk_final(id, model, created, "tool_calls", include_usage));
                } else {
                    if (tool_capable && !remaining.empty()) {
                        write_stream_item(sink, *stream,
                                          make_chat_chunk_content(id, model, created,
                                                                  std::string(remaining),
                                                                  include_usage));
                    }
                    write_stream_item(
                        sink, *stream,
                        make_chat_chunk_final(id, model, created,
                                              finish_reason_wire(outcome.finish_reason),
                                              include_usage));
                }
                if (include_usage) {
                    const CompletionUsage usage{outcome.prompt_tokens, outcome.completion_tokens};
                    write_stream_item(sink, *stream,
                                      make_chat_chunk_usage(id, model, created, usage));
                }
                write_stream_item(sink, *stream, sse_done());
                sink.done();
                return true;
            } catch (const ClientDisconnected& e) {
                log_request_error(log_context, e.what());
                return false;
            } catch (const ApiException& e) {
                log_request_error(log_context, e.error().message);
                try {
                    write_stream_item(sink, *stream, sse_error_event(e.error()));
                    sink.done();
                    return true;
                } catch (const ClientDisconnected&) { return false; }
            } catch (const std::exception& e) {
                log_request_error(log_context, e.what());
                ApiError error;
                error.status  = 500;
                error.type    = "internal_error";
                error.message = e.what();
                try {
                    write_stream_item(sink, *stream, sse_error_event(error));
                    sink.done();
                    return true;
                } catch (const ClientDisconnected&) { return false; }
            }
        },
        [stream](bool) { stream->cancelled.store(true, std::memory_order_release); });
}

void HttpServer::handle_count_tokens(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
        ApiError error;
        error.status  = 400;
        error.message = "request body is not valid JSON";
        write_messages_error(res, error);
        return;
    }
    try {
        RequestLimits limits;
        limits.default_max_tokens       = options_.default_max_tokens;
        const GenerationRequest request = parse_messages_request(body, limits);
        const int input_tokens          = service_->count_prompt_tokens(
            request, [&req] { return req.is_connection_alive && !req.is_connection_alive(); });
        res.set_content(make_count_tokens_response(input_tokens), "application/json");
    } catch (const ApiException& e) {
        write_messages_error(res, e.error());
    } catch (const std::exception& e) {
        ApiError error;
        error.status  = 500;
        error.type    = "internal_error";
        error.message = e.what();
        write_messages_error(res, error);
    }
}

void HttpServer::handle_messages(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
        ApiError error;
        error.status  = 400;
        error.message = "request body is not valid JSON";
        write_messages_error(res, error);
        return;
    }

    GenerationRequest request;
    try {
        RequestLimits limits;
        limits.default_max_tokens = options_.default_max_tokens;
        // The Anthropic endpoint accepts any `model` string (Claude Code sends real
        // Claude model names) and echoes it back; it never 404s on model id.
        request = parse_messages_request(body, limits);
    } catch (const ApiException& e) {
        write_messages_error(res, e.error());
        return;
    } catch (const std::exception& e) {
        ApiError error;
        error.status  = 500;
        error.type    = "internal_error";
        error.message = e.what();
        write_messages_error(res, error);
        return;
    }

    const std::uint64_t req_id = ++request_seq_;
    PreparedRequest prepared;
    try {
        prepared = service_->prepare(
            request, [&req] { return req.is_connection_alive && !req.is_connection_alive(); });
    } catch (const ApiException& e) {
        log_request_rejected(
            make_request_rejection_log_context(req_id, "anthropic_messages", request, e.error()));
        write_messages_error(res, e.error());
        return;
    } catch (const std::exception& e) {
        ApiError error;
        error.status  = 500;
        error.type    = "internal_error";
        error.message = e.what();
        log_request_rejected(
            make_request_rejection_log_context(req_id, "anthropic_messages", request, error));
        write_messages_error(res, error);
        return;
    }

    const std::string id    = new_message_id();
    const std::string model = request.model; // echo the requested model
    const int input_tokens  = prepared.prompt_tokens;

    const RequestLogContext log_context =
        make_request_log_context(req_id, "anthropic_messages", request, prepared);
    log_request_start(log_context);

    if (!request.stream) {
        try {
            const GenerationOutcome outcome = service_->run(prepared, nullptr, [&req] {
                return req.is_connection_alive && !req.is_connection_alive();
            });
            log_request_done(log_context, outcome);
            const CompletionUsage usage{outcome.prompt_tokens, outcome.completion_tokens};
            const char* stop_reason =
                messages_stop_reason(outcome.finish_reason, !outcome.tool_calls.empty());
            set_owned_content(res,
                              make_messages_response(id, model, outcome.text, outcome.reasoning,
                                                     outcome.tool_calls, stop_reason, usage),
                              prepared.lifetime);
        } catch (const ApiException& e) {
            log_request_error(log_context, e.error().message);
            write_messages_error(res, e.error());
        } catch (const std::exception& e) {
            log_request_error(log_context, e.what());
            ApiError error;
            error.status  = 500;
            error.type    = "internal_error";
            error.message = e.what();
            write_messages_error(res, error);
        }
        return;
    }

    auto stream             = std::make_shared<StreamingRequest>(std::move(prepared));
    const bool tool_capable = stream->prepared.tool_capable;

    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");

    res.set_chunked_content_provider(
        "text/event-stream",
        [this, stream, id, model, input_tokens, tool_capable,
         log_context](std::size_t, httplib::DataSink& sink) -> bool {
            if (stream->started) {
                sink.done();
                return true;
            }
            stream->started = true;

            int next_index     = 0;
            bool thinking_open = false;
            int thinking_index = -1;
            bool text_open     = false;
            int text_index     = -1;
            try {
                write_stream_item(sink, *stream, make_message_start(id, model, input_tokens));

                StreamSink output;
                output.on_reasoning = [&](const std::string& text) {
                    if (!thinking_open) {
                        thinking_index = next_index++;
                        thinking_open  = true;
                        write_stream_item(sink, *stream,
                                          make_content_block_start_thinking(thinking_index));
                    }
                    write_stream_item(sink, *stream,
                                      make_content_block_delta_thinking(thinking_index, text));
                };
                output.on_content = [&](const std::string& text) {
                    if (thinking_open) {
                        write_stream_item(sink, *stream, make_content_block_stop(thinking_index));
                        thinking_open = false;
                    }
                    if (!text_open) {
                        text_index = next_index++;
                        text_open  = true;
                        write_stream_item(sink, *stream, make_content_block_start_text(text_index));
                    }
                    write_stream_item(sink, *stream,
                                      make_content_block_delta_text(text_index, text));
                };
                output.is_cancelled = [&] {
                    return stream->cancelled.load(std::memory_order_acquire) ||
                           (sink.is_writable && !sink.is_writable());
                };

                const GenerationOutcome outcome = service_->run(stream->prepared, &output);
                log_request_done(log_context, outcome);
                const std::string_view remaining = unstreamed_content(outcome);

                if (thinking_open) {
                    write_stream_item(sink, *stream, make_content_block_stop(thinking_index));
                    thinking_open = false;
                }
                if (text_open) {
                    write_stream_item(sink, *stream, make_content_block_stop(text_index));
                    text_open = false;
                }

                if (tool_capable) {
                    if (!remaining.empty()) {
                        const int idx = next_index++;
                        write_stream_item(sink, *stream, make_content_block_start_text(idx));
                        write_stream_item(
                            sink, *stream,
                            make_content_block_delta_text(idx, std::string(remaining)));
                        write_stream_item(sink, *stream, make_content_block_stop(idx));
                    }
                    for (const ToolCall& call : outcome.tool_calls) {
                        const int idx = next_index++;
                        write_stream_item(sink, *stream,
                                          make_content_block_start_tool_use(idx, call));
                        write_stream_item(
                            sink, *stream,
                            make_content_block_delta_tool_json(idx, call.arguments_json));
                        write_stream_item(sink, *stream, make_content_block_stop(idx));
                    }
                }

                if (next_index == 0) {
                    const int idx = next_index++;
                    write_stream_item(sink, *stream, make_content_block_start_text(idx));
                    write_stream_item(sink, *stream, make_content_block_stop(idx));
                }

                const char* stop_reason =
                    messages_stop_reason(outcome.finish_reason, !outcome.tool_calls.empty());
                write_stream_item(sink, *stream,
                                  make_message_delta(stop_reason, outcome.completion_tokens));
                write_stream_item(sink, *stream, make_message_stop());
                sink.done();
                return true;
            } catch (const ClientDisconnected& e) {
                log_request_error(log_context, e.what());
                return false;
            } catch (const ApiException& e) {
                log_request_error(log_context, e.error().message);
                try {
                    write_stream_item(sink, *stream, messages_sse_error_event(e.error()));
                    sink.done();
                    return true;
                } catch (const ClientDisconnected&) { return false; }
            } catch (const std::exception& e) {
                log_request_error(log_context, e.what());
                ApiError error;
                error.status  = 500;
                error.type    = "internal_error";
                error.message = e.what();
                try {
                    write_stream_item(sink, *stream, messages_sse_error_event(error));
                    sink.done();
                    return true;
                } catch (const ClientDisconnected&) { return false; }
            }
        },
        [stream](bool) { stream->cancelled.store(true, std::memory_order_release); });
}

void HttpServer::maybe_checkpoint_completed_turn(const std::optional<std::string>& session_sha256,
                                                 const GenerationOutcome& outcome) noexcept {
    if (!automatic_checkpoints_ || !session_sha256) { return; }
    const std::uint64_t frontier =
        static_cast<std::uint64_t>(std::max(outcome.prompt_tokens, 0)) +
        static_cast<std::uint64_t>(std::max(outcome.completion_tokens, 0));
    if (frontier < options_.session_checkpoint_min_tokens) { return; }
    if (automatic_checkpoints_->enqueue(*session_sha256) ==
        AutomaticCheckpointEnqueueResult::Dropped) {
        try {
            write_console_log(ConsoleLogLevel::Warning,
                              "automatic session checkpoint queue dropped an acceleration save");
        } catch (...) {}
    }
}

void HttpServer::save_automatic_checkpoint(std::string_view session_sha256) noexcept {
    try {
        if (service_ != nullptr) {
            // Even with buffered export writes, staging briefly holds the engine. An automatic
            // save is pure acceleration, so yield to live traffic: wait for an idle engine
            // before starting, bounded so a busy server still checkpoints eventually
            // (ninfer#34). Explicit POST saves keep their synchronous contract and never wait.
            for (int waited = 0; waited < 120; ++waited) {
                const ninfer::RuntimeStats stats = service_->runtime_stats();
                const std::uint32_t busy =
                    stats.running_requests + stats.waiting_requests + stats.prefilling_requests +
                    stats.materializing_requests + stats.capture_pending_requests +
                    stats.terminal_pending_requests;
                if (busy == 0) { break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            runtime::SessionCheckpointSkipDetail skip;
            const bool saved =
                service_->save_checkpoint(session_sha256, response_store_, &skip).has_value();
            if (saved) {
                write_console_log(ConsoleLogLevel::Info, "automatic session checkpoint saved");
            } else {
                std::string message = "automatic session checkpoint skipped: ";
                message += runtime::session_checkpoint_skip_reason_name(skip.reason);
                if (!skip.attempted_tag.empty()) {
                    message += " (attempted ";
                    message += skip.attempted_tag;
                    message += ")";
                }
                if (skip.reason == runtime::SessionCheckpointSkipReason::TagMismatch) {
                    message += " (catalogued ";
                    message += skip.catalogued_tag.empty() ? "<empty>" : skip.catalogued_tag;
                    message += ")";
                }
                write_console_log(ConsoleLogLevel::Warning, message);
            }
        }
    } catch (const std::exception& exception) {
        try {
            write_console_log(ConsoleLogLevel::Warning,
                              std::string("session checkpoint save failed (continuing): ") +
                                  exception.what());
        } catch (...) {}
    }
}

void HttpServer::save_all_checkpoints() noexcept {
    if (service_ == nullptr || !service_->checkpoint_enabled()) { return; }
    if (automatic_checkpoints_) { automatic_checkpoints_->drain(); }
    try {
        for (const std::string& digest : response_store_.session_digests()) {
            try {
                (void)service_->save_checkpoint(digest, response_store_);
            } catch (const std::exception& exception) {
                write_console_log(ConsoleLogLevel::Warning,
                                  std::string("shutdown checkpoint save failed (continuing): ") +
                                      exception.what());
            }
        }
    } catch (...) {}
}

bool HttpServer::bind() { return server_.bind_to_port(options_.host, options_.port); }

void HttpServer::attach(GenerationService& service) {
    if (service_ != nullptr) {
        throw std::logic_error("HTTP generation service is already attached");
    }
    status_load_           = service.load_summary();
    status_engine_options_ = service.engine_options();
    status_memory_         = service.memory_summary();
    public_model_id_       = resolve_public_model_id(options_, status_load_.model_id);
    request_jsonl_.write_server_start(options_, status_engine_options_, service.sampling_defaults(),
                                      public_model_id_, status_load_, status_memory_);
    service_ = &service;
    if (service.checkpoint_enabled()) {
        automatic_checkpoints_ = std::make_unique<AutomaticCheckpointQueue>(
            [this](std::string_view digest) { save_automatic_checkpoint(digest); });
    }
}

bool HttpServer::listen() {
    if (service_ == nullptr) { throw std::logic_error("HTTP generation service is not attached"); }
    if (public_model_id_.empty()) {
        throw std::logic_error("HTTP public model id is not resolved");
    }
    if (options_.log_stats_interval_ms != 0) {
        stats_stopping_ = false;
        stats_thread_   = std::thread([this] { run_stats_reporter(); });
    }
    try {
        const bool result = server_.listen_after_bind();
        stop_stats_reporter();
        return result;
    } catch (...) {
        stop_stats_reporter();
        throw;
    }
}

void HttpServer::stop() { server_.stop(); }

} // namespace ninfer::serve
