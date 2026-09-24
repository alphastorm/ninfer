#include "serve/generation_service.h"

#include "core/sha256.h"

#include <fstream>

#include "product/media_acquire/acquire.h"
#include "runtime/engine/checkpoint_engine_access.h"
#include "serve/checkpoint_policy.h"
#include "serve/client_identity.h"
#include "serve/console_log.h"
#include "serve/tool_call_parser.h"
#include "serve/server_identity.h"
#include "serve/translate.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace ninfer::serve {

struct RequestCapacity {
    explicit RequestCapacity(std::size_t limit) : maximum(limit) {}

    std::mutex mutex;
    std::size_t active = 0;
    const std::size_t maximum;
};

struct RequestLifetime {
    RequestLifetime(std::shared_ptr<RequestCapacity> owner,
                    std::chrono::steady_clock::time_point begin,
                    std::chrono::steady_clock::time_point limit)
        : capacity(std::move(owner)), started(begin), deadline(limit) {}

    ~RequestLifetime() {
        std::lock_guard lock(capacity->mutex);
        --capacity->active;
    }

    std::shared_ptr<RequestCapacity> capacity;
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point deadline;
};

ApiError request_error_to_api_error(const ninfer::RequestError& exception) {
    ApiError error;
    error.param   = "messages";
    error.message = exception.what();
    switch (exception.kind()) {
    case ninfer::RequestErrorKind::ContextLengthExceeded:
        error.status = 400;
        error.code   = "context_length_exceeded";
        break;
    case ninfer::RequestErrorKind::ThinkingBudgetCapacityInsufficient:
        error.param.clear();
        error.status = 400;
        error.code   = "thinking_budget_capacity_insufficient";
        break;
    case ninfer::RequestErrorKind::MediaBudgetExceeded:
        error.status = 400;
        error.code   = "media_budget_exceeded";
        break;
    case ninfer::RequestErrorKind::Overloaded:
        error.param.clear();
        error.status = 429;
        error.type   = "rate_limit_error";
        error.code   = "server_overloaded";
        break;
    case ninfer::RequestErrorKind::QueueTimeout:
        error.param.clear();
        error.status = 503;
        error.type   = "server_error";
        error.code   = "request_queue_timeout";
        break;
    case ninfer::RequestErrorKind::Cancelled:
        error.status = 499;
        error.type   = "request_cancelled";
        error.code   = "client_disconnected";
        break;
    case ninfer::RequestErrorKind::Unavailable:
        error.param.clear();
        error.status = 503;
        error.type   = "server_error";
        error.code   = "service_unavailable";
        break;
    }
    return error;
}

namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void throw_preparation_cancelled();

[[noreturn]] void throw_media_error(const ninfer::product::media_acquire::Error& exception) {
    ApiError error;
    error.param   = "messages";
    error.message = exception.what();
    switch (exception.kind()) {
    case ninfer::product::media_acquire::ErrorKind::BudgetExceeded:
        error.status = 400;
        error.code   = "media_budget_exceeded";
        break;
    case ninfer::product::media_acquire::ErrorKind::RemoteUnavailable:
        error.status = 502;
        error.type   = "server_error";
        error.code   = "media_fetch_failed";
        break;
    case ninfer::product::media_acquire::ErrorKind::RemoteTimeout:
        error.status = 504;
        error.type   = "server_error";
        error.code   = "media_fetch_timeout";
        break;
    case ninfer::product::media_acquire::ErrorKind::DeadlineExceeded:
        error.status = 503;
        error.type   = "server_error";
        error.code   = "request_queue_timeout";
        break;
    case ninfer::product::media_acquire::ErrorKind::Cancelled:
        throw_preparation_cancelled();
    }
    throw ApiException(std::move(error));
}

[[noreturn]] void throw_invalid_input(const std::exception& exception,
                                      const char* code = "invalid_media") {
    ApiError error;
    error.status  = 400;
    error.param   = "messages";
    error.code    = code;
    error.message = exception.what();
    throw ApiException(std::move(error));
}

[[noreturn]] void throw_preparation_cancelled() {
    ApiError error;
    error.status  = 499;
    error.type    = "request_cancelled";
    error.code    = "client_disconnected";
    error.message = "client disconnected during media preparation";
    throw ApiException(std::move(error));
}

ninfer::OwnedMedia acquire_media(const ContentPart& part, Clock::time_point deadline,
                                 const std::function<bool()>& is_cancelled,
                                 std::size_t& remaining_bytes) {
    if (remaining_bytes == 0) {
        throw_media_error(ninfer::product::media_acquire::Error(
            ninfer::product::media_acquire::ErrorKind::BudgetExceeded,
            "request media exceeds aggregate byte limit"));
    }
    ninfer::product::media_acquire::Policy policy;
    policy.max_bytes    = std::min(policy.max_bytes, remaining_bytes);
    policy.deadline     = deadline;
    policy.is_cancelled = is_cancelled;
    std::vector<std::uint8_t> source_bytes;
    try {
        source_bytes = ninfer::product::media_acquire::acquire_bytes(part.source, policy);
    } catch (const ninfer::product::media_acquire::Error& exception) {
        throw_media_error(exception);
    } catch (const std::invalid_argument& exception) { throw_invalid_input(exception); }

    remaining_bytes -= source_bytes.size();
    ninfer::OwnedMedia media;
    media.kind =
        part.kind == ContentKind::Image ? ninfer::MediaKind::Image : ninfer::MediaKind::Video;
    media.media_type = part.source.media_type;
    switch (part.source.kind) {
    case ninfer::product::media_acquire::SourceKind::Path:
    case ninfer::product::media_acquire::SourceKind::Url:
        media.source_name = part.source.value;
        break;
    case ninfer::product::media_acquire::SourceKind::Data:
        media.source_name = "inline-data";
        break;
    case ninfer::product::media_acquire::SourceKind::Bytes:
        media.source_name = "inline-bytes";
        break;
    }
    media.bytes = std::move(source_bytes);
    return media;
}

