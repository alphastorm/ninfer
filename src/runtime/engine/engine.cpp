#include "ninfer/engine.h"

#include "core/device.h"
#include "core/nvtx.h"
#include "core/startup.h"
#include "runtime/contract/sampling.h"
#include "runtime/contract/request.h"
#include "runtime/engine/causal_score_core.h"
#include "runtime/engine/engine_core.h"
#include "runtime/engine/model_construction.h"
#include "models/common/qwen/frontend/prepared_prompt.h"
#if defined(__linux__)
#include "runtime/platform/linux/io_uring_checkpoint_backend.h"
#elif defined(_WIN32)
#include "runtime/windows/direct_storage_checkpoint_backend.h"
#endif

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace ninfer {
namespace {

DeviceContext initialize_device(const EngineOptions& options) {
    StartupPhaseScope phase(options.startup_observer, StartupPhase::CudaInitialize);
    DeviceContext device(options.device);
    phase.complete();
    return device;
}

runtime::ResolvedRequestOptions resolve_request_options(const ModelSamplingDefaults& defaults,
                                                        SamplingMode mode, RequestOptions options) {
    if (options.execution.thinking.budget && *options.execution.thinking.budget == 0) {
        throw std::invalid_argument("thinking budget must be positive");
    }
    runtime::ResolvedRequestOptions resolved;
    resolved.execution.sampling =
        runtime::resolve_sampling(defaults, mode, options.execution.sampling);
    resolved.execution.requested_output_tokens = options.execution.requested_output_tokens;
    resolved.execution.allow_prefix_reuse      = options.execution.allow_prefix_reuse;
    resolved.execution.thinking                = options.execution.thinking;
    resolved.stop                              = std::move(options.stop);
    resolved.output                            = options.output;
    resolved.constraint                        = options.constraint;
    resolved.tool_choice                       = std::move(options.tool_choice);
    return resolved;
}

std::string context_capacity_error(std::size_t prompt_tokens, std::uint32_t max_context) {
    return "prepared prompt has " + std::to_string(prompt_tokens) +
           " tokens, exceeding Engine max_context " + std::to_string(max_context);
}

runtime::DurableEngineIdentity engine_identity(const EngineOptions& options,
                                               const MemorySummary& memory) {
    const char* kv;
    switch (options.kv_cache) {
    case KvCacheStorage::BFloat16: kv = "bf16"; break;
    case KvCacheStorage::Int8Group64: kv = "int8"; break;
    case KvCacheStorage::Fp8E4M3Row256: kv = "fp8"; break;
    case KvCacheStorage::Fp8KeyNvfp4Value: kv = "k8v4"; break;
    case KvCacheStorage::Nvfp4Group16: kv = "nvfp4"; break;
    }
    const char* backend;
    switch (options.speculative.backend) {
    case SpeculativeBackend::None: backend = "none"; break;
    case SpeculativeBackend::Mtp: backend = "mtp"; break;
    case SpeculativeBackend::DFlash: backend = "dflash"; break;
    case SpeculativeBackend::DFlash2: backend = "dflash2"; break;
    }
    return {.max_context = options.max_context, .kv_capacity = memory.kv_capacity,
            .kv_dtype = kv, .max_concurrency = options.max_concurrency,
            .prefill_chunk = memory.prefill_chunk, .speculative_backend = backend,
            .draft_window = options.speculative.draft_tokens,
            .proposal_head = options.speculative.proposal_head == ProposalHead::Optimized
                ? "optimized" : "full",
            .enable_vision = options.enable_vision, .use_cuda_graph = options.use_cuda_graph};
}

} // namespace

class PreparedPrompt::Impl {
public:
    Impl(PromptSummary prompt_summary, PromptPreparationStats preparation, SamplingMode mode,
         models::qwen::PreparedPrompt prepared)
        : summary(std::move(prompt_summary)), prepare(std::move(preparation)), sampling_mode(mode),
          value(std::move(prepared)) {}

    PromptSummary summary;
    PromptPreparationStats prepare;
    SamplingMode sampling_mode = SamplingMode::Thinking;
    models::qwen::PreparedPrompt value;
    std::string checkpoint_tag;
};

PreparedPrompt::PreparedPrompt() noexcept                            = default;
PreparedPrompt::~PreparedPrompt()                                    = default;
PreparedPrompt::PreparedPrompt(PreparedPrompt&&) noexcept            = default;
PreparedPrompt& PreparedPrompt::operator=(PreparedPrompt&&) noexcept = default;

PreparedPrompt::PreparedPrompt(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

const PromptSummary& PreparedPrompt::summary() const noexcept {
    static const PromptSummary empty;
    return impl_ != nullptr ? impl_->summary : empty;
}

const PromptPreparationStats& PreparedPrompt::preparation_stats() const noexcept {
    static const PromptPreparationStats empty;
    return impl_ != nullptr ? impl_->prepare : empty;
}

PreparedPrompt::operator bool() const noexcept { return impl_ != nullptr; }

class GenerationHandle::Impl {
public:
    class Concept {
    public:
        virtual ~Concept() = default;
        virtual GenerationResult wait(OutputSink* sink, const CancellationView& cancellation) = 0;
        virtual runtime::DurableSessionEpoch publication_epoch() const noexcept = 0;
    };

    template <class Submission>
    class Model final : public Concept {
    public:
        Model(std::shared_ptr<void> keep_alive, Submission submission)
            : keep_alive_(std::move(keep_alive)), submission_(std::move(submission)) {}

        GenerationResult wait(OutputSink* sink, const CancellationView& cancellation) override {
            return submission_.wait(sink, cancellation);
        }

        runtime::DurableSessionEpoch publication_epoch() const noexcept override {
            if constexpr (requires { submission_.publication_epoch(); }) {
                return submission_.publication_epoch();
            } else {
                return {};
            }
        }

    private:
        std::shared_ptr<void> keep_alive_;
        Submission submission_;
    };

    template <class Submission>
    Impl(std::shared_ptr<void> keep_alive, Submission submission,
         ResolvedSamplingParameters sampling)
        : state_(std::make_unique<Model<Submission>>(std::move(keep_alive), std::move(submission))),
          sampling_(sampling) {}

    GenerationResult wait(OutputSink* sink, const CancellationView& cancellation) {
        return state_->wait(sink, cancellation);
    }

    [[nodiscard]] const ResolvedSamplingParameters& resolved_sampling() const noexcept {
        return sampling_;
    }

    [[nodiscard]] runtime::DurableSessionEpoch publication_epoch() const noexcept {
        return state_->publication_epoch();
    }

private:
    std::unique_ptr<Concept> state_;
    ResolvedSamplingParameters sampling_;
};

GenerationHandle::GenerationHandle() noexcept                              = default;
GenerationHandle::~GenerationHandle()                                      = default;
GenerationHandle::GenerationHandle(GenerationHandle&&) noexcept            = default;
GenerationHandle& GenerationHandle::operator=(GenerationHandle&&) noexcept = default;

GenerationHandle::GenerationHandle(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

GenerationHandle::operator bool() const noexcept { return impl_ != nullptr; }

const ResolvedSamplingParameters& GenerationHandle::resolved_sampling() const noexcept {
    static const ResolvedSamplingParameters empty;
    return impl_ != nullptr ? impl_->resolved_sampling() : empty;
}

GenerationResult GenerationHandle::wait(OutputSink* sink, const CancellationView& cancellation) {
    if (impl_ == nullptr) { throw std::logic_error("GenerationHandle is empty"); }
    std::unique_ptr<Impl> impl = std::move(impl_);
    return impl->wait(sink, cancellation);
}

class Engine::Impl {
public:
    using Qwen35         = runtime::ModelInstance<models::qwen3_5::RuntimeTypes>;
    using GenerationCore = runtime::EngineCore<Qwen35>;
    using ScoringCore    = runtime::CausalScoreCore<Qwen35>;
    using Qwen4          = runtime::ModelInstance<models::qwen4_exp::RuntimeTypes>;
    using Core =
        std::variant<std::monostate, std::unique_ptr<GenerationCore>, std::unique_ptr<ScoringCore>,
                     std::unique_ptr<runtime::EngineCore<Qwen4>>,
                     std::unique_ptr<runtime::CausalScoreCore<Qwen4>>>;

    explicit Impl(EngineOptions engine_options)
        : options(runtime::normalize_engine_options(std::move(engine_options))),
          device(initialize_device(options)) {
        nvtx::ScopedRange load_range(nvtx::Name::EngineLoad, nvtx::Category::Runtime);
        auto constructed    = runtime::construct_model(options, device);
        active              = std::move(constructed.instance);
        load                = std::move(constructed.load);
        load.cuda_sync_mode = device.sync_mode();
        sampling_defaults   = frontend().sampling_defaults();
        StartupPhaseScope finalize_phase(options.startup_observer, StartupPhase::EngineFinalize);
        std::visit(
            [&](auto& instance) {
                using Instance = typename std::remove_cvref_t<decltype(instance)>::element_type;
                if (options.purpose == EnginePurpose::CausalScoring) {
                    core = std::make_unique<runtime::CausalScoreCore<Instance>>(*instance, device);
                } else {
                    // Resolved identity is immutable. Capture it before the worker starts;
                    // import must not contend for live execution just to compare K2 fields.
                    checkpoint_identity = engine_identity(options, instance->program->memory_summary());
                    core = std::make_unique<runtime::EngineCore<Instance>>(
                        *instance, device, options, std::move(constructed.context_cost));
                }
            },
            active);
        finalize_phase.complete();
    }

    ~Impl() noexcept {
        device.bind_to_current_thread_noexcept();
        core.emplace<std::monostate>();
        try {
            device.synchronize();
        } catch (...) {}
    }

    [[nodiscard]] const models::qwen::Frontend& frontend() const {
        return std::visit(
            [](const auto& instance) -> const models::qwen::Frontend& {
                return instance->frontend;
            },
            active);
    }

    EngineOptions options;
    DeviceContext device;
    runtime::ActiveModel active;
    LoadSummary load;
    ModelSamplingDefaults sampling_defaults;
    runtime::DurableEngineIdentity checkpoint_identity;
    Core core;
};

Engine::Engine(EngineOptions options) {
    StartupObserver startup_observer = options.startup_observer;
    StartupPhaseScope startup_phase(startup_observer, StartupPhase::EngineStartup);
    impl_ = std::make_shared<Impl>(std::move(options));
    startup_phase.complete();
}

Engine::~Engine()                            = default;
Engine::Engine(Engine&&) noexcept            = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

PreparedPrompt Engine::prepare(PromptInput input, const PreparationControl& control) const {
    nvtx::ScopedRange prepare_range(nvtx::Name::FrontendPrepare, nvtx::Category::Runtime);
    if (impl_ == nullptr) { throw std::logic_error("Engine is moved from"); }
    auto prepared      = impl_->frontend().prepare(std::move(input), control);
    PromptSummary info = prepared.summary();
    const SamplingMode sampling_mode =
        info.starts_in_reasoning ? SamplingMode::Thinking : SamplingMode::NonThinking;
    if (info.prompt_tokens > impl_->options.max_context) {
        throw std::logic_error("target Frontend admitted a prompt beyond Engine capacity");
    }
    const PromptPreparationStats preparation = prepared.preparation_stats();
    return PreparedPrompt(std::make_unique<PreparedPrompt::Impl>(info, preparation, sampling_mode,
                                                                 std::move(prepared)));
}

PreparedPrompt Engine::prepare_tokens(std::vector<TokenId> token_ids,
                                      bool allow_prefix_identity) const {
    nvtx::ScopedRange prepare_range(nvtx::Name::FrontendPrepare, nvtx::Category::Runtime,
                                    static_cast<std::uint64_t>(token_ids.size()));
    if (impl_ == nullptr) { throw std::logic_error("Engine is moved from"); }
    if (token_ids.size() > impl_->options.max_context) {
        throw RequestError(RequestErrorKind::ContextLengthExceeded,
                           context_capacity_error(token_ids.size(), impl_->options.max_context));
    }
    auto prepared = impl_->frontend().prepare_tokens(std::move(token_ids), allow_prefix_identity);
    PromptSummary info = prepared.summary();
    if (info.prompt_tokens > impl_->options.max_context) {
        throw std::logic_error("target Frontend admitted prompt tokens beyond capacity");
    }
    const PromptPreparationStats preparation = prepared.preparation_stats();
    return PreparedPrompt(std::make_unique<PreparedPrompt::Impl>(
        info, preparation, SamplingMode::Thinking, std::move(prepared)));
}

std::vector<TokenId> Engine::tokenize_text(std::string_view text) const {
    if (impl_ == nullptr) { throw std::logic_error("Engine is moved from"); }
    return impl_->frontend().tokenize_text(text);
}

std::vector<float> Engine::score_tokens(std::vector<TokenId> tokens, std::uint32_t first_target) {
    nvtx::ScopedRange score_range(nvtx::Name::Score, nvtx::Category::Scoring,
                                  static_cast<std::uint64_t>(tokens.size()));
    if (impl_ == nullptr) { throw std::logic_error("Engine is moved from"); }
    if (impl_->options.purpose != EnginePurpose::CausalScoring) {
        throw std::logic_error("score_tokens requires a CausalScoring Engine");
    }
    if (tokens.size() < 2 || tokens.size() > impl_->options.max_context) {
        throw std::invalid_argument("score_tokens token count must be in [2,max_context]");
    }
    if (first_target == 0 || first_target >= tokens.size()) {
        throw std::invalid_argument("score_tokens first_target must be in [1,token_count-1]");
    }
    PreparedPrompt prompt      = prepare_tokens(std::move(tokens), false);
    const std::size_t expected = prompt.summary().prompt_tokens - first_target;
    std::vector<float> result  = std::visit(
        [&](auto& core) -> std::vector<float> {
            using CoreState = std::remove_cvref_t<decltype(core)>;
            if constexpr (std::is_same_v<CoreState, std::monostate>) {
                throw std::logic_error("Engine core is unavailable");
            } else if constexpr (CoreState::element_type::purpose == EnginePurpose::CausalScoring) {
                return core->score(std::move(prompt.impl_->value), first_target);
            } else {
                throw std::logic_error("Engine scoring core is unavailable");
            }
        },
        impl_->core);
    if (result.size() != expected) {
        throw std::logic_error("target Program returned an invalid causal score count");
    }
    return result;
}

std::uint32_t Engine::count_tokens(PromptInput input, const PreparationControl& control) const {
    if (impl_ == nullptr) { throw std::logic_error("Engine is moved from"); }
    return impl_->frontend().count_tokens(std::move(input), control);
}

ModelSamplingDefaults Engine::sampling_defaults() const {
    if (impl_ == nullptr) { throw std::logic_error("Engine is moved from"); }
    return impl_->sampling_defaults;
}

GenerationHandle Engine::submit(PreparedPrompt prompt, RequestOptions options,
                                OutputConsumerMode consumer_mode,
                                GenerationObservationOptions observation,
                                std::chrono::steady_clock::time_point pending_deadline) {
    if (impl_ == nullptr) { throw std::logic_error("Engine is moved from"); }
    if (impl_->options.purpose != EnginePurpose::Generation) {
        throw std::logic_error("submit requires a Generation Engine");
    }
    if (prompt.impl_ == nullptr) { throw std::invalid_argument("PreparedPrompt is empty"); }
    if (observation.live_timings) { observation.phase_timings = true; }
    if (consumer_mode != OutputConsumerMode::Streaming &&
        (observation.live_timings || observation.prompt_progress)) {
        throw std::invalid_argument("live generation observations require a Streaming consumer");
    }

    runtime::ResolvedRequestOptions resolved_options = resolve_request_options(
        impl_->sampling_defaults, prompt.impl_->sampling_mode, std::move(options));
    const ResolvedSamplingParameters resolved_sampling = resolved_options.execution.sampling;

    const PromptSummary prompt_summary = prompt.impl_->summary;
    if (prompt_summary.prompt_tokens > impl_->options.max_context) {
        throw RequestError(
            RequestErrorKind::ContextLengthExceeded,
            context_capacity_error(prompt_summary.prompt_tokens, impl_->options.max_context));
    }
    const double prepare_seconds = prompt.impl_->prepare.seconds;
    if (resolved_options.execution.requested_output_tokens == 0) {
        struct ImmediateSubmission {
            GenerationResult result;
            OutputConsumerMode consumer_mode = OutputConsumerMode::Aggregate;

            GenerationResult wait(OutputSink* sink, const CancellationView& cancellation) {
                const bool streaming = consumer_mode == OutputConsumerMode::Streaming;
                if (streaming != (sink != nullptr)) {
                    throw std::invalid_argument(
                        "GenerationHandle wait sink does not match its submitted consumer mode");
                }
                if (cancellation.requested()) { result.finish_reason = FinishReason::Cancelled; }
                return std::move(result);
            }
        } immediate{.consumer_mode = consumer_mode};

        immediate.result.prompt                     = prompt_summary;
        immediate.result.finish_reason              = FinishReason::OutputLimit;
        immediate.result.thinking.configured_budget = resolved_options.execution.thinking.budget;
        immediate.result.timings.prepare_seconds    = prepare_seconds;
        immediate.result.timings.total_seconds      = prepare_seconds;
        prompt.impl_.reset();
        return GenerationHandle(std::make_unique<GenerationHandle::Impl>(
            impl_, std::move(immediate), resolved_sampling));
    }

    return std::visit(
        [&](auto& core) -> GenerationHandle {
            using CoreState = std::remove_cvref_t<decltype(core)>;
            if constexpr (std::is_same_v<CoreState, std::monostate>) {
                throw std::logic_error("Engine core is unavailable");
            } else if constexpr (CoreState::element_type::purpose == EnginePurpose::CausalScoring) {
                throw std::logic_error("Engine generation core is unavailable");
            } else {
                std::string checkpoint_session;
                const auto& cache = models::qwen::PreparedPromptAccess::view(
                    prompt.impl_->value).context_cache;
                if (cache.update_session_index && cache.session_key && cache.reuse_scope &&
                    cache.session_key == cache.reuse_scope &&
                    cache.session_key->view().starts_with("http:")) {
                    checkpoint_session = cache.session_key->view().substr(5);
                }
                auto submission =
                    core->submit(std::move(prompt.impl_->value), prompt_summary, prepare_seconds,
                                 std::move(resolved_options), consumer_mode, std::move(observation),
                                 pending_deadline, std::move(checkpoint_session),
                                 std::move(prompt.impl_->checkpoint_tag));
                return GenerationHandle(std::make_unique<GenerationHandle::Impl>(
                    impl_, std::move(submission), resolved_sampling));
            }
        },
        impl_->core);
}

GenerationResult Engine::generate(PreparedPrompt prompt, RequestOptions options, OutputSink* sink,
                                  const CancellationView& cancellation) {
    const OutputConsumerMode consumer_mode =
        sink != nullptr ? OutputConsumerMode::Streaming : OutputConsumerMode::Aggregate;
    return submit(std::move(prompt), std::move(options), consumer_mode, {})
        .wait(sink, cancellation);
}

const EngineOptions& Engine::options() const {
    if (impl_ == nullptr) { throw std::logic_error("Engine is moved from"); }
    return impl_->options;
}

LoadSummary Engine::load_summary() const {
    if (impl_ == nullptr) { throw std::logic_error("Engine is moved from"); }
    return impl_->load;
}

MemorySummary Engine::memory_summary() const {
    if (impl_ == nullptr) { throw std::logic_error("Engine is moved from"); }
    return std::visit(
        [](const auto& core) -> MemorySummary {
            using CoreState = std::remove_cvref_t<decltype(core)>;
            if constexpr (std::is_same_v<CoreState, std::monostate>) {
                throw std::logic_error("Engine core is unavailable");
            } else {
                return core->memory_summary();
            }
        },
        impl_->core);
}

MediaCacheSummary Engine::media_cache_summary() const {
    if (impl_ == nullptr) { throw std::logic_error("Engine is moved from"); }
    return impl_->frontend().media_cache_summary();
}

RuntimeStats Engine::runtime_stats() const {
    if (impl_ == nullptr) { throw std::logic_error("Engine is moved from"); }
    return std::visit(
        [](const auto& core) -> RuntimeStats {
            using CoreState = std::remove_cvref_t<decltype(core)>;
            if constexpr (std::is_same_v<CoreState, std::monostate>) {
                throw std::logic_error("Engine core is unavailable");
            } else {
                return core->runtime_stats();
            }
        },
        impl_->core);
}

bool Engine::is_available() const {
    if (impl_ == nullptr) { return false; }
    return std::visit(
        [](const auto& core) {
            using CoreState = std::remove_cvref_t<decltype(core)>;
            if constexpr (std::is_same_v<CoreState, std::monostate>) {
                return false;
            } else {
                return core != nullptr && core->is_available();
            }
        },
        impl_->core);
}

void Engine::reset_memory_peaks() noexcept {
    if (impl_ == nullptr) { return; }
    std::visit(
        [](auto& core) {
            using CoreState = std::remove_cvref_t<decltype(core)>;
            if constexpr (!std::is_same_v<CoreState, std::monostate>) {
                core->reset_memory_peaks();
            }
        },
        impl_->core);
}

namespace runtime {
namespace {
template <class Variant, class Function>
auto generation_core(Variant& cores, Function&& function)
    -> decltype(function(*std::get<1>(cores))) {
    using Result = decltype(function(*std::get<1>(cores)));
    return std::visit([&](auto& core) -> Result {
        using State = std::remove_cvref_t<decltype(core)>;
        if constexpr (std::is_same_v<State, std::monostate>) {
            throw std::runtime_error("Engine core is unavailable");
        } else if constexpr (State::element_type::purpose != EnginePurpose::Generation) {
            throw std::invalid_argument("checkpoint access requires Generation Engine");
        } else {
            return function(*core);
        }
    }, cores);
}

} // namespace

struct DurableEngineImport::Impl {
    // Keep Program, its worker and the device alive until the native reservation is aborted.
    std::shared_ptr<void> engine;
    PreparedEngineCheckpoint operation;
    ~Impl() { if (operation.abort) { operation.abort(); } }
};

DurableEngineImport::DurableEngineImport(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
DurableEngineImport::DurableEngineImport(DurableEngineImport&&) noexcept = default;
DurableEngineImport& DurableEngineImport::operator=(DurableEngineImport&&) noexcept = default;
DurableEngineImport::~DurableEngineImport() = default;

void DurableEngineImport::abort() noexcept { impl_.reset(); }

DurableImportResult DurableEngineImport::commit() noexcept {
    if (!impl_) { return {.reason = DurableReason::Cancelled}; }
    DurableImportResult result;
    try {
        result = impl_->operation.commit();
    } catch (const std::bad_alloc&) {
        result = {DurableImportState::CapacityRefused, DurableReason::Capacity};
    } catch (...) {
        result = {DurableImportState::Unavailable, DurableReason::IoUnavailable};
    }
    abort();
    return result;
}

DurableNativeLayout CheckpointEngineAccess::checkpoint_layout(const Engine& engine) {
    if (!engine.impl_) { throw std::logic_error("Engine is moved from"); }
    return generation_core(engine.impl_->core, [](auto& core) { return core.checkpoint_layout(); });
}

DurableFrontendIdentity CheckpointEngineAccess::frontend_identity(const Engine& engine) {
    if (!engine.impl_) { throw std::logic_error("Engine is moved from"); }
    return engine.impl_->frontend().durable_identity();
}

void CheckpointEngineAccess::set_pressure_checkpoint_handler(
    Engine& engine, std::shared_ptr<PressureCheckpointHandler> handler) {
    if (!engine.impl_) { throw std::logic_error("Engine is moved from"); }
    generation_core(engine.impl_->core,
        [&](auto& core) { core.set_pressure_checkpoint_handler(std::move(handler)); });
}

void CheckpointEngineAccess::set_reclaimable_session_oracle(
    Engine& engine, std::shared_ptr<const ReclaimableSessionOracle> oracle) {
    if (!engine.impl_) { throw std::logic_error("Engine is moved from"); }
    generation_core(engine.impl_->core,
        [&](auto& core) { core.set_reclaimable_session_oracle(std::move(oracle)); });
}

void CheckpointEngineAccess::notify_checkpoint_progress(Engine& engine) noexcept {
    if (!engine.impl_) { return; }
    try {
        generation_core(engine.impl_->core, [](auto& core) { core.notify_checkpoint_progress(); });
    } catch (...) {}
}

void CheckpointEngineAccess::response_stored(
    Engine& engine, std::string_view digest, std::string_view tag) noexcept {
    if (!engine.impl_) { return; }
    try {
        generation_core(engine.impl_->core, [&](auto& core) { core.response_stored(digest, tag); });
    } catch (...) {}
}

std::uint64_t CheckpointEngineAccess::checkpoint_progress(const Engine& engine) noexcept {
    if (!engine.impl_) { return 0; }
    try {
        return generation_core(engine.impl_->core, [](auto& core) { return core.checkpoint_progress(); });
    } catch (...) { return 0; }
}

void CheckpointEngineAccess::set_checkpoint_tag(PreparedPrompt& prompt, std::string_view tag) {
    if (!prompt.impl_) { throw std::invalid_argument("PreparedPrompt is empty"); }
    prompt.impl_->checkpoint_tag = tag;
}

DurableSessionEpoch CheckpointEngineAccess::session_epoch(
    Engine& engine, std::string_view digest) {
    if (!engine.impl_) { throw std::logic_error("Engine is moved from"); }
    return generation_core(engine.impl_->core, [&](auto& core) { return core.session_epoch(digest); });
}

DurableSessionEpoch CheckpointEngineAccess::publication_epoch(const GenerationHandle& handle) {
    return handle.impl_ ? handle.impl_->publication_epoch() : DurableSessionEpoch{};
}

std::shared_ptr<ContinuationCheckpointReadQueue> CheckpointEngineAccess::make_read_queue(
    Engine& engine, const std::filesystem::path& root) {
    if (!engine.impl_) { throw std::logic_error("Engine is moved from"); }
#if defined(__linux__)
    return make_io_uring_checkpoint_read_queue(root);
#elif defined(_WIN32)
    return generation_core(engine.impl_->core, [&](auto& core) {
        return core.checkpoint_call([&] {
            return windows::make_direct_storage_checkpoint_read_queue(root, 30'000);
        });
    });
#else
    throw std::runtime_error("checkpoint read queue is unavailable on this platform");
#endif
}

DurableExportResult CheckpointEngineAccess::export_session(
    Engine& engine, std::string_view digest, std::string_view tag,
    ContinuationCheckpointWriter& writer, const DurableLimits& limits) {
    if (!engine.impl_) { return {.reason = DurableReason::IoUnavailable}; }
    const auto keep_alive = engine.impl_;
    return generation_core(keep_alive->core, [&](auto& core) {
        return core.export_session(digest, tag, writer, limits);
    });
}

DurablePublicationResult CheckpointEngineAccess::publish_export(
    Engine& engine, std::string_view digest, std::string_view tag,
    DurableSessionEpoch epoch, const std::function<bool()>& callback) {
    if (!engine.impl_) { return {.reason = DurableReason::IoUnavailable}; }
    return generation_core(engine.impl_->core, [&](auto& core) {
        return core.publish_export(digest, tag, epoch, callback);
    });
}

DurablePrepareResult CheckpointEngineAccess::prepare_import(
    Engine& engine, const DurableImportTarget& target, const ContinuationCheckpointReader& reader,
    const RuntimeFingerprintV2& fingerprint, const DurableCheckpointStats& stats,
    const DurableLimits& limits) {
    if (!engine.impl_) {
        return {.state = DurablePrepareState::Unavailable, .reason = DurableReason::IoUnavailable};
    }
    // Warmup/store:false/cache-disabled must refuse before inspecting Reader or reserving native
    // capacity. The complete authenticated identity remains the serving store's responsibility.
    const auto& options = engine.impl_->options;
    if (!options.context_cache.enabled || !target.allow_prefix_reuse ||
        !target.cache_participation || options.purpose != EnginePurpose::Generation) {
        return {.state = DurablePrepareState::PolicyDisabled, .reason = DurableReason::CacheDisabled};
    }
    if (fingerprint.engine != engine.impl_->checkpoint_identity ||
        fingerprint.frontend != frontend_identity(engine)) {
        return {.state = DurablePrepareState::IdentityMismatch,
                .reason = DurableReason::FingerprintMismatch};
    }
    auto impl = std::make_unique<DurableEngineImport::Impl>();
    impl->engine = engine.impl_;
    impl->operation = generation_core(engine.impl_->core, [&](auto& core) {
        return core.prepare_import(target, reader, fingerprint, stats, limits);
    });
    DurablePrepareResult result{.state = impl->operation.state, .reason = impl->operation.reason};
    if (result.state == DurablePrepareState::Prepared) {
        result.prepared = DurableEngineImport(std::move(impl));
    }
    return result;
}

DurableImportResult CheckpointEngineAccess::import_session(
    Engine& engine, const DurableImportTarget& target, const ContinuationCheckpointReader& reader,
    const RuntimeFingerprintV2& fingerprint, const DurableCheckpointStats& stats,
    const DurableLimits& limits) {
    auto prepared = prepare_import(engine, target, reader, fingerprint, stats, limits);
    if (prepared.prepared) { return prepared.prepared->commit(); }
    DurableImportState state;
    switch (prepared.state) {
    case DurablePrepareState::CapacityRefused: state = DurableImportState::CapacityRefused; break;
    case DurablePrepareState::Corrupt: state = DurableImportState::Corrupt; break;
    case DurablePrepareState::IdentityMismatch: state = DurableImportState::IdentityMismatch; break;
    case DurablePrepareState::Unavailable: state = DurableImportState::Unavailable; break;
    case DurablePrepareState::PolicyDisabled: state = DurableImportState::PolicyDisabled; break;
    case DurablePrepareState::Prepared: std::terminate();
    }
    return {state, prepared.reason};
}
} // namespace runtime

} // namespace ninfer
