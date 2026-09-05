#include "serve/generation_service.h"
#if defined(_WIN32)
#    include "runtime/windows/direct_storage_checkpoint_read_queue.h"
#endif

#include "core/sha256.h"
#include "product/media_acquire/acquire.h"
#include "runtime/engine/checkpoint_engine_access.h"
#include "serve/client_identity.h"
#include "serve/console_log.h"
#include "serve/server_identity.h"
#include "serve/tool_call_parser.h"
#include "serve/translate.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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

struct MediaInputCapacity {
    std::mutex mutex;
    std::condition_variable cv;
    bool occupied = false;
};

struct MediaInputPermit {
    explicit MediaInputPermit(std::shared_ptr<MediaInputCapacity> owner)
        : capacity(std::move(owner)) {}

    ~MediaInputPermit() {
        {
            std::lock_guard lock(capacity->mutex);
            capacity->occupied = false;
        }
        capacity->cv.notify_one();
    }

    std::shared_ptr<MediaInputCapacity> capacity;
};

struct ClientSessionLocks {
    std::mutex mutex;
    std::unordered_map<std::string, std::weak_ptr<std::timed_mutex>> sessions;
};

struct ClientSessionLease {
    explicit ClientSessionLease(std::shared_ptr<std::timed_mutex> session_mutex)
        : owner(std::move(session_mutex)), lock(*owner, std::defer_lock) {}

    std::shared_ptr<std::timed_mutex> owner;
    std::unique_lock<std::timed_mutex> lock;
};