[[noreturn]] void throw_request_error(const ninfer::RequestError& exception) {
    throw ApiException(request_error_to_api_error(exception));
}

void check_preparation_control(Clock::time_point deadline,
                               const std::function<bool()>& is_cancelled) {
    if (is_cancelled && is_cancelled()) { throw_preparation_cancelled(); }
    if (Clock::now() >= deadline) {
        throw_request_error(ninfer::RequestError(RequestErrorKind::QueueTimeout,
                                                 "inference request expired during preparation"));
    }
}

class ServiceOutputSink final : public ninfer::OutputSink {
public:
    ServiceOutputSink(const StreamSink& sink, bool filter_tool_calls)
        : sink_(&sink), filter_tool_calls_(filter_tool_calls) {}

    void publish(ninfer::OutputDelta delta) override {
        if (delta.text.empty()) { return; }
        if (delta.channel == ninfer::OutputChannel::Reasoning) {
            if (sink_->on_reasoning) { sink_->on_reasoning(delta.text); }
        } else {
            std::string visible =
                filter_tool_calls_ ? tool_filter_.feed(delta.text) : std::move(delta.text);
            publish_content(visible);
        }
    }

    std::size_t finish(bool is_tool_call_response) {
        if (filter_tool_calls_) { publish_content(tool_filter_.finish(is_tool_call_response)); }
        return content_bytes_;
    }

private:
    void publish_content(const std::string& text) {
        if (text.empty() || !sink_->on_content) { return; }
        sink_->on_content(text);
        content_bytes_ += text.size();
    }

    const StreamSink* sink_ = nullptr;
    bool filter_tool_calls_ = false;
    ToolCallStreamFilter tool_filter_;
    std::size_t content_bytes_ = 0;
};

} // namespace

namespace {

// Lets a restore that has run out of shared engine capacity drop other live sessions instead of
// refusing, but only the ones this store can already reproduce: the current generation must be
// fingerprint-compatible and already hold the tag the engine is carrying. Anything unprovable -
// including a store that throws - is kept, so reclaim can cost latency but never context.
//
// A live session that has taken a turn since its last save is exactly that unprovable case, and
// it is the common one: the caller collects those names so it can bring them up to date and try
// again. If saving or pinning fails, the resident continuation remains untouched.
class StoredCheckpointOracle final : public runtime::ReclaimableSessionOracle {
public:
    struct Candidate {
        std::string session_sha256;
        std::string checkpoint_tag;
    };

    StoredCheckpointOracle(SessionCheckpointStore& store,
                           const nlohmann::json& fingerprint) noexcept
        : store_(store), fingerprint_(fingerprint) {}

    void begin_attempt() noexcept { stale_.clear(); }

    [[nodiscard]] bool recoverable(std::string_view session_sha256,
                                   std::string_view checkpoint_tag) const noexcept override {
        try {
            const auto matches = [&](const Candidate& candidate) {
                return candidate.session_sha256 == session_sha256 &&
                       candidate.checkpoint_tag == checkpoint_tag;
            };
            if (std::any_of(pinned_.begin(), pinned_.end(),
                            [&](const PinnedCheckpoint& pin) { return matches(pin.identity); })) {
                return true;
            }
            // load verifies the response snapshot and payload inventory. Retaining its reader
            // pins the generation against quota eviction until the entire restore finishes.
            auto loaded = store_.load(session_sha256, fingerprint_, checkpoint_tag);
            if (loaded.checkpoint &&
                loaded.checkpoint->responses.latest_response_id == checkpoint_tag) {
                auto& checkpoint = *loaded.checkpoint;
                pinned_.push_back({{std::move(checkpoint.responses.client_session_sha256),
                                    std::move(checkpoint.responses.latest_response_id)},
                                   std::move(checkpoint.engine)});
                return true;
            }
            if (std::none_of(stale_.begin(), stale_.end(), matches)) {
                stale_.push_back({std::string(session_sha256), std::string(checkpoint_tag)});
            }
        } catch (...) {}
        return false;
    }

    [[nodiscard]] const std::vector<Candidate>& stale() const noexcept { return stale_; }

private:
    struct PinnedCheckpoint {
        Candidate identity;
        std::shared_ptr<const runtime::ContinuationCheckpointReader> reader;
    };

    SessionCheckpointStore& store_;
    const nlohmann::json& fingerprint_;
    mutable std::vector<Candidate> stale_;
    mutable std::vector<PinnedCheckpoint> pinned_;
};

// Saves each live session the engine is about to drop under pressure, so the drop costs cache
// rather than the session's newest turn (alphastorm/omp-ninfer#45). It runs on the engine worker
// with the execution lock released, and every save takes the ordinary checkpoint path.
class PressureCheckpointSaver final : public runtime::PressureCheckpointHandler {
public:
    PressureCheckpointSaver(GenerationService& service, ResponseStore& responses) noexcept
        : service_(service), responses_(responses) {}

    void save_before_eviction(
        std::span<runtime::PressureCheckpointVictim> victims) noexcept override {
        std::vector<Deferral> deferred;
        for (runtime::PressureCheckpointVictim& victim : victims) {
            victim.outcome = Outcome::Settled;
            try {
                Deferral deferral = take_deferral(victim);
                victim.outcome    = save(victim, deferral);
                if (victim.outcome == Outcome::Pending) { deferred.push_back(std::move(deferral)); }
            } catch (const std::exception& error) {
                try {
                    write_console_log(ConsoleLogLevel::Warning,
                                      "session checkpoint before eviction failed for session " +
                                          victim.session_sha256.substr(0, 12) + ": " +
                                          error.what());
                } catch (...) {}
            } catch (...) {}
        }
        deferred_ = std::move(deferred);
    }

private:
    using Outcome = runtime::PressureCheckpointOutcome;

    // A victim answered Pending by the previous call, and what has been reported about it. Only
    // the victims of one call are carried into the next, so this never outgrows a single plan.
    struct Deferral {
        std::string session_sha256;
        std::string checkpoint_tag;
        unsigned transient_refusals = 0;
        bool publication_reported   = false;
    };

    // A reply still being stored - a streamed one is stored only after its last delta reaches the
    // client - is awaited this long per call; after that the victim stays resident until the next
    // admission pass asks again, and the waiting request's own deadline bounds the whole wait.
    static constexpr std::chrono::milliseconds kPublicationWait{1000};

    Deferral take_deferral(const runtime::PressureCheckpointVictim& victim) {
        for (Deferral& deferral : deferred_) {
            if (deferral.session_sha256 == victim.session_sha256 &&
                deferral.checkpoint_tag == victim.checkpoint_tag) {
                return std::move(deferral);
            }
        }
        return Deferral{.session_sha256 = victim.session_sha256,
                        .checkpoint_tag = victim.checkpoint_tag};
    }

    Outcome save(const runtime::PressureCheckpointVictim& victim, Deferral& deferral) {
        const std::string session = victim.session_sha256.substr(0, 12);
        if (service_.checkpoint_covers(victim.session_sha256, victim.checkpoint_tag)) {
            return Outcome::Settled;
        }
        switch (responses_.await_publication(victim.session_sha256, victim.checkpoint_tag,
                                             kPublicationWait)) {
        case ResponsePublicationState::Stored:
            break;
        case ResponsePublicationState::Publishing:
            if (!deferral.publication_reported) {
                write_console_log(ConsoleLogLevel::Info,
                                  "session checkpoint before eviction deferred: newest turn is "
                                  "still being stored (session " +
                                      session + ")");
                deferral.publication_reported = true;
            }
            return Outcome::Pending;
        case ResponsePublicationState::Absent:
            write_console_log(ConsoleLogLevel::Warning,
                              "session checkpoint before eviction skipped: newest turn is not the "
                              "session's newest stored response (session " +
                                  session + ")");
            return Outcome::Settled;
        }
        runtime::SessionCheckpointSkipDetail skip;
        if (service_.save_checkpoint(victim.session_sha256, responses_, &skip)) {
            write_console_log(ConsoleLogLevel::Info,
                              "session checkpoint saved before eviction (session " + session + ")");
            return Outcome::Settled;
        }
        // A gate that clears once the engine quiesces keeps the victim resident for another
        // admission pass, a bounded number of times; any other refusal is final.
        const bool retry = checkpoint_refusal_is_transient(skip) &&
                           deferral.transient_refusals < kTransientCheckpointRetries;
        if (retry) { ++deferral.transient_refusals; }
        write_console_log(ConsoleLogLevel::Warning,
                          format_session_checkpoint_skip(
                              retry ? "session checkpoint before eviction deferred: "
                                    : "session checkpoint before eviction refused: ",
                              victim.session_sha256, skip)
                              .view());
        return retry ? Outcome::Pending : Outcome::Settled;
    }

    GenerationService& service_;
    ResponseStore& responses_;
    std::vector<Deferral> deferred_;
};

// The lifecycle passes the artifact digest it hashed at deployment time. The serve
// re-hashes the bytes it will actually load and refuses a mismatch, so a mutated
// bind-mount source cannot restart under a stale identity and accept checkpoints
// fingerprinted for a different model (review CR-20260830 R3).
void verify_declared_artifact_digest(const std::string& artifact_path,
                                     const std::string& declared_sha256) {
    if (declared_sha256.empty()) { return; }
    std::ifstream input(artifact_path, std::ios::binary);
    if (!input) {
        throw std::invalid_argument("model artifact is unreadable for identity verification: " +
                                    artifact_path);
    }
    ninfer::crypto::Sha256 hasher;
    std::vector<std::byte> buffer(1 << 20);
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = input.gcount();
        if (got > 0) {
            hasher.update(std::span<const std::byte>(buffer.data(),
                                                     static_cast<std::size_t>(got)));
        }
    }
    if (input.bad()) {
        throw std::invalid_argument("model artifact read failed during identity verification: " +
                                    artifact_path);
    }
    const std::string actual = ninfer::crypto::sha256_hex(hasher.finish());
    if (actual != declared_sha256) {
        throw std::invalid_argument(
            "model artifact bytes do not match the declared model_artifact_sha256 identity");
    }
}

} // namespace