namespace {

using Clock                              = std::chrono::steady_clock;
constexpr std::size_t kMaximumMediaItems = 16;

[[noreturn]] void throw_preparation_cancelled();

[[noreturn]] void throw_media_error(const ninfer::product::media_acquire::Error& exception) {
    ApiError error;
    error.param   = "messages";
    error.message = exception.what();
    switch (exception.kind()) {
    case ninfer::product::media_acquire::ErrorKind::BudgetExceeded:
        error.status = 413;
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
    error.message = "client disconnected during request preparation";
    throw ApiException(std::move(error));
}

std::size_t media_item_count(const GenerationRequest& request) {
    std::size_t count = 0;
    for (const ChatTurn& message : request.messages) {
        for (const ContentPart& part : message.content) {
            if (part.kind == ContentKind::Image || part.kind == ContentKind::Video) { ++count; }
        }
    }
    return count;
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
    ApiError error;
    error.param   = "messages";
    error.message = exception.what();
    switch (exception.kind()) {
    case ninfer::RequestErrorKind::ContextLengthExceeded:
        error.status = 400;
        error.code   = "context_length_exceeded";
        break;
    case ninfer::RequestErrorKind::MediaBudgetExceeded:
        error.status = 413;
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
    case ninfer::RequestErrorKind::Unavailable:
        error.param.clear();
        error.status = 503;
        error.type   = "server_error";
        error.code   = "service_unavailable";
        break;
    }
    throw ApiException(std::move(error));
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

GenerationService::GenerationService(ServeOptions options, LoadProgress load_progress)
    : options_(std::move(options)) {
    validate_session_checkpoint_options(options_);
    // Inline ECC on GDDR6X GeForce cards reserves ~6.25% of VRAM for checksums and taxes
    // memory bandwidth on every access. Decode is bandwidth-bound, so an ECC-enabled card
    // silently loses a large share of its published throughput and KV capacity while looking
    // exactly like an engine regression. ECC is off by default on GeForce; warn loudly when
    // someone (or some tool) left it on.
    {
        cudaDeviceProp props{};
        if (cudaGetDeviceProperties(&props, options_.device) == cudaSuccess &&
            props.ECCEnabled != 0) {
            write_console_log(ConsoleLogLevel::Warning,
                              std::string("ECC is ENABLED on ") + props.name +
                                  ": GDDR6X stores ECC checksums in VRAM, costing ~6.25% of "
                                  "capacity (23,028 vs 24,564 MiB on a 24 GB card) and ~12% of "
                                  "memory bandwidth (measured 803 vs 902 GB/s on an RTX 3090 Ti). "
                                  "KV capacity and prefill suffer accordingly. ECC is off by "
                                  "default on GeForce; if this is not a deliberate reliability "
                                  "choice, disable it with `nvidia-smi -e 0` and reboot.");
        }
    }
    ninfer::EngineOptions engine_options;
    engine_options.artifact_path        = options_.artifact_path;
    engine_options.device               = options_.device;
    engine_options.max_context          = options_.max_context;
    engine_options.kv_capacity          = options_.kv_capacity;
    engine_options.max_concurrency      = options_.max_concurrency;
    engine_options.max_pending_requests = options_.max_pending_requests;
    engine_options.pending_timeout_ms   = options_.pending_timeout_ms;
    engine_options.prefill_chunk        = options_.prefill_chunk;
    engine_options.kv_cache             = options_.kv_cache;
    engine_options.enable_vision        = options_.enable_vision;
    engine_options.use_cuda_graph       = options_.use_cuda_graph;
    engine_options.speculative          = options_.speculative;
    engine_options.load_progress        = std::move(load_progress);
    engine_              = std::make_unique<ninfer::Engine>(std::move(engine_options));
    prompt_capabilities_ = engine_->prompt_capabilities();
    request_capacity_    = std::make_shared<RequestCapacity>(
        static_cast<std::size_t>(options_.max_concurrency) + options_.max_pending_requests);
    media_input_capacity_ = std::make_shared<MediaInputCapacity>();
    if (!options_.api_key.empty()) {
        session_tenant_sha256_ = session_checkpoint_tenant_sha256(options_.api_key);
        client_session_locks_  = std::make_shared<ClientSessionLocks>();
    }
    if (!options_.session_checkpoint_root.empty()) {
        std::shared_ptr<runtime::ContinuationCheckpointReadQueue> read_queue;
#if defined(_WIN32)
        read_queue = runtime::windows::make_direct_storage_checkpoint_read_queue(
            options_.session_checkpoint_root, 30'000);
#endif
        nlohmann::json fingerprint = session_checkpoint_runtime_fingerprint(
            options_, engine_->options(), engine_->load_summary(), engine_->memory_summary(),
            ninfer::build_info());
        SessionCheckpointEngine checkpoint_engine;
        checkpoint_engine.checkpoint =
            [this](const runtime::AuthenticatedCheckpointNamespace& checkpoint_namespace,
                   std::string_view checkpoint_tag,
                   runtime::ContinuationCheckpointWriter& writer, std::size_t staging_bytes) {
                return runtime::CheckpointEngineAccess::checkpoint_session(
                    *engine_, checkpoint_namespace, checkpoint_tag, writer, staging_bytes);
            };
        checkpoint_engine.restore =
            [this](const runtime::AuthenticatedCheckpointNamespace& checkpoint_namespace,
                   std::string_view checkpoint_tag,
                   const runtime::ContinuationCheckpointReader& reader,
                   runtime::ContinuationCheckpointStats expected, std::size_t staging_bytes) {
                return runtime::CheckpointEngineAccess::restore_session(
                    *engine_, checkpoint_namespace, std::string(checkpoint_tag), reader, expected,
                    staging_bytes);
            };
        if (options_.api_key.empty()) {
            // parse_serve_options enforces this for the CLI; direct construction must not
            // reach a publicly computable HMAC(empty, domain) origin key.
            throw std::invalid_argument("session checkpoints require a configured API key");
        }
        // The manifest origin MAC key derives from the bearer key through a fixed domain
        // separator - the bearer key itself never touches the checkpoint machinery, and a
        // writer inside the checkpoint root cannot recreate the key (alphastorm/ninfer#32).
        static constexpr std::string_view kOriginDomain =
            "ninfer-checkpoint-manifest-origin-v1";
        const crypto::Sha256Digest origin_key = crypto::hmac_sha256(
            std::as_bytes(std::span(options_.api_key.data(), options_.api_key.size())),
            std::as_bytes(std::span(kOriginDomain.data(), kOriginDomain.size())));
        checkpoint_manager_ = std::make_unique<SessionCheckpointManager>(
            SessionCheckpointStoreOptions{
                .root = options_.session_checkpoint_root,
                .disk_quota_bytes = options_.session_checkpoint_quota_bytes,
                .staging_bytes = options_.session_checkpoint_staging_bytes,
                .write_buffer_bytes = options_.session_checkpoint_write_buffer_bytes,
                .read_queue = std::move(read_queue),
                .tombstone_cleanup = {},
                .origin_mac_key = std::string(reinterpret_cast<const char*>(origin_key.data()),
                                              origin_key.size()),
                .require_origin_auth = options_.session_checkpoint_require_origin_auth,
            },
            std::move(fingerprint), session_tenant_sha256_, std::move(checkpoint_engine));
    }
}

std::shared_ptr<RequestLifetime> GenerationService::acquire_request_lifetime() const {
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
        return std::make_shared<RequestLifetime>(
            request_capacity_, started,
            started + std::chrono::milliseconds(options_.pending_timeout_ms));
    } catch (...) {
        std::lock_guard lock(request_capacity_->mutex);
        --request_capacity_->active;
        throw;
    }
}

HostInputLease
GenerationService::acquire_media_input(Clock::time_point deadline,
                                       const std::function<bool()>& is_cancelled) const {
    std::unique_lock lock(media_input_capacity_->mutex);
    while (media_input_capacity_->occupied) {
        if (is_cancelled && is_cancelled()) { throw_preparation_cancelled(); }
        const Clock::time_point now = Clock::now();
        if (now >= deadline) {
            throw_request_error(ninfer::RequestError(
                RequestErrorKind::QueueTimeout,
                "inference request expired while waiting for media preparation"));
        }
        media_input_capacity_->cv.wait_until(
            lock, std::min(deadline, now + std::chrono::milliseconds(10)));
    }
    if (is_cancelled && is_cancelled()) { throw_preparation_cancelled(); }
    if (Clock::now() >= deadline) {
        throw_request_error(
            ninfer::RequestError(RequestErrorKind::QueueTimeout,
                                 "inference request expired while waiting for media preparation"));
    }

    media_input_capacity_->occupied = true;
    lock.unlock();
    try {
        auto permit = std::make_shared<MediaInputPermit>(media_input_capacity_);
        return HostInputLease(std::static_pointer_cast<void>(std::move(permit)));
    } catch (...) {
        {
            std::lock_guard capacity_lock(media_input_capacity_->mutex);
            media_input_capacity_->occupied = false;
        }
        media_input_capacity_->cv.notify_one();
        throw;
    }
}

std::shared_ptr<ClientSessionLease> GenerationService::acquire_client_session(
    std::string_view session_sha256, Clock::time_point deadline,
    const std::function<bool()>& is_cancelled) const {
    if (!client_session_locks_) {
        throw std::logic_error("authenticated client session locks are unavailable");
    }
    std::shared_ptr<std::timed_mutex> session_mutex;
    {
        std::lock_guard lock(client_session_locks_->mutex);
        if (client_session_locks_->sessions.size() >= 1024) {
            for (auto entry = client_session_locks_->sessions.begin();
                 entry != client_session_locks_->sessions.end();) {
                if (entry->second.expired()) {
                    entry = client_session_locks_->sessions.erase(entry);
                } else {
                    ++entry;
                }
            }
        }
        std::weak_ptr<std::timed_mutex>& stored =
            client_session_locks_->sessions[std::string(session_sha256)];
        session_mutex = stored.lock();
        if (!session_mutex) {
            session_mutex = std::make_shared<std::timed_mutex>();
            stored        = session_mutex;
        }
    }
    auto lease = std::make_shared<ClientSessionLease>(std::move(session_mutex));
    for (;;) {
        if (is_cancelled && is_cancelled()) { throw_preparation_cancelled(); }
        const Clock::time_point now = Clock::now();
        if (now >= deadline) {
            throw_request_error(ninfer::RequestError(
                RequestErrorKind::QueueTimeout,
                "inference request expired while waiting for its private session"));
        }
        if (lease->lock.try_lock_until(
                std::min(deadline, now + std::chrono::milliseconds(10)))) {
            return lease;
        }
    }
}

PreparedRequest GenerationService::prepare(const GenerationRequest& request,
                                           std::function<bool()> is_cancelled,
                                           std::string checkpoint_tag) const {
    require_authenticated_client_identity(request, !options_.api_key.empty());
    if (!checkpoint_tag.empty() &&
        (!checkpoint_manager_ || !request.client_session_sha256 ||
         !runtime::AuthenticatedCheckpointNamespace::valid_sha256(
             *request.client_session_sha256))) {
        const std::invalid_argument error(
            "checkpoint-bound prompts require an authenticated session");
        throw_invalid_input(error);
    }
    PreparedRequest prepared;
    ninfer::RequestOptions request_options = to_request_options(request, options_);
    prepared.include_usage                 = request.include_usage;
    prepared.tool_capable                  = request.uses_tools() || request.has_tool_history();
    prepared.tool_name_max_length          = request.tool_name_max_length;
    prepared.tool_argument_types           = build_tool_argument_type_contracts(request);
    const ResolvedPromptSemantics semantics =
        resolve_prompt_semantics(request, options_, prompt_capabilities_);
    prepared.enable_thinking                   = semantics.enable_thinking;
    prepared.reasoning_effort                  = semantics.reasoning_effort;
    prepared.preserve_thinking                 = semantics.preserve_thinking;
    prepared.preserve_thinking_semantic_change = request.preserve_thinking_semantic_change;
    const std::size_t media_items              = media_item_count(request);
    const bool request_has_media               = media_items != 0;
    if (request_has_media && !options_.enable_vision) {
        const std::invalid_argument error("Vision is disabled for this server");
        throw_invalid_input(error, "vision_disabled");
    }
    if (media_items > kMaximumMediaItems) {
        throw_request_error(ninfer::RequestError(RequestErrorKind::MediaBudgetExceeded,
                                                 "request exceeds the 16-item media limit"));
    }
    prepared.lifetime = acquire_request_lifetime();
    if (request.client_session_sha256) {
        prepared.client_session = acquire_client_session(
            *request.client_session_sha256, prepared.lifetime->deadline, is_cancelled);
    }
    HostInputLease host_input;
    if (request_has_media) {
        host_input = acquire_media_input(prepared.lifetime->deadline, is_cancelled);
    }

    try {
        std::size_t remaining_media_bytes = options_.max_request_bytes;
        ninfer::PromptInput input =
            to_prompt_input(request, semantics, [&](const ContentPart& part) {
                return acquire_media(part, prepared.lifetime->deadline, is_cancelled,
                                     remaining_media_bytes);
            });
        check_preparation_control(prepared.lifetime->deadline, is_cancelled);
        ninfer::PreparedPrompt prompt = engine_->prepare(std::move(input));
        if (request.client_session_sha256) {
            runtime::AuthenticatedCheckpointNamespace checkpoint_namespace =
                runtime::AuthenticatedCheckpointNamespace::authenticated(
                    session_tenant_sha256_, *request.client_session_sha256);
            if (checkpoint_tag.empty()) {
                runtime::CheckpointEngineAccess::bind_cache_session(
                    prompt, std::move(checkpoint_namespace));
            } else {
                runtime::CheckpointEngineAccess::bind_checkpoint_session(
                    prompt, std::move(checkpoint_namespace), std::move(checkpoint_tag));
            }
        }
        check_preparation_control(prepared.lifetime->deadline, is_cancelled);
        prepared.prompt_tokens = static_cast<int>(prompt.summary().prompt_tokens);
        prepared.prepare_seconds =
            std::chrono::duration<double>(Clock::now() - prepared.lifetime->started).count();
        prepared.generation = engine_->submit(std::move(prompt), std::move(request_options),
                                              prepared.lifetime->deadline, std::move(host_input));
        prepared.sampling   = prepared.generation.resolved_sampling();
    } catch (const ApiException&) { throw; } catch (const ninfer::RequestError& exception) {
        throw_request_error(exception);
    } catch (const std::invalid_argument& exception) { throw_invalid_input(exception); }
    return prepared;
}

int GenerationService::count_prompt_tokens(const GenerationRequest& request,
                                           std::function<bool()> is_cancelled) const {
    require_authenticated_client_identity(request, !options_.api_key.empty());
    const std::size_t media_items = media_item_count(request);
    const bool request_has_media  = media_items != 0;
    if (request_has_media && !options_.enable_vision) {
        const std::invalid_argument error("Vision is disabled for this server");
        throw_invalid_input(error, "vision_disabled");
    }
    if (media_items > kMaximumMediaItems) {
        throw_request_error(ninfer::RequestError(RequestErrorKind::MediaBudgetExceeded,
                                                 "request exceeds the 16-item media limit"));
    }
    const Clock::time_point deadline =
        Clock::now() + std::chrono::milliseconds(options_.pending_timeout_ms);
    const ResolvedPromptSemantics semantics =
        resolve_prompt_semantics(request, options_, prompt_capabilities_);
    HostInputLease host_input;
    if (request_has_media) { host_input = acquire_media_input(deadline, is_cancelled); }
    try {
        std::size_t remaining_media_bytes = options_.max_request_bytes;
        ninfer::PromptInput input =
            to_prompt_input(request, semantics, [&](const ContentPart& part) {
                return acquire_media(part, deadline, is_cancelled, remaining_media_bytes);
            });
        check_preparation_control(deadline, is_cancelled);
        const int prompt_tokens = static_cast<int>(engine_->count_tokens(std::move(input)));
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
    outcome.metrics.prefix_cache_hit_tokens     = result.reused_prompt_tokens;
    outcome.metrics.prefix_reuse_path           = result.prefix_reuse_path;
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

bool GenerationService::checkpoint_enabled() const noexcept {
    return checkpoint_manager_ != nullptr && checkpoint_manager_->enabled();
}

SessionCheckpointSaveOutcome
GenerationService::save_checkpoint(std::string_view session_sha256,
                                   std::string_view required_response_id,
                                   ResponseStore& responses) {
    if (!checkpoint_manager_) {
        return {.state = SessionCheckpointSaveState::Disabled, .checkpoint = std::nullopt};
    }
    return checkpoint_manager_->save(session_sha256, required_response_id, responses);
}

SessionCheckpointRestoreState
GenerationService::restore_checkpoint(std::string_view session_sha256,
                                      std::string_view required_response_id,
                                      ResponseStore& responses) {
    if (!checkpoint_manager_) { return SessionCheckpointRestoreState::Disabled; }
    return checkpoint_manager_->restore(session_sha256, required_response_id, responses);
}

nlohmann::json GenerationService::checkpoint_status(std::string_view session_sha256) {
    if (!checkpoint_manager_) {
        return nlohmann::json{{"artifact_type", "ninfer_session_checkpoint_status"},
                              {"state", "disabled"}};
    }
    return checkpoint_manager_->status(session_sha256);
}

bool GenerationService::checkpoint_covers(std::string_view session_sha256,
                                          std::string_view response_id) const {
    if (!checkpoint_manager_) { return false; }
    return checkpoint_manager_->covers(session_sha256, response_id);
}

SessionCheckpointEraseResult GenerationService::erase_checkpoint(
    std::string_view session_sha256) {
    if (!checkpoint_manager_) { return SessionCheckpointEraseResult::Missing; }
    return checkpoint_manager_->erase(session_sha256);
}

SessionCheckpointEraseResult GenerationService::erase_checkpoint_response(
    std::string_view session_sha256, std::string_view response_id, ResponseStore& responses) {
    if (!checkpoint_manager_) { return SessionCheckpointEraseResult::Missing; }
    return checkpoint_manager_->erase_response(session_sha256, response_id, responses);
}

void GenerationService::warmup() {
    try {
        GenerationRequest request;
        ChatTurn turn;
        turn.role = "user";
        ContentPart content;
        content.kind     = ContentKind::Text;
        content.text     = "hi";
        content.type_raw = "text";
        turn.content.push_back(std::move(content));
        request.messages.push_back(std::move(turn));
        request.max_tokens       = 4;
        request.max_tokens_set   = true;
        PreparedRequest prepared = prepare(request);
        run(prepared, nullptr);
    } catch (const std::exception& exception) {
        write_console_log(ConsoleLogLevel::Warning,
                          std::string("warmup failed (continuing): ") + exception.what());
    }
}

} // namespace ninfer::serve