GenerationService::GenerationService(ServeOptions options, LoadProgress load_progress)
    : options_(std::move(options)) {
    verify_declared_artifact_digest(options_.artifact_path, options_.artifact_sha256);
    ninfer::EngineOptions engine_options;
    engine_options.artifact_path            = options_.artifact_path;
    engine_options.device                   = options_.device;
    engine_options.max_context              = options_.max_context;
    engine_options.kv_capacity              = options_.kv_capacity;
    engine_options.max_concurrency          = options_.max_concurrency;
    engine_options.max_pending_requests     = options_.max_pending_requests;
    engine_options.pending_timeout_ms       = options_.pending_timeout_ms;
    engine_options.prefill_chunk            = options_.prefill_chunk;
    engine_options.kv_cache                 = options_.kv_cache;
    engine_options.enable_vision            = options_.enable_vision;
    engine_options.use_cuda_graph           = options_.use_cuda_graph;
    engine_options.speculative              = options_.speculative;
    engine_options.context_cache            = options_.context_cache;
    engine_options.context_cost.preset_path = options_.context_cost_presets;
    engine_options.media_cache_bytes        = options_.media_cache_bytes;
    engine_options.media_live_bytes         = options_.media_live_bytes;
    engine_options.media_preprocess_threads = options_.media_preprocess_threads;
    engine_options.load_progress            = std::move(load_progress);
    engine_              = std::make_unique<ninfer::Engine>(std::move(engine_options));
    prompt_capabilities_ = engine_->prompt_capabilities();
    request_capacity_    = std::make_shared<RequestCapacity>(
        static_cast<std::size_t>(options_.max_concurrency) + options_.max_pending_requests);
    if (!options_.session_checkpoint_root.empty()) {
        std::shared_ptr<runtime::ContinuationCheckpointReadQueue> read_queue =
            runtime::CheckpointEngineAccess::make_read_queue(*engine_,
                                                             options_.session_checkpoint_root);
        if (!read_queue) {
            throw std::invalid_argument(
                "session checkpoints require native Windows DirectStorage or Linux io_uring");
        }
        if (!read_queue->available()) {
            throw std::invalid_argument("session checkpoint read backend is unavailable: " +
                                        std::string(read_queue->unavailable_reason()));
        }
        checkpoint_runtime_fingerprint_ = session_checkpoint_runtime_fingerprint(
            options_, engine_->options(), engine_->load_summary());
        if (options_.api_key.empty()) {
            // parse_serve_options enforces this for the CLI; direct construction must not
            // reach a publicly computable HMAC(empty, domain) origin key (council
            // CR-20260831-originauth U2).
            throw std::invalid_argument("session checkpoints require a configured API key");
        }
        // The manifest origin MAC key derives from the bearer key through a fixed domain
        // separator - the bearer key itself never touches the checkpoint machinery, and a
        // writer inside the checkpoint root cannot recreate the key (alphastorm/ninfer#32).
        // parse_serve_options guarantees an API key whenever checkpoints are enabled.
        static constexpr std::string_view kOriginDomain =
            "ninfer-checkpoint-manifest-origin-v1";
        const crypto::Sha256Digest origin_key = crypto::hmac_sha256(
            std::as_bytes(std::span(options_.api_key.data(), options_.api_key.size())),
            std::as_bytes(std::span(kOriginDomain.data(), kOriginDomain.size())));
        checkpoint_store_ = std::make_unique<SessionCheckpointStore>(SessionCheckpointStoreOptions{
            .root             = options_.session_checkpoint_root,
            .disk_quota_bytes = options_.session_checkpoint_quota_bytes,
            .staging_bytes      = options_.session_checkpoint_staging_bytes,
            .write_buffer_bytes = options_.session_checkpoint_write_buffer_bytes,
            .origin_mac_key = std::string(reinterpret_cast<const char*>(origin_key.data()),
                                          origin_key.size()),
            .require_origin_auth = options_.session_checkpoint_require_origin_auth,
            .read_queue       = std::move(read_queue),
        });
    }
}

GenerationService::~GenerationService() {
    // The engine worker may be inside the pressure handler, saving through this service's store
    // and mutex, and those are destroyed before the engine. Detach it, waiting that call out.
    if (engine_ == nullptr) { return; }
    try {
        runtime::CheckpointEngineAccess::set_pressure_checkpoint_handler(*engine_, nullptr);
    } catch (...) {}
}

void GenerationService::save_sessions_before_eviction(ResponseStore& responses) {
    if (!checkpoint_store_) { return; }
    runtime::CheckpointEngineAccess::set_pressure_checkpoint_handler(
        *engine_, std::make_shared<PressureCheckpointSaver>(*this, responses));
}

std::shared_ptr<RequestLifetime>
GenerationService::acquire_request_lifetime(DeadlinePolicy deadline_policy) const {
    const auto started = Clock::now();
    {
        std::lock_guard lock(request_capacity_->mutex);
        if (request_capacity_->active >= request_capacity_->maximum) {
            throw_request_error(ninfer::RequestError(RequestErrorKind::Overloaded,
                                                     "inference request queue is full"));
        }
        ++request_capacity_->active;
    }
    try {
        const Clock::time_point deadline =
            deadline_policy == DeadlinePolicy::UnboundedStartup
                ? Clock::time_point::max()
                : started + std::chrono::milliseconds(options_.pending_timeout_ms);
        return std::make_shared<RequestLifetime>(request_capacity_, started, deadline);
    } catch (...) {
        std::lock_guard lock(request_capacity_->mutex);
        --request_capacity_->active;
        throw;
    }
}

PreparedRequest GenerationService::prepare(const GenerationRequest& request,
                                           std::function<bool()> is_cancelled,
                                           ContextCacheHints context_cache,
                                           std::string checkpoint_tag) const {
    return prepare_impl(request, std::move(is_cancelled), std::move(context_cache),
                        options_.allow_prefix_reuse ? CacheParticipation::ReadWrite
                                                    : CacheParticipation::Disabled,
                        DeadlinePolicy::ClientPendingTimeout, std::move(checkpoint_tag));
}

PreparedRequest GenerationService::prepare_impl(const GenerationRequest& request,
                                                std::function<bool()> is_cancelled,
                                                ContextCacheHints context_cache,
                                                CacheParticipation cache_participation,
                                                DeadlinePolicy deadline_policy,
                                                std::string checkpoint_tag) const {
    apply_client_identity_cache_hints(request, !options_.api_key.empty(), context_cache);

    PreparedRequest prepared;
    prepared.include_usage        = request.include_usage;
    prepared.tool_capable         = request.uses_tools() || request.has_tool_history();
    prepared.tool_name_max_length = request.tool_name_max_length;
    prepared.tool_argument_types  = build_tool_argument_type_contracts(request);
    const ResolvedPromptSemantics semantics =
        resolve_prompt_semantics(request, options_, prompt_capabilities_);
    ninfer::RequestOptions request_options = to_request_options(
        request, options_, semantics, cache_participation == CacheParticipation::ReadWrite);
    prepared.enable_thinking                   = semantics.enable_thinking;
    prepared.thinking_budget                   = request_options.execution.thinking.budget;
    prepared.preserve_thinking                 = semantics.preserve_thinking;
    prepared.preserve_thinking_semantic_change = request.preserve_thinking_semantic_change;
    const bool request_has_media               = request.media_item_count() != 0;
    if (request_has_media && !options_.enable_vision) {
        const std::invalid_argument error("Vision is disabled for this server");
        throw_invalid_input(error, "vision_disabled");
    }
    prepared.lifetime = acquire_request_lifetime(deadline_policy);

    try {
        const auto acquisition_started = Clock::now();
        std::size_t remaining_media_bytes =
            std::min(options_.max_request_bytes, ninfer::kMaximumPromptMediaBytes);
        ninfer::PromptInput input =
            to_prompt_input(request, semantics, [&](const ContentPart& part) {
                return acquire_media(part, prepared.lifetime->deadline, is_cancelled,
                                     remaining_media_bytes);
            });
        std::vector<PromptCacheMarker> protocol_markers = std::move(input.context_cache.markers);
        input.context_cache                             = std::move(context_cache);
        input.context_cache.markers.insert(input.context_cache.markers.end(),
                                           std::make_move_iterator(protocol_markers.begin()),
                                           std::make_move_iterator(protocol_markers.end()));
        prepared.acquisition_seconds =
            std::chrono::duration<double>(Clock::now() - acquisition_started).count();
        check_preparation_control(prepared.lifetime->deadline, is_cancelled);
        const PreparationControl control{
            .deadline     = prepared.lifetime->deadline,
            .cancellation = CancellationView(is_cancelled),
        };
        ninfer::PreparedPrompt prompt = engine_->prepare(std::move(input), control);
        if (!checkpoint_tag.empty()) {
            runtime::CheckpointEngineAccess::set_checkpoint_tag(prompt, std::move(checkpoint_tag));
        }
        check_preparation_control(prepared.lifetime->deadline, is_cancelled);
        prepared.prompt_tokens = static_cast<int>(prompt.summary().prompt_tokens);
        prepared.preparation   = prompt.preparation_stats();
        prepared.prepare_seconds =
            std::chrono::duration<double>(Clock::now() - prepared.lifetime->started).count();
        prepared.generation =
            engine_->submit(std::move(prompt), std::move(request_options),
                            request.stream ? ninfer::OutputConsumerMode::Streaming
                                           : ninfer::OutputConsumerMode::Aggregate,
                            prepared.lifetime->deadline);
        prepared.sampling = prepared.generation.resolved_sampling();
    } catch (const ApiException&) { throw; } catch (const ninfer::RequestError& exception) {
        throw_request_error(exception);
    } catch (const std::invalid_argument& exception) { throw_invalid_input(exception); }
    return prepared;
}

int GenerationService::count_prompt_tokens(const GenerationRequest& request,
                                           std::function<bool()> is_cancelled) const {
    require_authenticated_client_identity(request, !options_.api_key.empty());
    const bool request_has_media = request.media_item_count() != 0;
    if (request_has_media && !options_.enable_vision) {
        const std::invalid_argument error("Vision is disabled for this server");
        throw_invalid_input(error, "vision_disabled");
    }
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(options_.pending_timeout_ms);
    const ResolvedPromptSemantics semantics =
        resolve_prompt_semantics(request, options_, prompt_capabilities_);
    try {
        std::size_t remaining_media_bytes =
            std::min(options_.max_request_bytes, ninfer::kMaximumPromptMediaBytes);
        ninfer::PromptInput input =
            to_prompt_input(request, semantics, [&](const ContentPart& part) {
                return acquire_media(part, deadline, is_cancelled, remaining_media_bytes);
            });
        check_preparation_control(deadline, is_cancelled);
        const PreparationControl control{
            .deadline     = deadline,
            .cancellation = CancellationView(is_cancelled),
        };
        const int prompt_tokens =
            static_cast<int>(engine_->count_tokens(std::move(input), control));
        check_preparation_control(deadline, is_cancelled);
        return prompt_tokens;
    } catch (const ApiException&) { throw; } catch (const ninfer::RequestError& exception) {
        throw_request_error(exception);
    } catch (const std::invalid_argument& exception) { throw_invalid_input(exception); }
}

GenerationOutcome GenerationService::run(PreparedRequest& prepared, const StreamSink* sink,
                                         std::function<bool()> is_cancelled) {
    std::unique_ptr<ServiceOutputSink> output_sink;
    if (sink != nullptr) {
        output_sink = std::make_unique<ServiceOutputSink>(*sink, prepared.tool_capable);
    }
    ninfer::OutputSink* public_sink = output_sink.get();
    ninfer::CancellationView cancellation;
    if (is_cancelled || (sink != nullptr && sink->is_cancelled)) {
        cancellation = ninfer::CancellationView([external = std::move(is_cancelled), sink]() {
            return (external && external()) ||
                   (sink != nullptr && sink->is_cancelled && sink->is_cancelled());
        });
    }

    ninfer::GenerationResult result;
    try {
        result = prepared.generation.wait(public_sink, cancellation);
    } catch (const ninfer::RequestError& exception) { throw_request_error(exception); }
    GenerationOutcome outcome;
    outcome.text              = std::move(result.content);
    outcome.reasoning         = std::move(result.reasoning);
    outcome.prompt_tokens     = static_cast<int>(result.prompt.prompt_tokens);
    outcome.completion_tokens = static_cast<int>(result.generated_token_ids.size());
    outcome.reasoning_tokens  = static_cast<int>(result.reasoning_tokens);
    outcome.thinking          = result.thinking;
    outcome.finish_reason     = result.finish_reason;

    outcome.metrics.prepare_seconds = prepared.prepare_seconds;
    outcome.metrics.ttft_seconds =
        prepared.prepare_seconds +
        std::max(0.0, result.timings.first_token_seconds - result.timings.prepare_seconds);
    outcome.metrics.vision_seconds  = result.timings.vision_seconds;
    outcome.metrics.prefill_seconds = result.timings.prefill_seconds;
    outcome.metrics.decode_seconds  = result.timings.decode_seconds;
    outcome.metrics.total_seconds =
        prepared.prepare_seconds +
        std::max(0.0, result.timings.total_seconds - result.timings.prepare_seconds);
    outcome.metrics.engine_timing               = result.engine_timing;
    outcome.metrics.prefix_cache_hit_tokens     = result.reused_prompt_tokens;
    outcome.metrics.prefix_reuse_path           = result.prefix_reuse_path;
    outcome.metrics.materialization             = result.materialization;
    outcome.metrics.speculative_backend         = result.speculative.backend;
    outcome.metrics.speculative_draft_window    = result.speculative.draft_window;
    outcome.metrics.speculative_rounds          = result.speculative.rounds;
    outcome.metrics.speculative_draft_tokens    = result.speculative.drafted_tokens;
    outcome.metrics.speculative_accepted_tokens = result.speculative.accepted_tokens;
    outcome.metrics.speculative_fallback_steps  = result.speculative.fallback_steps;
    outcome.metrics.speculative_accepted_per_position =
        std::move(result.speculative.accepted_per_position);

    bool is_tool_call_response = false;
    if (prepared.tool_capable) {
        ParsedToolCallOutput parsed = parse_qwen_tool_call_output(
            outcome.text, prepared.tool_name_max_length, prepared.tool_argument_types);
        outcome.text          = std::move(parsed.content);
        is_tool_call_response = parsed.is_tool_call_response;
        if (is_tool_call_response) { outcome.tool_calls = std::move(parsed.tool_calls); }
    }
    if (output_sink) {
        outcome.streamed_content_bytes = output_sink->finish(is_tool_call_response);
    }
    return outcome;
}

bool GenerationService::checkpoint_enabled() const noexcept { return checkpoint_store_ != nullptr; }

std::optional<SessionCheckpointSaveResult>
GenerationService::save_checkpoint(std::string_view session_sha256, ResponseStore& responses,
                                   runtime::SessionCheckpointSkipDetail* skip) {
    if (!checkpoint_store_) {
        if (skip != nullptr) {
            skip->reason = runtime::SessionCheckpointSkipReason::StoreDisabled;
        }
        return std::nullopt;
    }
    std::lock_guard lock(checkpoint_mutex_);
    return save_checkpoint_locked(session_sha256, responses, skip);
}

// Caller holds checkpoint_mutex_. Restore needs this to bring a session's stored copy up to date
// before the engine may drop it, and re-taking the mutex there would deadlock.
std::optional<SessionCheckpointSaveResult>
GenerationService::save_checkpoint_locked(std::string_view session_sha256, ResponseStore& responses,
                                          runtime::SessionCheckpointSkipDetail* skip) {
    std::optional<ResponseStoreSnapshot> snapshot = responses.snapshot_session(session_sha256);
    if (!snapshot) {
        if (skip != nullptr) {
            skip->reason = runtime::SessionCheckpointSkipReason::NoSessionRecords;
        }
        return std::nullopt;
    }
    const std::string checkpoint_tag = snapshot->latest_response_id;
    if (skip != nullptr) {
        runtime::SessionCheckpointSkipDetail::assign_tag(skip->attempted_tag, checkpoint_tag);
    }
    return checkpoint_store_->save(*snapshot, checkpoint_runtime_fingerprint_,
                                   [&](runtime::ContinuationCheckpointWriter& writer) {
                                       return runtime::CheckpointEngineAccess::checkpoint_session(
                                           *engine_, session_sha256, checkpoint_tag, writer,
                                           checkpoint_store_->options().staging_bytes, skip);
                                   },
                                   skip);
}

bool GenerationService::restore_checkpoint(std::string_view session_sha256,
                                           std::optional<std::string_view> required_response_id,
                                           ResponseStore& responses) {
    if (!checkpoint_store_) { return false; }
    // A restore without a response id is attempted on every new session's first request; a
    // session that never saved must not wait behind another session's in-flight save to learn it.
    if (!required_response_id && !checkpoint_store_->may_hold(session_sha256)) { return false; }
    std::lock_guard lock(checkpoint_mutex_);
    try {
        // Another request for the same session may have restored it, or stored its reply, while
        // this one waited here; restoring again would replace that newer lineage with the
        // checkpoint.
        if (!required_response_id && responses.latest_response_id(session_sha256)) { return false; }
        // Each attempt gets a fresh reader, because a checkpoint reader is a single verified pass
        // and the engine consumes it before it can discover that the pool is full. An attempt is
        // worth repeating only when something actually changed: the engine dropped a resident
        // session, or this layer brought a stale one up to date so the engine may drop it next
        // time. Pins survive all attempts, including saves of later victims. Every retry either
        // releases a resident or pins a previously unpinned one; the derived bound below includes
        // both steps for every initial catalog slot and the final import.
        const std::size_t catalog_limit = std::max<std::size_t>(
            1, engine_->options().context_cache.max_private_continuations.value_or(1));
        // One pass may pin one stale resident and the next may reclaim it. Reserve the final
        // import after every initially resident catalog entry has made both progress steps. This
        // admits the shipped eight-entry worst case (17 reads) while sustained new arrivals
        // cannot extend one restore indefinitely.
        const std::size_t max_attempts = catalog_limit * 2 + 1;
        unsigned reclaimed_total       = 0;
        StoredCheckpointOracle reclaim(*checkpoint_store_, checkpoint_runtime_fingerprint_);
        for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
            reclaim.begin_attempt();
            SessionCheckpointLoadResult loaded = checkpoint_store_->load(
                session_sha256, checkpoint_runtime_fingerprint_, required_response_id);
            if (!loaded.checkpoint) {
                if (reclaimed_total != 0) {
                    write_console_log(ConsoleLogLevel::Warning,
                                      "checkpoint restore for session " +
                                          std::string(session_sha256.substr(0, 12)) +
                                          " became unavailable after reclaiming " +
                                          std::to_string(reclaimed_total) + " resident(s)");
                }
                return false;
            }
            VerifiedSessionCheckpoint checkpoint = std::move(*loaded.checkpoint);
            const std::string checkpoint_tag     = checkpoint.responses.latest_response_id;
            runtime::SessionRestoreSkipDetail skip;
            const bool restored = responses.restore_session(std::move(checkpoint.responses), [&] {
                return runtime::CheckpointEngineAccess::restore_session(
                           *engine_, session_sha256, checkpoint_tag, *checkpoint.engine,
                           checkpoint.expected_engine, checkpoint_store_->options().staging_bytes,
                           &skip, &reclaim)
                    .has_value();
            });
            reclaimed_total += skip.reclaimed;
            if (restored) {
                if (reclaimed_total != 0) {
                    // The session only fit because others were dropped. Their next request pays a
                    // restore, so the operator needs to see the pool running at its limit.
                    write_console_log(ConsoleLogLevel::Info,
                                      "checkpoint restore for session " +
                                          std::string(session_sha256.substr(0, 12)) +
                                          " reclaimed " + std::to_string(reclaimed_total) +
                                          " checkpoint-backed session(s)");
                }
                return true;
            }
            const bool capacity_bound =
                skip.reason == runtime::SessionRestoreSkipReason::ProgramRejected
                    ? runtime::contended_import_capacity(skip.import_reason)
                    : skip.reason == runtime::SessionRestoreSkipReason::CatalogFull;
            if (capacity_bound && skip.reclaimed != 0) { continue; }
            if (capacity_bound && !reclaim.stale().empty()) {
                std::size_t saved = 0;
                for (const auto& stale : reclaim.stale()) {
                    runtime::SessionCheckpointSkipDetail save_skip;
                    const auto result =
                        save_checkpoint_locked(stale.session_sha256, responses, &save_skip);
                    if (result && reclaim.recoverable(stale.session_sha256, stale.checkpoint_tag)) {
                        saved = 1;
                        // The engine releases at most one victim per import attempt, so saving
                        // more residents here only adds latency and can exhaust the disk quota.
                        break;
                    }
                    if (result) {
                        write_console_log(ConsoleLogLevel::Warning,
                                          "checkpoint save refused for resident session " +
                                              stale.session_sha256.substr(0, 12) +
                                              ": published checkpoint was not current");
                    } else {
                        write_console_log(
                            ConsoleLogLevel::Warning,
                            format_session_checkpoint_skip(
                                "checkpoint save refused for resident session: ",
                                stale.session_sha256, save_skip).view());
                    }
                }
                write_console_log(ConsoleLogLevel::Info,
                                  "checkpoint restore for session " +
                                      std::string(session_sha256.substr(0, 12)) + " saved " +
                                      std::to_string(saved) + " of " +
                                      std::to_string(reclaim.stale().size()) +
                                      " resident session(s) to make room");
                if (saved != 0) { continue; }
            }
            // A decline used to read as "the engine did not accept the checkpointed
            // continuation", which cannot distinguish a capacity bound from a drifted binding.
            // Name the gate, and the target's import gate when it reported one
            // (alphastorm/omp-ninfer#40).
            std::string detail =
                skip.reason == runtime::SessionRestoreSkipReason::None
                    ? "response store capacity cannot preserve unrelated sessions"
                    : std::string(runtime::session_restore_skip_reason_name(skip.reason));
            if (skip.import_reason != runtime::ContinuationImportSkipReason::None) {
                detail += " (";
                detail += runtime::continuation_import_skip_reason_name(skip.import_reason);
                detail += ")";
            }
            if (skip.reclaimed != 0 || skip.reclaim_declined != 0) {
                // Distinguishes "the pool cannot hold this session at all" from "the pool is
                // full of sessions that are not safe to drop yet".
                detail += " after reclaiming " + std::to_string(reclaimed_total) + ", kept " +
                          std::to_string(skip.reclaim_declined) + " unreproducible";
            }
            write_console_log(ConsoleLogLevel::Warning,
                              "checkpoint restore declined for session " +
                                  std::string(session_sha256.substr(0, 12)) + ": " + detail);
            return false;
        }
        write_console_log(ConsoleLogLevel::Warning,
                          "checkpoint restore declined for session " +
                              std::string(session_sha256.substr(0, 12)) + ": reached the " +
                              std::to_string(max_attempts) +
                              "-attempt catalog progress bound after reclaiming " +
                              std::to_string(reclaimed_total) + " resident(s)");
        return false;
    } catch (const std::exception& error) {
        // A restore that fails here surfaces to the client as previous_response_not_found;
        // the cause must be diagnosable from the server log.
        write_console_log(ConsoleLogLevel::Warning,
                          "checkpoint restore failed for session " +
                              std::string(session_sha256.substr(0, 12)) + ": " + error.what());
        return false;
    }
}

nlohmann::json GenerationService::checkpoint_status(std::string_view session_sha256) const {
    if (!checkpoint_store_) {
        return nlohmann::json{{"artifact_type", "ninfer_session_checkpoint_status"},
                              {"session_sha256", session_sha256},
                              {"state", "disabled"}};
    }
    std::lock_guard lock(checkpoint_mutex_);
    return checkpoint_store_->status(session_sha256, checkpoint_runtime_fingerprint_);
}

bool GenerationService::checkpoint_covers(std::string_view session_sha256,
                                          std::string_view response_id) const {
    if (!checkpoint_store_) { return false; }
    std::lock_guard lock(checkpoint_mutex_);
    return checkpoint_store_->covers(session_sha256, checkpoint_runtime_fingerprint_,
                                     response_id);
}

SessionCheckpointEraseResult GenerationService::erase_checkpoint(std::string_view session_sha256) {
    if (!checkpoint_store_) { return SessionCheckpointEraseResult::Missing; }
    std::lock_guard lock(checkpoint_mutex_);
    return checkpoint_store_->erase(session_sha256);
}

void GenerationService::warmup() {
    GenerationRequest request;
    ChatTurn turn;
    turn.role = ChatRole::User;
    ContentPart content;
    content.kind     = ContentKind::Text;
    content.text     = "hi";
    content.type_raw = "text";
    turn.content.push_back(std::move(content));
    request.messages.push_back(std::move(turn));
    request.max_tokens       = 4;
    request.max_tokens_set   = true;
    PreparedRequest prepared = prepare_impl(request, {}, {}, CacheParticipation::Disabled,
                                            DeadlinePolicy::UnboundedStartup, {});
    run(prepared, nullptr);
}

} // namespace ninfer::serve
