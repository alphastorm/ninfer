// Host-native EngineCore lifecycle contracts. A scripted model contract drives the production
// Engine worker, Scheduler, ResourceManager, durable checkpoint adapter and response delivery
// without a GPU, artifact or CUDA work. Native physical behavior belongs to the model's own
// transaction tests.
#include "runtime/engine/engine_core.h"

#include "cuda_interpose.h"
#include "runtime/contract/continuation_checkpoint.h"
#include "runtime/engine/context_cache/context_cost.h"
#include "serve/automatic_checkpoint_queue.h"
#include "text/grammar.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

// DeviceContext only binds the Engine worker here. A GPU-less host still links the CUDA runtime;
// these entry points present one device without submitting any CUDA work. Names are expanded
// before wrapping so a runtime header's versioned symbol selection is wrapped as linked.
#define NINFER_ENGINE_CORE_TEST_CUDA(name) NINFER_TEST_CUDA_WRAP(name)

extern "C" {

cudaError_t CUDARTAPI NINFER_ENGINE_CORE_TEST_CUDA(cudaGetDeviceCount)(int* count) {
    *count = 1;
    return cudaSuccess;
}

cudaError_t CUDARTAPI NINFER_ENGINE_CORE_TEST_CUDA(cudaSetDevice)(int) { return cudaSuccess; }

cudaError_t CUDARTAPI NINFER_ENGINE_CORE_TEST_CUDA(cudaSetDeviceFlags)(unsigned int) {
    return cudaSuccess;
}

cudaError_t CUDARTAPI NINFER_ENGINE_CORE_TEST_CUDA(cudaGetDeviceProperties)(
    cudaDeviceProp* properties, int) {
    *properties = cudaDeviceProp{};
    return cudaSuccess;
}

cudaError_t CUDARTAPI NINFER_ENGINE_CORE_TEST_CUDA(cudaStreamCreateWithFlags)(
    cudaStream_t* stream, unsigned int) {
    static std::array<unsigned char, 2> streams{};
    static std::size_t created = 0;
    *stream = reinterpret_cast<cudaStream_t>(&streams[created++ % streams.size()]);
    return cudaSuccess;
}

cudaError_t CUDARTAPI NINFER_ENGINE_CORE_TEST_CUDA(cudaStreamDestroy)(cudaStream_t) {
    return cudaSuccess;
}

} // extern "C"

namespace {

namespace rt = ninfer::runtime;
using ninfer::FinishReason;
using ninfer::RequestError;
using ninfer::RequestErrorKind;
using ninfer::TokenId;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr TokenId kGeneratedToken     = 7;
constexpr TokenId kControlToken       = 11;
constexpr std::uint32_t kPrefillChunk = 1024;
constexpr std::uint32_t kPageGroups   = 64;
// Every scripted checkpoint, import reservation destination and staged export copy occupies
// this much counted Host memory.
constexpr std::size_t kCheckpointHostBytes = 4096;
constexpr std::size_t kHostCapacityBytes   = 1U << 20;
constexpr std::uint32_t kDurableFrontier   = 4;
// Fork #102 fixtures: a short reusable prefix, a resident prompt long enough never to finish
// before the test releases it, the page groups one arrival's binding needs, and the bounded
// priority attempts each fresh request may receive.
constexpr std::uint32_t kSourceFrontier    = 3;
constexpr std::uint32_t kLongPrompt        = 8192;
constexpr std::uint32_t kLongContext       = 16384;
constexpr std::uint32_t kArrivalPageGroups = 2;
constexpr std::uint32_t kPriorityAttempts  = 4;
constexpr auto kSession                    = "session";
// A publication that correctly precedes its result is held this long, while no observer can
// return; a result delivered first is read by its observer within this interval.
constexpr auto kPublicationHold = 500ms;
constexpr auto kEventTimeout    = 10s;

void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

class StderrCapture {
public:
    StderrCapture() : file_(std::tmpfile()) {
        require(file_ != nullptr, "could not create stderr capture");
        std::fflush(stderr);
#if defined(_WIN32)
        original_ = _dup(_fileno(stderr));
        const auto result = _dup2(_fileno(file_), _fileno(stderr));
#else
        original_ = dup(fileno(stderr));
        const auto result = dup2(fileno(file_), fileno(stderr));
#endif
        require(original_ >= 0 && result >= 0, "could not redirect stderr");
    }
    ~StderrCapture() {
        std::fflush(stderr);
#if defined(_WIN32)
        _dup2(original_, _fileno(stderr));
        _close(original_);
#else
        dup2(original_, fileno(stderr));
        close(original_);
#endif
        std::fclose(file_);
    }
    std::string text() {
        std::fflush(stderr);
        std::rewind(file_);
        std::string result;
        std::array<char, 1024> bytes;
        while (const auto count = std::fread(bytes.data(), 1, bytes.size(), file_))
            result.append(bytes.data(), count);
        return result;
    }
private:
    std::FILE* file_;
    int original_ = -1;
};

// An observed change arms the gate: the observed request's planning state is released immediately
// before it becomes terminal, or a checkpoint operation changes native state. The next statistics
// publication then waits until the observer has read statistics. A result delivered before
// publication lets its observer read the earlier snapshot.
class PublicationGate {
public:
    void changed() {
        std::lock_guard lock(mutex_);
        armed_         = true;
        consumer_read_ = false;
    }

    void hold_publication() {
        std::unique_lock lock(mutex_);
        if (!std::exchange(armed_, false)) { return; }
        (void)read_.wait_for(lock, kPublicationHold, [&] { return consumer_read_; });
    }

    void consumer_read() {
        {
            std::lock_guard lock(mutex_);
            consumer_read_ = true;
        }
        read_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable read_;
    bool armed_         = false;
    bool consumer_read_ = false;
};

// Worker-side milestones the scenarios wait for before acting.
class Events {
public:
    struct State {
        std::uint32_t planned = 0;
        std::uint32_t bound   = 0;
        bool binding_held     = false;
        bool pause_started    = false;
        bool paused           = false;
        bool pressure_progress = false;
        bool export_blocked   = false;
        std::uint32_t pressure_saves = 0;
        bool pressure_save_named = false;
        std::vector<std::uint32_t> pause_lanes;
        bool host_pause_saved = false;
        // ESAVE per-site cases: a held operation started; the worker-side order, while traced, of
        // decode rounds (D), operation starts (b binding, p pause) and completions (B, P), durable
        // prepares (E export, I import) and markers the test records.
        bool held_operation = false;
        std::string trace;
        bool victim_finished  = false;
        bool pressure_finished_before_victim = false;
        bool decode_held = false;
        bool decode_released = false;
        std::uint32_t decode_rounds = 0;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> prefill_steps;
        // Context-operation boundary fixtures (fork #102).
        std::uint32_t demotes          = 0;
        std::uint32_t demoting_steps   = 0;
        std::uint32_t state_binds      = 0;
        std::uint32_t credit_shortages = 0;
        // Demotions started when a held arrival's binding completed and when the latest export
        // was prepared, and whether that preparation followed the held binding's completion.
        std::uint32_t demotes_at_held_binding = 0;
        std::uint32_t demotes_at_prepare      = 0;
        bool held_binding_completed           = false;
        bool prepared_after_held_binding      = false;
        // A HeldDemotionArrival's own demotion started; exports prepared so far, and how many
        // had been prepared when that arrival's binding started.
        bool arrival_demotion_started         = false;
        std::uint32_t prepares                = 0;
        std::uint32_t prepares_at_arrival_bind = 0;
    };

    template <class Update>
    void record(Update update) {
        {
            std::lock_guard lock(mutex_);
            update(state_);
        }
        changed_.notify_all();
    }

    template <class Predicate>
    void await(Predicate predicate, const char* milestone) {
        std::unique_lock lock(mutex_);
        require(changed_.wait_for(lock, kEventTimeout, [&] { return predicate(state_); }),
                std::string("worker did not reach ") + milestone);
    }

    [[nodiscard]] State snapshot() {
        std::lock_guard lock(mutex_);
        return state_;
    }

    // Stalled prefills make no progress until released.
    std::atomic<bool> stall_released{false};
    // A LaneHold prefill makes no progress until released.
    std::atomic<bool> hold_released{false};

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    State state_;
};

enum class Script : std::uint8_t {
    Complete,        // One prefill chunk; the first committed token reaches the output limit.
    Stall,           // Prefill makes no progress until released, then completes.
    HoldBinding,     // Binding stays in flight until cancellation aborts it.
    InvalidBinding,  // Binding publishes without a sequence; Engine must deliver its error.
    ControlRejected, // The first token requests target control; its preview violates grammar.
    DeadEnd,         // The first token's constraint mask has no legal token.
    DecodeFailure,   // Decode raises an internal error that fails the Engine.
    NumericalPrefill,
    NumericalDecode,
    OutOfDomainDecode,
    IsolatedMiss,    // The first legal unit never fits although total capacity could hold it.
    StateSlotMiss,   // No Device state can be reclaimed for an isolated request.
    Pressure,        // Stall; its unit cannot be reserved while a victim is active.
    PressureProgress, // Pressure owner commits one prefill unit, then stalls.
    VictimHold,      // Stall; its pause stays in flight until cancellation aborts it.
    VictimPause,     // Stall; its pause publishes Replay state, whose restore never fits.
    VictimResume,    // Stall; can pause and restore, for younger-owner handoff.
    ExportPressure, // Stall; released units need the capacity pinned by a checkpoint export.
    PacingDecode,   // Hold the first decode until the test has submitted its neighbour.
    PacingPrefill,  // Chunked text work, recording executed caps and intervening decode rounds.
    HistorySeed,     // Publishes a catalog checkpoint whose KV pages can always be demoted.
    DemotingPrefill, // One token per step; each unit needs a page group that a demotion frees.
    CreditArrival,   // Binding needs two freed page groups, so its own demotion must run.
    StateArrival,    // Binding needs a Device state that no reclaim can supply.
    SourceSeed,      // Publishes the short reusable prefix.
    SourceArrival,   // Prefers the SourceSeed prefix.
    LaneHold,        // Occupies its lane without progress until hold_released.
    HeldArrival,     // Binding stays in flight until hold_released, then publishes.
    HeldDemotionArrival, // Binds only after its own demotion, held until hold_released, ends.
};

[[nodiscard]] bool stalls(Script script) noexcept {
    return script == Script::Stall || script == Script::DecodeFailure ||
           script == Script::Pressure || script == Script::PressureProgress ||
           script == Script::VictimHold ||
           script == Script::VictimPause || script == Script::VictimResume ||
           script == Script::ExportPressure;
}

[[nodiscard]] bool victim(Script script) noexcept {
    return script == Script::VictimHold || script == Script::VictimPause ||
           script == Script::VictimResume;
}

using Resources = rt::ContextResources<1>;

struct Prompt {
    Script script        = Script::Complete;
    std::uint32_t tokens = 4;
    std::shared_ptr<PublicationGate> observed;
    // A durable session turn publishes its completed continuation as a checkpoint.
    bool publish_continuation = false;
};

struct SessionKey {
    std::array<char, 128> bytes{};
    std::uint16_t size = 0;

    friend bool operator==(const SessionKey&, const SessionKey&) = default;
};

class Plan {
public:
    struct Advice {
        bool update_session_index = false;
        std::optional<SessionKey> session_key;
        std::optional<SessionKey> reuse_scope;
        std::optional<SessionKey> shared_reuse_scope;
    };

    Plan(Script script, rt::RequestPlanSummary summary, std::shared_ptr<PublicationGate> observed)
        : script_(script), summary_(summary), observed_(std::move(observed)) {}

    Plan(Plan&&) noexcept            = default;
    Plan& operator=(Plan&&) noexcept = delete;
    Plan(const Plan&)                = delete;
    Plan& operator=(const Plan&)     = delete;

    ~Plan() {
        if (observed_) { observed_->changed(); }
    }

    [[nodiscard]] Script script() const noexcept { return script_; }

    [[nodiscard]] const rt::RequestPlanSummary& summary() const noexcept { return summary_; }

    [[nodiscard]] const Advice& context_cache() const noexcept { return advice_; }

    [[nodiscard]] std::vector<std::uint32_t> capture_frontiers() const { return {}; }

    [[nodiscard]] std::optional<std::uint64_t>
    prefix_shortlist_key(std::uint32_t frontier) const noexcept {
        if (script_ == Script::SourceArrival && frontier == kSourceFrontier) { return frontier; }
        return std::nullopt;
    }

private:
    Script script_;
    rt::RequestPlanSummary summary_;
    Advice advice_;
    std::shared_ptr<PublicationGate> observed_;
};

struct Sequence {
    std::uint32_t lane  = 0;
    std::uint64_t epoch = 0;

    friend bool operator==(Sequence, Sequence) noexcept = default;
};

struct Checkpoint {
    std::uint32_t index = 0;

    friend bool operator==(Checkpoint, Checkpoint) noexcept = default;
};

struct Source {
    std::optional<Checkpoint> checkpoint;
    std::uint32_t reused_tokens = 0;
    rt::PrefillWork remaining_work;
    std::vector<rt::ContextTransferRequirement> transfers;
    bool consume_source = false;
    bool take_private   = false;
    std::vector<Checkpoint> private_points;
    std::vector<Checkpoint> retired_points;
};

class Resume {
public:
    explicit Resume(std::uint32_t frontier) noexcept : frontier_(frontier) {}

    [[nodiscard]] bool has_snapshot() const noexcept { return false; }

    [[nodiscard]] std::optional<Checkpoint> snapshot_handle() const noexcept {
        return std::nullopt;
    }

    [[nodiscard]] std::uint32_t frontier() const noexcept { return frontier_; }

private:
    std::uint32_t frontier_ = 0;
};

enum class UnitKind : std::uint8_t { Prefill, Replay, Decode, Control };

struct Unit {
    Sequence sequence;
    UnitKind kind        = UnitKind::Decode;
    std::uint32_t tokens = 0;
};

class PendingBatch {
public:
    PendingBatch() = default;

    PendingBatch(std::span<const Sequence> sequences, bool constraint_failed)
        : sequences_(sequences.begin(), sequences.end()),
          tokens_(sequences.size(), kGeneratedToken),
          constraint_failed_(sequences.size(), constraint_failed) {}

    [[nodiscard]] std::size_t row_count() const noexcept { return sequences_.size(); }

    [[nodiscard]] std::uint32_t row_stride() const noexcept { return 1; }

    [[nodiscard]] std::span<const std::int32_t> row_counts() const noexcept { return {}; }

    [[nodiscard]] std::span<const TokenId> tokens() const noexcept { return tokens_; }

    [[nodiscard]] std::optional<RequestErrorKind> failure(std::size_t row) const {
        if (numerical_failed_[row]) return RequestErrorKind::NumericalFailure;
        return constraint_failed_.at(row) ? std::optional(RequestErrorKind::ConstraintDeadEnd)
                                         : std::nullopt;
    }

    void inject_numerical(std::size_t row, TokenId sampled) {
        require(sampled < 0 || sampled >= 256, "fixture injection was a valid token");
        tokens_[row] = 0;
        numerical_failed_[row] = true;
    }

    void inject_nonfinite(std::size_t row, float logit) {
        require(!std::isfinite(logit), "fixture injection was finite");
        inject_numerical(row, -1);
    }

    [[nodiscard]] rt::ExecutionTiming execution_timing() const noexcept { return {}; }

    [[nodiscard]] Sequence sequence(std::size_t row) const { return sequences_.at(row); }

private:
    std::vector<Sequence> sequences_;
    std::vector<TokenId> tokens_;
    std::vector<unsigned char> constraint_failed_;
    std::array<bool, ninfer::kMaximumConcurrency> numerical_failed_{};
};

struct PrefillProgress {
    rt::BeginSummary summary;
    std::uint32_t processed_prompt_tokens = 0;
    bool complete                         = false;
    rt::ExecutionTiming timing;
    std::optional<PendingBatch> pending;
    bool capture_ready = false;
};

struct ReplayProgress {
    bool capture_ready             = false;
    std::uint32_t processed_tokens = 0;
    bool complete                  = false;
    rt::ExecutionTiming timing;
};

struct CommitRow {
    rt::CommitDisposition disposition = rt::CommitDisposition::Active;
    ninfer::GenerationTimings timings;
    ninfer::SpeculativeStats speculative;

    struct SpeculativeCounters {
        std::uint64_t rounds          = 0;
        std::uint64_t drafted_tokens  = 0;
        std::uint64_t accepted_tokens = 0;
        std::uint64_t fallback_steps  = 0;
    } speculative_counters;
};

struct CommitResult {
    std::array<CommitRow, ninfer::kMaximumConcurrency> rows{};
    std::array<bool, ninfer::kMaximumConcurrency> capture_ready{};
    std::size_t row_count = 0;
    rt::ExecutionTiming timing;
};

struct DiscardResult {
    rt::ConsumeStatus status = rt::ConsumeStatus::InvariantMismatch;
    std::size_t row_count    = 0;
};

struct FinishResult {
    rt::ConsumeStatus status = rt::ConsumeStatus::InvariantMismatch;
    ninfer::GenerationTimings timings;
    ninfer::SpeculativeStats speculative;
    std::optional<Checkpoint> checkpoint;
};

struct AbortResult {
    rt::ConsumeStatus status = rt::ConsumeStatus::InvariantMismatch;
    ninfer::GenerationTimings timings;
    ninfer::SpeculativeStats speculative;
};

struct CapturePreparation {
    std::uint32_t frontier = 0;
    bool reserved          = false;
    Resources shortage;
    ninfer::HostAllocationRun host_run;
};

enum class ContextKind : std::uint8_t { Bind, Capture, Demote, Pause };

struct ContextProgress {
    ContextKind kind = ContextKind::Bind;
    bool complete    = false;
    bool published   = false;
    std::optional<Sequence> sequence;
    std::optional<Resume> paused;
    bool replaying = false;
    std::vector<Checkpoint> private_points;
    std::vector<Checkpoint> retired_checkpoints;
    std::vector<Checkpoint> captured_checkpoints;
    std::vector<rt::ContextTransferObservation> transfers;
    rt::ContextOperationCounts operations;
    std::optional<ninfer::GenerationTimings> request_timings;
    ninfer::SpeculativeStats request_speculative;
};

struct BindingReservation {
    bool reserved          = false;
    bool source_valid      = true;
    bool capacity_possible = true;
    Resources shortage;
    std::vector<Checkpoint> retired_points;
    std::optional<Checkpoint> consumed_source;

    explicit operator bool() const noexcept { return reserved; }
};

struct PhysicalUsage {
    Resources occupied;
    Resources capacity;
    std::size_t host_reserved_bytes      = 0;
    std::size_t host_peak_occupied_bytes = 0;
    std::uint32_t host_state_slots       = 0;
    std::size_t host_kv_bytes            = 0;
};

struct Metadata {
    std::uint32_t frontier = 0;
    rt::CheckpointRole role = rt::CheckpointRole::Continuation;
    bool leased             = false;
};

struct Demotion {
    Checkpoint handle;
    ninfer::HostAllocationRun host_run;
    std::vector<Checkpoint> sources;
    Resources released;
};

struct Release {
    std::vector<Checkpoint> sources;
    Resources released;
};

// Native release quotes are backed by the same per-record Host ledger used by stats.
struct ReclaimPlan {
    struct Batch {
        bool append(const Demotion&) { return false; }

        [[nodiscard]] bool covers(const Release&) const { return false; }

        [[nodiscard]] std::optional<Demotion> finish() const { return std::nullopt; }
    };

    std::vector<Demotion> demotions;
    std::vector<Release> releases;

    [[nodiscard]] Batch begin_kv_batch(Resources) const { return {}; }

    [[nodiscard]] std::uint64_t recovery_loss(std::span<const Checkpoint>,
                                              std::span<const Checkpoint>) const {
        return 0;
    }
};

// What the one-point scripted session checkpoint restores; an import's expected statistics.
[[nodiscard]] rt::DurableCheckpointStats durable_stats() noexcept {
    return {.frontier_tokens = kDurableFrontier,
            .restored_tokens = kDurableFrontier,
            .payload_bytes   = 1,
            .points          = 1,
            .states          = 1};
}

// Scripted native ownership: lanes hold sequences, one context operation is in flight at a time,
// and every sequence is released exactly once by commit, finish, abort or its context operation.
// Durable session turns publish their completed continuations as checkpoints. Each change a
// checkpoint operation answers for (an import reserved, committed or aborted, an export lease
// retired) arms the publication gate.
class Program {
public:
    using ContextResources = Resources;

    Program(Events& events, std::shared_ptr<PublicationGate> gate)
        : events_(events), gate_(std::move(gate)) {}

    [[nodiscard]] std::span<const rt::KvPoolDescriptor> kv_pools() const noexcept {
        return pools_;
    }

    [[nodiscard]] Plan plan_request(Prompt&& prompt, const rt::ResolvedExecutionOptions& options) {
        const rt::RequestPlanSummary summary{
            .prompt_tokens           = prompt.tokens,
            .requested_output_tokens = options.requested_output_tokens,
            .effective_output_tokens = options.requested_output_tokens,
            .effective_limit_reason  = FinishReason::OutputLimit,
            .publish_continuation    = prompt.publish_continuation,
        };
        events_.record([](Events::State& state) { ++state.planned; });
        return Plan(prompt.script, summary, std::move(prompt.observed));
    }

    [[nodiscard]] std::optional<Source> inspect_source(const Plan& base,
                                                       std::optional<Checkpoint> checkpoint,
                                                       bool = false,
                                                       std::span<const Checkpoint> = {},
                                                       std::span<const Checkpoint> = {}) const {
        if (checkpoint && (!valid_checkpoint(*checkpoint) || base.script() != Script::SourceArrival ||
                           !live_record(*checkpoint).source_prefix)) {
            return std::nullopt;
        }
        Source source{.checkpoint    = checkpoint,
                      .reused_tokens = checkpoint ? live_record(*checkpoint).frontier : 0};
        source.remaining_work = rt::make_prefill_work(
            source.reused_tokens, base.summary().prompt_tokens - source.reused_tokens, 0, 0,
            kPrefillChunk);
        return source;
    }

    [[nodiscard]] BindingReservation start_binding(const Plan& base, rt::LaneId lane,
                                                   const Source& source, Resume* resume = nullptr,
                                                   UnitKind = UnitKind::Decode,
                                                   std::uint32_t = 1) {
        require(!operation_ && !lanes_.at(lane.value).active &&
                    (!source.checkpoint || base.script() == Script::SourceArrival),
                "scripted binding overlaps native ownership");
        if (base.script() == Script::IsolatedMiss ||
            (resume != nullptr && base.script() == Script::VictimPause)) {
            return {.capacity_possible = true, .shortage = {.kv_page_groups = {1}}};
        }
        if (base.script() == Script::StateSlotMiss) {
            return {.capacity_possible = true, .shortage = {.state_slots = 1}};
        }
        if (base.script() == Script::StateArrival) {
            events_.record([](Events::State& state) { ++state.state_binds; });
            return {.capacity_possible = true, .shortage = {.state_slots = 1}};
        }
        if (base.script() == Script::CreditArrival) {
            if (credits_ < kArrivalPageGroups) {
                events_.record([](Events::State& state) { ++state.credit_shortages; });
                return {.capacity_possible = true,
                        .shortage = {.kv_page_groups = {kArrivalPageGroups - credits_}}};
            }
            credits_ -= kArrivalPageGroups;
        }
        if (base.script() == Script::HeldDemotionArrival) {
            if (!arrival_demotion_done_) {
                // Its own reclaim starts the next demotion, which frees what it needs.
                arrival_demotion_requested_ = true;
                return {.capacity_possible = true, .shortage = {.kv_page_groups = {1}}};
            }
            events_.record(
                [](Events::State& state) { state.prepares_at_arrival_bind = state.prepares; });
        }
        operation_ = Operation{.kind          = ContextKind::Bind,
                               .lane          = lane.value,
                               .script        = base.script(),
                               .prompt_tokens = base.summary().prompt_tokens,
                               .reused_tokens = source.reused_tokens,
                               .publish       = base.summary().publish_continuation};
        if (base.script() == Script::HoldBinding || base.script() == Script::HeldArrival) {
            events_.record([](Events::State& state) { state.binding_held = true; });
        }
        operation_started('b');
        return {.reserved = true};
    }

    [[nodiscard]] ContextProgress poll_context(rt::CancellationFlagView cancellation) {
        require(operation_.has_value(), "context progress has no operation");
        const auto operation = *operation_;
        const bool held = (operation.kind == ContextKind::Bind &&
                           (operation.script == Script::HoldBinding ||
                            (operation.script == Script::HeldArrival &&
                             !events_.hold_released.load()))) ||
                          (operation.kind == ContextKind::Pause &&
                           operation.script == Script::VictimHold) ||
                          (operation.kind == ContextKind::Demote &&
                           operation.script == Script::HeldDemotionArrival &&
                           !events_.hold_released.load());
        ContextProgress progress;
        progress.kind = operation.kind;
        // ESAVE: a released hold completes at the first poll after a decode round (mid-turn).
        if (held_until_release_ && !decoded_after_release_ && !cancellation.requested()) {
            return progress;
        }
        if (held && !cancellation.requested()) { return progress; }
        operation_.reset();
        operation_completed(operation.kind);
        progress.complete = true;
        if (operation.kind == ContextKind::Demote) {
            // The demoted history keeps its catalog entry; one Device page group is now free.
            ++credits_;
            if (operation.script == Script::HeldDemotionArrival) { arrival_demotion_done_ = true; }
            progress.published = true;
            return progress;
        }
        auto& lane        = lanes_.at(operation.lane);
        if (operation.kind == ContextKind::Bind) {
            if (held) { return progress; }
            if (operation.script == Script::HeldArrival) {
                events_.record([](Events::State& state) {
                    state.held_binding_completed  = true;
                    state.demotes_at_held_binding = state.demotes;
                });
            }
            lane = Lane{.active        = true,
                        .script        = operation.script,
                        .epoch         = next_epoch_++,
                        .prompt_tokens = operation.prompt_tokens,
                        .reused_tokens = operation.reused_tokens,
                        .publish       = operation.publish};
            progress.published = true;
            if (operation.script != Script::InvalidBinding) {
                progress.sequence = Sequence{operation.lane, lane.epoch};
            }
            events_.record([](Events::State& state) { ++state.bound; });
            return progress;
        }
        lane = Lane{};
        if (!held) {
            progress.published = true;
            progress.paused.emplace(operation.prompt_tokens);
            events_.record([](Events::State& state) { state.paused = true; });
        }
        return progress;
    }

    [[nodiscard]] bool has_context_operation() const noexcept { return operation_.has_value(); }

    [[nodiscard]] bool context_blocks(Sequence sequence) const noexcept {
        return operation_ && operation_->kind == ContextKind::Pause &&
               operation_->lane == sequence.lane;
    }

    [[nodiscard]] bool recovery_pending(Sequence) const noexcept { return false; }

    [[nodiscard]] rt::ResourceReservation<Resources> reserve_units(std::span<const Unit> units) {
        require(units.size() == 1, "Engine reserves one unit per row");
        if (live(units.front().sequence).script == Script::ExportPressure &&
            staged_exports_ != 0) {
            events_.record([](Events::State& state) { state.export_blocked = true; });
            return {.shortage = {.kv_page_groups = {1}}};
        }
        if ((live(units.front().sequence).script == Script::Pressure ||
             live(units.front().sequence).script == Script::PressureProgress) &&
            std::any_of(lanes_.begin(), lanes_.end(),
                        [](const Lane& lane) { return lane.active && victim(lane.script); })) {
            return {.shortage = {.kv_page_groups = {1}}};
        }
        if (auto& lane = live(units.front().sequence);
            lane.script == Script::DemotingPrefill && !lane.permit &&
            !events_.stall_released.load()) {
            // Each prefill unit needs one Device page group that only a demotion frees.
            if (credits_ == 0) { return {.shortage = {.kv_page_groups = {1}}}; }
            --credits_;
        }
        live(units.front().sequence).permit = true;
        return {.reserved = true};
    }

    void release_units(std::span<const Sequence> sequences) noexcept {
        for (const auto sequence : sequences) {
            if (sequence.lane < lanes_.size() && lanes_[sequence.lane].active &&
                lanes_[sequence.lane].epoch == sequence.epoch) {
                lanes_[sequence.lane].permit = false;
            }
        }
    }

    [[nodiscard]] PrefillProgress advance_prefill(Sequence sequence,
                                                  rt::ExecutionTiming* = nullptr,
                                                  rt::TokenMaskProvider* = nullptr,
                                                  std::uint32_t max_tokens = 0) {
        auto& lane = live(sequence);
        require(lane.permit, "prefill consumed a released unit permit");
        if (lane.script == Script::DemotingPrefill && !events_.stall_released.load()) {
            // Never reaches its prompt end before release, so every step needs a new unit.
            if (lane.prefilled + 1 >= lane.prompt_tokens) { return {}; }
            std::this_thread::sleep_for(1ms);
            ++lane.prefilled;
            lane.permit = false;
            events_.record([](Events::State& state) { ++state.demoting_steps; });
            return {.summary = {.prompt_tokens = lane.prompt_tokens},
                    .processed_prompt_tokens = 1};
        }
        if (lane.script == Script::LaneHold && !events_.hold_released.load()) { return {}; }
        if (lane.script == Script::PressureProgress && !lane.partial_progress) {
            if (!events_.snapshot().paused) return {};
            // Service is charged from real wall time. One bounded unit crosses the real
            // Scheduler quantum; later polls make no progress until the test releases them.
            std::this_thread::sleep_for(1100ms);
            lane.partial_progress = true;
            events_.record([](Events::State& state) { state.pressure_progress = true; });
            return {.summary = {.prompt_tokens = lane.prompt_tokens},
                    .processed_prompt_tokens = 1};
        }
        if (stalls(lane.script) && !events_.stall_released.load()) { return {}; }
        PrefillProgress progress;
        progress.summary = {.prompt_tokens        = lane.prompt_tokens,
                            .reused_prompt_tokens = lane.reused_tokens,
                            .prefix_reuse_path    = lane.reused_tokens
                                                        ? ninfer::PrefixReusePath::Checkpoint
                                                        : ninfer::PrefixReusePath::Root};
        progress.processed_prompt_tokens = lane.prompt_tokens - lane.reused_tokens -
                                           lane.prefilled - (lane.partial_progress ? 1U : 0U);
        if (lane.script == Script::PacingPrefill) {
            const auto prefix = lane.prefilled;
            progress.processed_prompt_tokens = std::min(
                lane.prompt_tokens - lane.prefilled,
                max_tokens ? std::min(max_tokens, kPrefillChunk) : kPrefillChunk);
            lane.prefilled += progress.processed_prompt_tokens;
            lane.permit = false;
            events_.record([&](Events::State& state) {
                state.prefill_steps.emplace_back(progress.processed_prompt_tokens,
                                                  state.decode_rounds);
            });
            // Deterministic timing independent of the host's clock or thread scheduling.
            progress.timing.device_wait_ns =
                rt::generic_context_machine_cost_model().prefill_ns(rt::make_prefill_work(
                    prefix, progress.processed_prompt_tokens, 0, 0,
                    progress.processed_prompt_tokens));
            if (lane.prefilled != lane.prompt_tokens) return progress;
        }
        progress.complete                = true;
        progress.pending.emplace(std::span<const Sequence>(&sequence, 1),
                                 lane.script == Script::DeadEnd);
        if (lane.script == Script::NumericalPrefill)
            progress.pending->inject_nonfinite(0, std::numeric_limits<float>::quiet_NaN());
        return progress;
    }

    [[nodiscard]] ReplayProgress advance_replay(Sequence, rt::ExecutionTiming* = nullptr) {
        throw std::logic_error("scripted Program has no Replay");
    }

    [[nodiscard]] PendingBatch decode(std::span<const Sequence> sequences,
                                      std::span<const rt::RoundBudget>,
                                      rt::ExecutionTiming* = nullptr,
                                      rt::TokenMaskProvider* = nullptr) {
        if (held_until_release_ && release_held_operation) { decoded_after_release_ = true; }
        trace('D');
        for (const auto sequence : sequences) {
            require(live(sequence).permit, "decode consumed a released unit permit");
            if (live(sequence).script == Script::PacingDecode) {
                events_.record([](Events::State& state) { state.decode_held = true; });
                events_.await([](const Events::State& state) { return state.decode_released; },
                              "the test's decode release");
                events_.record([](Events::State& state) { ++state.decode_rounds; });
            }
            if (live(sequence).script == Script::DecodeFailure) {
                throw std::logic_error("injected decode failure");
            }
        }
        PendingBatch pending(sequences, false);
        for (std::size_t row = 0; row < sequences.size(); ++row) {
            const auto script = live(sequences[row]).script;
            if (script == Script::NumericalDecode)
                pending.inject_nonfinite(row, std::numeric_limits<float>::infinity());
            if (script == Script::OutOfDomainDecode) pending.inject_numerical(row, 256);
        }
        return pending;
    }

    [[nodiscard]] rt::ExecutionTiming
    append_forced_tokens(std::span<const Sequence> sequences, std::span<const TokenId>,
                         std::uint32_t, std::span<const std::optional<std::uint32_t>>,
                         rt::ExecutionTiming* = nullptr) {
        for (const auto sequence : sequences) { (void)live(sequence); }
        return {};
    }

    [[nodiscard]] CommitResult commit(PendingBatch&& pending,
                                      std::span<const rt::CommitDecision> decisions,
                                      rt::CommitObservation = rt::CommitObservation::AllRows,
                                      rt::ExecutionTiming* = nullptr) {
        require(decisions.size() == pending.row_count(), "commit is not row aligned");
        CommitResult result;
        result.row_count = pending.row_count();
        for (std::size_t row = 0; row < pending.row_count(); ++row) {
            const auto sequence = pending.sequence(row);
            live(sequence).permit = false;
            const auto& decision = decisions[row];
            using Disposition    = rt::CommitDisposition;
            result.rows[row].disposition = decision.failed      ? Disposition::FailedReleased
                                           : decision.cancelled ? Disposition::CancelledReleased
                                           : decision.terminal  ? Disposition::Finishable
                                                                : Disposition::Active;
            if (decision.failed || decision.cancelled) { lanes_.at(sequence.lane) = Lane{}; }
        }
        return result;
    }

    [[nodiscard]] DiscardResult abort_pending(PendingBatch&& pending) noexcept {
        return {.status = rt::ConsumeStatus::Consumed, .row_count = pending.row_count()};
    }

    [[nodiscard]] FinishResult finish(Sequence sequence) noexcept {
        FinishResult result;
        if (sequence.lane < lanes_.size() && lanes_[sequence.lane].active &&
            lanes_[sequence.lane].epoch == sequence.epoch) {
            const auto script = lanes_[sequence.lane].script;
            events_.record([&](Events::State& state) {
                if (script == Script::Pressure)
                    state.pressure_finished_before_victim = !state.victim_finished;
                if (victim(script)) state.victim_finished = true;
            });
        }
        if (sequence.lane < lanes_.size() && lanes_[sequence.lane].active &&
            lanes_[sequence.lane].epoch == sequence.epoch && lanes_[sequence.lane].publish) {
            result.checkpoint = publish_checkpoint(lanes_[sequence.lane].prompt_tokens);
            auto& record         = records_[result.checkpoint->index];
            record.history       = lanes_[sequence.lane].script == Script::HistorySeed;
            record.source_prefix = lanes_[sequence.lane].script == Script::SourceSeed;
        }
        result.status = release(sequence);
        return result;
    }

    [[nodiscard]] AbortResult abort(Sequence sequence) noexcept {
        return {.status = release(sequence)};
    }

    void fail_all_cleanup() noexcept {
        operation_.reset();
        lanes_ = {};
    }

    [[nodiscard]] PhysicalUsage physical_usage() const noexcept {
        gate_->hold_publication();
        PhysicalUsage usage;
        usage.capacity = {.state_slots    = ninfer::kMaximumConcurrency,
                          .kv_page_groups = {kPageGroups},
                          .host_bytes     = host_capacity};
        for (const auto& lane : lanes_) {
            usage.occupied.state_slots += lane.active ? 1U : 0U;
            usage.occupied.kv_page_groups[0] += lane.active ? 1U : 0U;
        }
        std::size_t checkpoint_bytes = 0;
        for (const auto& record : records_) if (record.valid) checkpoint_bytes += record.host_bytes;
        usage.host_reserved_bytes = reserved_points_ * kCheckpointHostBytes;
        usage.occupied.host_bytes =
            checkpoint_bytes + staged_exports_ * kCheckpointHostBytes + usage.host_reserved_bytes +
            redundant_bytes + retained_extent_bytes_;
        return usage;
    }

    [[nodiscard]] bool host_can_place(std::span<const ninfer::HostAllocationRun> runs,
                                       std::span<const Checkpoint> released = {}) const {
        const auto usage = physical_usage();
        return ninfer::total_bytes(runs) <= usage.capacity.host_bytes - usage.occupied.host_bytes +
                                              host_bytes_released(released);
    }

    [[nodiscard]] std::optional<std::vector<ninfer::HostAllocationRun>>
    pause_host_runs(Sequence sequence) const {
        if (pause_needs_host && live(sequence).script == Script::VictimResume) {
            return std::vector<ninfer::HostAllocationRun>{{kCheckpointHostBytes, 1}};
        }
        return std::nullopt;
    }

    [[nodiscard]] std::size_t release_redundant_host(std::span<const Checkpoint>,
                                                     std::optional<Sequence> = std::nullopt) {
        return std::exchange(redundant_bytes, 0);
    }

    [[nodiscard]] bool start_pause(Sequence sequence, bool save_snapshot,
                                    rt::ExecutionTiming* = nullptr) {
        const auto& lane = live(sequence);
        require(!operation_, "pause overlaps a context operation");
        operation_ = Operation{.kind          = ContextKind::Pause,
                               .lane          = sequence.lane,
                               .script        = lane.script,
                               .prompt_tokens = lane.prompt_tokens};
        events_.record([&](Events::State& state) {
            state.pause_started = true;
            state.pause_lanes.push_back(sequence.lane);
            state.host_pause_saved = save_snapshot;
        });
        operation_started('p');
        return true;
    }

    [[nodiscard]] bool revoke_snapshot(Resume&) noexcept { return false; }

    [[nodiscard]] Resources snapshot_resources(const Resume&) const { return {}; }

    [[nodiscard]] bool reclaim_capture_reservation(Resources) { return false; }

    [[nodiscard]] std::optional<CapturePreparation> prepare_capture(Sequence) {
        return std::nullopt;
    }

    [[nodiscard]] bool capture_is_input(Sequence) const { return false; }

    [[nodiscard]] bool start_capture(Sequence) { return false; }

    void skip_capture(Sequence) {}

    [[nodiscard]] bool valid_checkpoint(Checkpoint checkpoint) const noexcept {
        return checkpoint.index < records_.size() && records_[checkpoint.index].valid;
    }

    // Like native checkpoint storage, a record pinned by an export lease cannot be erased.
    [[nodiscard]] bool release_checkpoint(Checkpoint checkpoint) noexcept {
        if (!valid_checkpoint(checkpoint) || records_[checkpoint.index].pins != 0) { return false; }
        if (zero_arena_gain && checkpoint.index == 1)
            retained_extent_bytes_ += records_[checkpoint.index].host_bytes;
        records_[checkpoint.index].valid = false;
        return true;
    }

    [[nodiscard]] Metadata checkpoint_metadata(Checkpoint checkpoint) const {
        const auto& record = live_record(checkpoint);
        return {.frontier = record.frontier, .leased = record.pins != 0};
    }

    // Distinct checkpoints never share a prefix key, so no request reuses another's checkpoint;
    // only the SourceSeed prefix is keyed by position, where the SourceArrival plan finds it.
    [[nodiscard]] std::uint64_t checkpoint_key(Checkpoint checkpoint,
                                               std::uint32_t position) const {
        if (live_record(checkpoint).source_prefix) { return position; }
        return (std::uint64_t{checkpoint.index} << 32U) | position;
    }

    [[nodiscard]] Resources checkpoint_footprint(std::span<const Checkpoint>) const { return {}; }

    [[nodiscard]] std::uint64_t checkpoint_recovery_loss(std::span<const Checkpoint>,
                                                         std::span<const Checkpoint>) const {
        return 0;
    }

    [[nodiscard]] std::uint32_t checkpoint_recovery_frontier(Checkpoint, const Plan&,
                                                             std::uint32_t) const {
        return 0;
    }

    [[nodiscard]] std::size_t host_bytes_released(std::span<const Checkpoint> points) const {
        std::size_t bytes = 0;
        for (auto point : points) {
            if (!valid_checkpoint(point) || live_record(point).pins) return 0;
            bytes += live_record(point).host_bytes;
        }
        return bytes;
    }

    // A KV shortage may demote one page group of the seeded history, which keeps its catalog
    // entry, so every resident unit can start a demotion (fork #102). In release mode only
    // ordinary releases of other checkpoints are offered, as when Host takes no more demotions.
    [[nodiscard]] ReclaimPlan plan_reclaim(std::span<const Checkpoint> allowed,
                                           std::span<const Checkpoint> excluded,
                                           Resources shortage) const {
        ReclaimPlan plan;
        if (!shortage.kv_page_groups[0]) { return plan; }
        for (const auto point : allowed) {
            if (!valid_checkpoint(point) ||
                std::find(excluded.begin(), excluded.end(), point) != excluded.end()) {
                continue;
            }
            const bool history = records_[point.index].history;
            if (history && !release_named_victim) {
                plan.demotions.push_back(
                    {.handle = point, .sources = {point}, .released = {.kv_page_groups = {1}}});
            } else if (!history && release_named_victim) {
                plan.releases.push_back({.sources = {point}, .released = {.kv_page_groups = {1}}});
            }
        }
        return plan;
    }

    [[nodiscard]] std::vector<Release> plan_releases(std::span<const Checkpoint> allowed,
                                                     std::span<const Checkpoint> excluded,
                                                     Resources shortage) const {
        std::vector<Release> plans;
        if (!shortage.host_bytes) return plans;
        for (auto point : allowed) {
            if (std::find(excluded.begin(), excluded.end(), point) != excluded.end()) continue;
            const auto bytes = host_bytes_released({&point, 1});
            if (bytes) plans.push_back({{point}, {.host_bytes = bytes}});
        }
        return plans;
    }

    [[nodiscard]] bool start_demote(const Demotion& plan) {
        require(!operation_ && valid_checkpoint(plan.handle), "demotion overlaps native ownership");
        // A HeldDemotionArrival's shortage asked for this demotion; it is held like its binding.
        const bool own = std::exchange(arrival_demotion_requested_, false);
        operation_ = Operation{.kind   = ContextKind::Demote,
                               .script = own ? Script::HeldDemotionArrival : Script::Complete};
        events_.record([own](Events::State& state) {
            ++state.demotes;
            state.arrival_demotion_started |= own;
        });
        return true;
    }

    [[nodiscard]] ninfer::MemorySummary memory_summary() const { return {}; }

    void reset_memory_peaks() noexcept {}

    // Host-only import destinations held until commit consumes or destruction releases them.
    class ImportReservation {
    public:
        ImportReservation(Program& program, std::uint32_t points) noexcept
            : program_(&program), points_(points) {}

        // A refused preparation reserves nothing.
        explicit ImportReservation(rt::DurableReason refusal) noexcept : refusal_(refusal) {}

        ImportReservation(ImportReservation&& other) noexcept
            : program_(std::exchange(other.program_, nullptr)), points_(other.points_),
              refusal_(other.refusal_) {}

        ImportReservation& operator=(ImportReservation&&) = delete;

        ~ImportReservation() {
            if (program_ != nullptr) { program_->release_reservation(points_); }
        }

        [[nodiscard]] rt::DurablePrepareState state() const noexcept {
            return refusal_ == rt::DurableReason::None ? rt::DurablePrepareState::Prepared
                                                       : rt::DurablePrepareState::CapacityRefused;
        }

        [[nodiscard]] rt::DurableReason reason() const noexcept { return refusal_; }

        [[nodiscard]] rt::DurableCheckpointStats stats() const noexcept { return durable_stats(); }

    private:
        friend class Program;
        Program* program_     = nullptr;
        std::uint32_t points_ = 0;
        rt::DurableReason refusal_ = rt::DurableReason::None;
    };

    enum class HostKind { Prepared, Busy, NoHostArena, HostCapacity, HostContiguity };
    std::size_t host_capacity = kHostCapacityBytes;
    std::size_t redundant_bytes = 0;
    bool zero_arena_gain = false;
    // ESAVE per-site cases. The next context operation started while armed stays in flight until
    // released; released, it completes at the first poll after a decode round, i.e. mid-turn at
    // the post-decode poll and never at a turn's first poll. Tracing fills Events::State::trace.
    bool hold_next_operation    = false;
    bool release_held_operation = false;
    bool trace_operations       = false;
    std::optional<HostKind> forced_refusal;
    rt::DurableReason refusal_reason = rt::DurableReason::Capacity;
    std::uint64_t prepare_calls = 0;
    // Preparations refused because a context operation was in flight (busy_during_context).
    std::uint64_t busy_refusals = 0;
    std::function<void()> validation_callback;
    bool validation_valid = true;

    // Refused leases acquire no readers; prepared leases stage counted Host copies, or pin the
    // Device images a staged export streams without any Host copy.
    class ExportLease {
    public:
        ExportLease(Program& program, std::span<const Checkpoint> points, bool device = false)
            : program_(&program), points_(points.begin(), points.end()), device_(device) {
            ++program.prepare_calls;
            program.trace('E');
            // Like native preparation, a busy context transaction refuses retryably.
            if (program.busy_during_context && program.operation_) {
                ++program.busy_refusals;
                kind_   = HostKind::Busy;
                reason_ = rt::DurableReason::TransactionBusy;
                return;
            }
            if (!device_) {
                const auto used = program.physical_usage().occupied.host_bytes;
                if (program.forced_refusal || used + kCheckpointHostBytes > program.host_capacity) {
                    kind_ = program.forced_refusal.value_or(HostKind::HostCapacity);
                    shortage_ = kind_ == HostKind::HostCapacity
                        ? used + kCheckpointHostBytes - program.host_capacity : 0;
                    reason_ = program.refusal_reason;
                    return;
                }
            }
            for (const auto point : points_) { ++program.live_record(point).pins; }
            if (!device_) { ++program.staged_exports_; }
            program.events_.record([](Events::State& state) {
                ++state.prepares;
                state.demotes_at_prepare          = state.demotes;
                state.prepared_after_held_binding = state.held_binding_completed;
            });
        }

        ExportLease(ExportLease&& other) noexcept
            : program_(std::exchange(other.program_, nullptr)), points_(std::move(other.points_)),
              device_(other.device_), kind_(other.kind_), shortage_(other.shortage_),
              reason_(other.reason_) {}

        ExportLease& operator=(ExportLease&&) = delete;

        ~ExportLease() {
            if (program_ != nullptr && kind_ == HostKind::Prepared) {
                program_->retire_export(points_, device_);
            }
        }

        [[nodiscard]] rt::DurablePrepareState state() const noexcept {
            return kind_ == HostKind::Prepared ? rt::DurablePrepareState::Prepared
                                               : rt::DurablePrepareState::CapacityRefused;
        }

        [[nodiscard]] rt::DurableReason reason() const noexcept { return reason_; }
        [[nodiscard]] HostKind host_shortage_kind() const noexcept { return kind_; }
        [[nodiscard]] std::size_t host_shortage_bytes() const noexcept { return shortage_; }
        [[nodiscard]] std::uint64_t device_staged_states() const noexcept {
            return device_ && kind_ == HostKind::Prepared ? points_.size() : 0;
        }

        // Runs off the worker and reads only the copy this lease staged.
        [[nodiscard]] rt::DurableExportResult write(rt::ContinuationCheckpointWriter& writer) {
            const std::array<std::byte, 1> index{};
            if (!writer.write_file("engine/index.cbor", 0, index.size(), index, 1) ||
                !writer.finish()) {
                return {.reason = rt::DurableReason::IoUnavailable};
            }
            return {.state = rt::DurableExportState::Exported, .stats = durable_stats()};
        }

    private:
        Program* program_ = nullptr;
        std::vector<Checkpoint> points_;
        bool device_ = false;
        HostKind kind_ = HostKind::Prepared;
        std::size_t shortage_ = 0;
        rt::DurableReason reason_ = rt::DurableReason::None;
    };

    [[nodiscard]] rt::DurableNativeLayout checkpoint_layout() const { return {}; }

    [[nodiscard]] static rt::DurableCheckpointSet
    decode_checkpoint_index(std::span<const std::byte>, const rt::DurableLimits&) {
        rt::DurableCheckpointSet set;
        set.points.resize(1);
        return set;
    }

    struct ImportValidation {
        enum class State : std::uint8_t { Validated, Corrupt } state = State::Validated;
        rt::DurableReason reason = rt::DurableReason::None;
    };

    [[nodiscard]] ImportValidation validate_checkpoint_import(const rt::DurableCheckpointSet&,
                                                              const rt::ContinuationCheckpointReader& reader,
                                                              const rt::DurableLimits&) const {
        if (validation_callback) validation_callback();
        return validation_valid && reader.verify_complete() ? ImportValidation{} :
            ImportValidation{ImportValidation::State::Corrupt, rt::DurableReason::PayloadDigestMismatch};
    }

    [[nodiscard]] ImportReservation prepare_checkpoint_import(const rt::DurableCheckpointSet& set,
                                                              const rt::ContinuationCheckpointReader&,
                                                              const rt::DurableLimits&) {
        trace('I');
        // Like native preparation, a busy context transaction refuses retryably.
        if (busy_during_context && operation_) { return ImportReservation(rt::DurableReason::TransactionBusy); }
        const auto points = static_cast<std::uint32_t>(set.points.size());
        reserved_points_ += points;
        gate_->changed();
        return ImportReservation(*this, points);
    }

    [[nodiscard]] std::vector<Checkpoint> commit_import(ImportReservation&& reservation) {
        require(reservation.program_ == this, "scripted import reservation is foreign or consumed");
        reservation.program_ = nullptr;
        reserved_points_ -= reservation.points_;
        std::vector<Checkpoint> handles;
        for (std::uint32_t point = 0; point < reservation.points_; ++point) {
            handles.push_back(publish_checkpoint(kDurableFrontier));
        }
        gate_->changed();
        return handles;
    }

    [[nodiscard]] ExportLease prepare_checkpoint_export(std::span<const Checkpoint> points,
                                                        const rt::DurableLimits&) {
        return ExportLease(*this, points);
    }

    // The Device-staged retry after an exhausted Host route (fork #89) streams from Device only
    // when a fixture enables it; otherwise it is refused like the Host route.
    [[nodiscard]] ExportLease prepare_checkpoint_export(std::span<const Checkpoint> points,
                                                        const rt::DurableLimits&,
                                                        bool device_staging) {
        return ExportLease(*this, points, device_staging && stage_from_device);
    }

    bool pause_needs_host = false;
    bool release_named_victim = false;
    bool busy_during_context  = false;
    bool stage_from_device    = false;

private:
    struct Lane {
        bool active                 = false;
        Script script               = Script::Complete;
        std::uint64_t epoch         = 0;
        std::uint32_t prompt_tokens = 0;
        std::uint32_t reused_tokens = 0;
        bool publish                = false;
        bool permit                 = false;
        bool partial_progress       = false;
        std::uint32_t prefilled      = 0;
    };

    struct Operation {
        ContextKind kind            = ContextKind::Bind;
        std::uint32_t lane          = 0;
        Script script               = Script::Complete;
        std::uint32_t prompt_tokens = 0;
        std::uint32_t reused_tokens = 0;
        bool publish                = false;
    };

    struct Record {
        std::uint32_t frontier = 0;
        std::uint32_t pins     = 0;
        bool valid             = false;
        bool history           = false;
        bool source_prefix     = false;
        std::size_t host_bytes = kCheckpointHostBytes;
    };

    [[nodiscard]] Checkpoint publish_checkpoint(std::uint32_t frontier) noexcept {
        records_.push_back({.frontier = frontier, .valid = true});
        return {static_cast<std::uint32_t>(records_.size() - 1)};
    }

    [[nodiscard]] Record& live_record(Checkpoint checkpoint) {
        require(valid_checkpoint(checkpoint), "checkpoint handle is not live");
        return records_[checkpoint.index];
    }

    [[nodiscard]] const Record& live_record(Checkpoint checkpoint) const {
        require(valid_checkpoint(checkpoint), "checkpoint handle is not live");
        return records_[checkpoint.index];
    }

    void release_reservation(std::uint32_t points) noexcept {
        reserved_points_ -= points;
        gate_->changed();
    }

    void retire_export(std::span<const Checkpoint> points, bool device) noexcept {
        for (const auto point : points) { --records_[point.index].pins; }
        if (!device) { --staged_exports_; }
        gate_->changed();
    }

    [[nodiscard]] Lane& live(Sequence sequence) {
        return const_cast<Lane&>(std::as_const(*this).live(sequence));
    }

    [[nodiscard]] const Lane& live(Sequence sequence) const {
        const auto& lane = lanes_.at(sequence.lane);
        require(lane.active && lane.epoch == sequence.epoch, "sequence handle is not live");
        return lane;
    }

    [[nodiscard]] rt::ConsumeStatus release(Sequence sequence) noexcept {
        if (sequence.lane >= lanes_.size() || !lanes_[sequence.lane].active ||
            lanes_[sequence.lane].epoch != sequence.epoch) {
            return rt::ConsumeStatus::InvariantMismatch;
        }
        lanes_[sequence.lane] = Lane{};
        return rt::ConsumeStatus::Consumed;
    }

    // ESAVE per-site cases: arm the hold of an operation started while armed; record order.
    void operation_started(char event) {
        if (std::exchange(hold_next_operation, false)) {
            held_until_release_ = true;
            events_.record([](Events::State& state) { state.held_operation = true; });
        }
        trace(event);
    }

    void operation_completed(ContextKind kind) {
        if (std::exchange(held_until_release_, false)) { release_held_operation = false; }
        decoded_after_release_ = false;
        trace(kind == ContextKind::Bind ? 'B' : 'P');
    }

    void trace(char event) {
        if (!trace_operations) { return; }
        events_.record([event](Events::State& state) {
            if (state.trace.size() < kTraceLimit) { state.trace.push_back(event); }
        });
    }

    Events& events_;
    std::size_t retained_extent_bytes_ = 0;
    std::shared_ptr<PublicationGate> gate_;
    std::array<rt::KvPoolDescriptor, 1> pools_{
        rt::KvPoolDescriptor{.id = {0}, .label = "main", .device_page_groups = kPageGroups}};
    std::array<Lane, ninfer::kMaximumConcurrency> lanes_{};
    std::optional<Operation> operation_;
    // Device page groups freed by completed demotions and not yet reserved.
    std::uint32_t credits_ = 0;
    // A HeldDemotionArrival's shortage asked for the next demotion, and that demotion completed.
    bool arrival_demotion_requested_ = false;
    bool arrival_demotion_done_      = false;
    std::uint64_t next_epoch_ = 1;
    // Index 0 never names a checkpoint.
    std::vector<Record> records_{Record{}};
    std::size_t reserved_points_ = 0;
    std::size_t staged_exports_  = 0;
    // ESAVE per-site cases: the trace bound; the current operation is held, and a decode round
    // followed its release.
    static constexpr std::size_t kTraceLimit = 64;
    bool held_until_release_    = false;
    bool decoded_after_release_ = false;
};

using Published = std::vector<ninfer::OutputDelta>;

class OutputSession {
public:
    explicit OutputSession(Script script) : script_(script) {}

    [[nodiscard]] rt::OutputDecision preview_model(std::span<const TokenId> tokens,
                                                   std::uint32_t remaining,
                                                   FinishReason limit_reason) {
        const auto accepted =
            static_cast<std::uint32_t>(std::min<std::size_t>(tokens.size(), remaining));
        rt::OutputDecision decision{.accepted_tokens = accepted};
        if (script_ == Script::ControlRejected) {
            staged_control_         = true;
            decision.continuation = rt::ContinuationAction::ApplyTargetControl;
        } else if (accepted == remaining) {
            decision.finish_reason = limit_reason;
        }
        return decision;
    }

    [[nodiscard]] std::uint32_t
    model_token_budget_remaining(std::uint32_t remaining) const noexcept {
        return remaining;
    }

    [[nodiscard]] std::span<const TokenId> pending_control_tokens() const noexcept {
        return control_ ? std::span<const TokenId>(&kControlToken, 1) : std::span<const TokenId>{};
    }

    [[nodiscard]] rt::OutputDecision preview_control(std::span<const TokenId>, std::uint32_t) {
        throw ninfer::text::GrammarViolation();
    }

    void validate_generation_capacity(std::uint32_t) const {}

    [[nodiscard]] rt::OutputDecision preview_terminal(FinishReason reason) {
        return {.finish_reason = reason};
    }

    [[nodiscard]] Published commit_preview() {
        control_        = control_ || staged_control_;
        staged_control_ = false;
        return {};
    }

    void discard_preview() { staged_control_ = false; }

    [[nodiscard]] bool constrained() const noexcept { return false; }

    void observe_constraint(bool, double) noexcept {}

    void constraint_uploaded(std::size_t) noexcept {}

    [[nodiscard]] std::optional<ninfer::ConstraintObservation> constraint_observation() const {
        return std::nullopt;
    }

    [[nodiscard]] std::uint32_t grammar_masks(std::span<const TokenId>, std::span<std::uint32_t>) {
        return 0;
    }

    [[nodiscard]] std::vector<ninfer::GeneratedToolCall> take_tool_calls() noexcept { return {}; }

    [[nodiscard]] ninfer::ToolCallParseDiagnostics tool_call_parse_diagnostics() const noexcept {
        return {};
    }

    [[nodiscard]] std::uint32_t reasoning_tokens() const noexcept { return 0; }

    [[nodiscard]] ninfer::ThinkingBudgetStats thinking_stats() const noexcept { return {}; }

    [[nodiscard]] std::optional<std::string> matched_stop_string() const { return std::nullopt; }

private:
    Script script_;
    bool staged_control_ = false;
    bool control_        = false;
};

struct Frontend {
    [[nodiscard]] OutputSession
    make_output_session(const Prompt& prompt, const ninfer::StopPolicy&,
                        const ninfer::OutputOptions&, const ninfer::ThinkingControlOptions&,
                        const std::optional<ninfer::OutputConstraint>&,
                        const ninfer::ToolChoice&) const {
        return OutputSession(prompt.script);
    }
};

struct Contract {
    using Frontend          = ::Frontend;
    using PreparedPrompt    = Prompt;
    using OutputSession     = ::OutputSession;
    using PublishedOutput   = Published;
    using RequestBasePlan   = Plan;
    using SequenceHandle    = Sequence;
    using CheckpointHandle  = Checkpoint;
    using SourceCandidate   = Source;
    using ResumeState       = Resume;
    using ExecutionUnit     = Unit;
    using ExecutionUnitKind = UnitKind;
    using ContextProgress   = ::ContextProgress;
    using PendingBatch      = ::PendingBatch;
    using PrefillProgress   = ::PrefillProgress;
    using CommitResult      = ::CommitResult;
    using Program           = ::Program;
    using CacheSessionKey   = SessionKey;
};

struct Instance {
    using ModelContract = Contract;

    Frontend frontend;
    std::unique_ptr<Program> program;
};

using Core       = rt::EngineCore<Instance>;
using Submission = Core::Submission;

ninfer::DeviceContext& device() {
    static ninfer::DeviceContext context(0);
    return context;
}

struct Outcome {
    std::optional<ninfer::GenerationResult> result;
    std::exception_ptr error;
    // Read immediately after wait() returned, as an HTTP status reader would.
    ninfer::RuntimeStats stats;
};

// The serving store's authenticated Reader for the one-point scripted session checkpoint.
class IndexReader final : public rt::ContinuationCheckpointReader {
public:
    bool fresh_available = true, readable = true, complete = true;
    std::string digest = "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d";
    mutable bool consumed = false;
    [[nodiscard]] std::optional<std::uint64_t> file_size(std::string_view path) const override {
        return path == "engine/index.cbor" ? std::optional<std::uint64_t>(1) : std::nullopt;
    }

    [[nodiscard]] std::optional<rt::DurablePayload> file_descriptor(std::string_view path) const override {
        if (path != "engine/index.cbor") return std::nullopt;
        return rt::DurablePayload{std::string(path), 1, digest, 1};
    }

    bool read_file(std::string_view, std::uint64_t, std::span<std::byte> destination) const override {
        consumed = true;
        std::fill(destination.begin(), destination.end(), std::byte{});
        return readable;
    }

    [[nodiscard]] bool verify_complete() const override { return complete && readable && consumed; }

    [[nodiscard]] rt::DurableReason failure_reason() const noexcept override {
        return rt::DurableReason::None;
    }

    [[nodiscard]] std::shared_ptr<const rt::ContinuationCheckpointReader>
    fresh_reader() const override {
        return fresh_available ? std::make_shared<IndexReader>(*this) : nullptr;
    }
};

class VictimOracle final : public rt::ReclaimableSessionOracle {
public:
    mutable std::size_t calls = 0;
    bool missing = false, missing_current = false, changed = false;
    std::string required_tag;
    IndexReader reader;
    std::shared_ptr<const rt::ContinuationCheckpointReader>
    pin_recoverable(std::string_view, std::string_view tag) const noexcept override {
        ++calls;
        if (!required_tag.empty() && required_tag != tag) return {};
        if (missing || (missing_current && calls % 2 == 0)) return {};
        auto pin = std::make_shared<IndexReader>(reader);
        if (changed && calls % 2 == 0) pin->digest = std::string(64, 'b');
        return pin;
    }
};

class NullWriter final : public rt::ContinuationCheckpointWriter {
public:
    bool write_file(std::string_view, std::uint64_t, std::uint64_t, std::span<const std::byte>,
                    std::uint32_t) override {
        return true;
    }

    [[nodiscard]] bool finish() override { return true; }
};

class CallbackWriter final : public rt::ContinuationCheckpointWriter {
public:
    explicit CallbackWriter(std::function<void()> callback) : callback_(std::move(callback)) {}

    bool write_file(std::string_view, std::uint64_t, std::uint64_t, std::span<const std::byte>,
                    std::uint32_t) override {
        callback_();
        return true;
    }

    [[nodiscard]] bool finish() override { return true; }

private:
    std::function<void()> callback_;
};

// Aborts an uncommitted restore on the worker, as destroying DurableEngineImport does.
struct Restore {
    rt::PreparedEngineCheckpoint prepared;

    ~Restore() {
        if (prepared.abort) { prepared.abort(); }
    }
};

class Harness {
public:
    explicit Harness(std::uint32_t concurrency, bool context_cache = false,
                     std::uint32_t stall_budget = 0, std::uint32_t decode_rounds = 0,
                     std::uint32_t max_context = 64) {
        instance_.program = std::make_unique<Program>(events_, gate_);
        ninfer::EngineOptions options;
        options.max_concurrency       = concurrency;
        options.max_context           = max_context;
        options.prefill_chunk         = kPrefillChunk;
        options.prefill_stall_budget_ms = stall_budget;
        options.decode_rounds_per_prefill = decode_rounds;
        options.context_cache.enabled = context_cache;
        core_.emplace(instance_, device(), options, rt::generic_context_machine_cost_model());
    }

    Harness(const Harness&)            = delete;
    Harness& operator=(const Harness&) = delete;

    [[nodiscard]] Submission submit(Script script, std::uint32_t output_tokens,
                                     bool observed = false,
                                     Clock::time_point pending_deadline = {},
                                     std::uint32_t prompt_tokens = 4) {
        Prompt prompt{.script = script, .tokens = prompt_tokens,
                      .observed = observed ? gate_ : nullptr};
        const ninfer::PromptSummary summary{.prompt_tokens = prompt.tokens};
        rt::ResolvedRequestOptions options;
        options.execution.requested_output_tokens = output_tokens;
        return core_->submit(std::move(prompt), summary, 0.0, std::move(options),
                             ninfer::OutputConsumerMode::Aggregate, {}, pending_deadline);
    }

    // A turn of the durable session: its completed continuation becomes a named checkpoint owner.
    [[nodiscard]] Submission submit_turn(Script script, const std::string& tag,
                                         std::string_view session = kSession) {
        Prompt prompt{.script = script, .publish_continuation = true};
        const ninfer::PromptSummary summary{.prompt_tokens = prompt.tokens};
        rt::ResolvedRequestOptions options;
        options.execution.requested_output_tokens = 1;
        return core_->submit(std::move(prompt), summary, 0.0, std::move(options),
                             ninfer::OutputConsumerMode::Aggregate, {}, {}, std::string(session), tag);
    }

    // An unnamed request whose completed continuation stays an ordinary catalog checkpoint.
    [[nodiscard]] Submission submit_published(Script script, std::uint32_t prompt_tokens) {
        Prompt prompt{.script = script, .tokens = prompt_tokens, .publish_continuation = true};
        const ninfer::PromptSummary summary{.prompt_tokens = prompt.tokens};
        rt::ResolvedRequestOptions options;
        options.execution.requested_output_tokens = 1;
        return core_->submit(std::move(prompt), summary, 0.0, std::move(options),
                             ninfer::OutputConsumerMode::Aggregate, {});
    }

    // Waits as an HTTP handler would, then reads runtime statistics before anything else runs.
    [[nodiscard]] Outcome await(Submission& submission, bool cancel = false,
                                Clock::time_point cancel_at = {}) {
        Outcome outcome;
        try {
            outcome.result = submission.wait(nullptr, ninfer::CancellationView([&] {
                return cancel || (cancel_at != Clock::time_point{} && Clock::now() >= cancel_at);
            }));
        } catch (...) { outcome.error = std::current_exception(); }
        outcome.stats = core_->runtime_stats();
        gate_->consumer_read();
        return outcome;
    }

    // Reads runtime statistics right after a checkpoint operation returned, as an HTTP status
    // reader would.
    [[nodiscard]] ninfer::RuntimeStats observed_stats() {
        const auto stats = core_->runtime_stats();
        gate_->consumer_read();
        return stats;
    }

    // Prepares a restore of the durable session's stored checkpoint, as the checkpoint service
    // does after loading it from disk.
    [[nodiscard]] Restore prepare_restore() {
        const IndexReader reader;
        const rt::DurableImportTarget target{.session_sha256      = kSession,
                                             .expected_tag        = "restored",
                                             .expected_epoch      = core_->session_epoch(kSession),
                                             .allow_prefix_reuse  = true,
                                             .cache_participation = true};
        return {core_->prepare_import(target, reader, {}, durable_stats(), {})};
    }

    // Exports a durable session endpoint, as the checkpoint service does before publishing it.
    [[nodiscard]] rt::DurableExportResult save(
        const std::string& tag, std::string_view session = kSession,
        rt::DurableSaveIntent intent = rt::DurableSaveIntent::Automatic) {
        NullWriter writer;
        return core_->export_session(session, tag, writer, {}, intent);
    }

    void configure(const std::function<void(Program&)>& action) {
        core_->checkpoint_call([&] { action(*instance_.program); });
    }
    void release_stalls() { events_.stall_released.store(true); }
    void release_hold() { events_.hold_released.store(true); }

    [[nodiscard]] Events& events() noexcept { return events_; }

    [[nodiscard]] Core& core() noexcept { return *core_; }

private:
    Events events_;
    std::shared_ptr<PublicationGate> gate_ = std::make_shared<PublicationGate>();
    Instance instance_;
    std::optional<Core> core_;
};

struct Membership {
    std::uint32_t running       = 0;
    std::uint32_t waiting       = 0;
    std::uint32_t paused        = 0;
    std::uint32_t materializing = 0;
};

void require_membership(const ninfer::RuntimeStats& stats, Membership expected,
                        const std::string& label) {
    require(stats.running_requests == expected.running &&
                stats.waiting_requests == expected.waiting &&
                stats.paused_requests == expected.paused &&
                stats.materializing_requests == expected.materializing &&
                stats.terminal_pending_requests == 0,
            label + " response preceded retirement: running=" +
                std::to_string(stats.running_requests) +
                " waiting=" + std::to_string(stats.waiting_requests) +
                " paused=" + std::to_string(stats.paused_requests) +
                " materializing=" + std::to_string(stats.materializing_requests) +
                " terminal=" + std::to_string(stats.terminal_pending_requests));
}

void require_finished(const Outcome& outcome, FinishReason reason, const std::string& label) {
    if (outcome.error) {
        try {
            std::rethrow_exception(outcome.error);
        } catch (const std::exception& error) {
            throw std::runtime_error(label + " failed: " + error.what());
        }
    }
    require(outcome.result && outcome.result->finish_reason == reason,
            label + " finished with an unexpected reason");
}

template <class Error>
Error require_error(const Outcome& outcome, const std::string& label) {
    require(outcome.error != nullptr, label + " did not fail");
    try {
        std::rethrow_exception(outcome.error);
    } catch (const Error& error) {
        return error;
    } catch (const std::exception& error) {
        throw std::runtime_error(label + " failed with an unexpected error: " + error.what());
    }
}

void require_request_error(const Outcome& outcome, RequestErrorKind kind,
                           const std::string& label) {
    require(require_error<RequestError>(outcome, label).kind() == kind,
            label + " failed with an unexpected request error kind");
}

// Counted durable checkpoint state, in kCheckpointHostBytes units of Host memory.
struct Durable {
    std::uint32_t catalog = 0;
    std::size_t host      = 0; // occupied, reserved destinations included
    std::size_t reserved  = 0;
};

void require_durable(const ninfer::RuntimeStats& stats, Durable expected, const std::string& label) {
    require(stats.private_catalog_occupied == expected.catalog &&
                stats.host_context_occupied_bytes == expected.host * kCheckpointHostBytes &&
                stats.host_context_reserved_bytes == expected.reserved * kCheckpointHostBytes,
            label + " result preceded its statistics: catalog=" +
                std::to_string(stats.private_catalog_occupied) +
                " host_occupied=" + std::to_string(stats.host_context_occupied_bytes) +
                " host_reserved=" + std::to_string(stats.host_context_reserved_bytes));
}

void completed_response() {
    Harness harness(1);
    auto request = harness.submit(Script::Complete, 1, true);
    const auto outcome = harness.await(request);
    require_finished(outcome, FinishReason::OutputLimit, "completed request");
    require_membership(outcome.stats, {}, "completed request");
    require(outcome.stats.generated_tokens == 1, "completed response preceded token publication");
}

void resident_cancellation() {
    Harness harness(1);
    auto request = harness.submit(Script::Stall, 1, true);
    harness.events().await([](const Events::State& state) { return state.bound == 1; },
                           "the resident binding");
    const auto outcome = harness.await(request, true);
    require_finished(outcome, FinishReason::Cancelled, "resident cancellation");
    require_membership(outcome.stats, {}, "resident cancellation");
}

void waiting_cancellation() {
    Harness harness(1);
    auto blocker = harness.submit(Script::Stall, 1);
    auto request = harness.submit(Script::Complete, 1, true);
    harness.events().await([](const Events::State& state) { return state.planned == 2; },
                           "the waiting admission scan");
    const auto outcome = harness.await(request, true);
    require_finished(outcome, FinishReason::Cancelled, "waiting cancellation");
    require_membership(outcome.stats, {.running = 1}, "waiting cancellation");
    harness.release_stalls();
    require_finished(harness.await(blocker), FinishReason::OutputLimit, "waiting blocker");
}

void waiting_expiry() {
    Harness harness(1);
    auto blocker = harness.submit(Script::Stall, 1);
    auto request = harness.submit(Script::Complete, 1, true, Clock::now() + 400ms);
    harness.events().await([](const Events::State& state) { return state.planned == 2; },
                           "the waiting admission scan");
    const auto outcome = harness.await(request);
    require_request_error(outcome, RequestErrorKind::QueueTimeout, "waiting expiry");
    require_membership(outcome.stats, {.running = 1}, "waiting expiry");
    harness.release_stalls();
    require_finished(harness.await(blocker), FinishReason::OutputLimit, "expiry blocker");
}

void materializing_cancellation() {
    Harness harness(1);
    auto request = harness.submit(Script::HoldBinding, 1, true);
    harness.events().await([](const Events::State& state) { return state.binding_held; },
                           "the held binding");
    const auto outcome = harness.await(request, true);
    require_finished(outcome, FinishReason::Cancelled, "materializing cancellation");
    require_membership(outcome.stats, {}, "materializing cancellation");
}

void materializing_failure() {
    Harness harness(1);
    auto request = harness.submit(Script::InvalidBinding, 1, true);
    const auto outcome = harness.await(request);
    require(std::string(require_error<std::logic_error>(outcome, "materializing failure").what()) ==
                "binding completed without a native sequence",
            "materializing failure did not deliver the binding error");
    require_membership(outcome.stats, {}, "materializing failure");
    require(!harness.core().is_available(), "invalid binding left the Engine available");
}

void pausing_cancellation() {
    Harness harness(2);
    auto pressure = harness.submit(Script::Pressure, 1);
    auto request  = harness.submit(Script::VictimHold, 1, true);
    harness.events().await([](const Events::State& state) { return state.pause_started; },
                           "the victim pause");
    const auto outcome = harness.await(request, true);
    require_finished(outcome, FinishReason::Cancelled, "pausing cancellation");
    require_membership(outcome.stats, {.running = 1}, "pausing cancellation");
    harness.release_stalls();
    require_finished(harness.await(pressure), FinishReason::OutputLimit, "pause requester");
}

void paused_cancellation() {
    Harness harness(2);
    auto pressure = harness.submit(Script::Pressure, 1);
    auto request  = harness.submit(Script::VictimPause, 1, true);
    harness.events().await([](const Events::State& state) { return state.paused; },
                           "the published pause");
    const auto outcome = harness.await(request, true);
    require_finished(outcome, FinishReason::Cancelled, "paused cancellation");
    require_membership(outcome.stats, {.running = 1}, "paused cancellation");
    harness.release_stalls();
    require_finished(harness.await(pressure), FinishReason::OutputLimit, "pause requester");
}

void younger_pressure_owner_progresses_before_older_finishes() {
    Harness harness(2);
    auto older = harness.submit(Script::VictimResume, 1);
    auto younger = harness.submit(Script::Pressure, 1);
    harness.events().await([](const Events::State& state) { return state.paused; },
                           "younger pressure handoff");
    harness.release_stalls();
    require_finished(harness.await(younger), FinishReason::OutputLimit, "younger pressure owner");
    require_finished(harness.await(older), FinishReason::OutputLimit, "restored older request");
    require(harness.events().snapshot().pressure_finished_before_victim,
            "younger admitted request waited for the older request's completion");
    require_membership(harness.observed_stats(), {}, "pressure handoff cleanup");
}

void fresh_arrival_waits_behind_paused_admitted_work() {
    Harness harness(2);
    auto owner = harness.submit(Script::Pressure, 1);
    auto paused = harness.submit(Script::VictimPause, 1);
    harness.events().await([](const Events::State& state) { return state.paused; },
                           "paused admitted request");
    auto fresh = harness.submit(Script::Complete, 1, false, Clock::now() + 100ms);
    const auto outcome = harness.await(fresh);
    require_request_error(outcome, RequestErrorKind::QueueTimeout, "deferred fresh arrival");
    require_membership(outcome.stats, {.running = 1, .paused = 1}, "deferred fresh arrival");
    require_finished(harness.await(paused, true), FinishReason::Cancelled, "paused cleanup");
    harness.release_stalls();
    require_finished(harness.await(owner), FinishReason::OutputLimit, "pressure cleanup");
    require_membership(harness.observed_stats(), {}, "fresh admission cleanup");
}

void fresh_deadline_after_pressure_progress() {
    Harness harness(2);
    auto owner = harness.submit(Script::PressureProgress, 1);
    auto paused = harness.submit(Script::VictimResume, 1);
    harness.events().await([](const Events::State& state) { return state.paused; },
                           "paused peer before the pressure owner's useful quantum");
    auto fresh = harness.submit(Script::Complete, 1, false, Clock::now() + 2s);
    const auto outcome = harness.await(fresh);
    // Settle held requests before asserting the regression, so predecessor RED is bounded.
    require_finished(harness.await(paused, true), FinishReason::Cancelled, "deadline paused cleanup");
    harness.release_stalls();
    require_finished(harness.await(owner), FinishReason::OutputLimit, "deadline owner cleanup");
    require_finished(outcome, FinishReason::OutputLimit,
                     "fresh deadline after pressure owner's committed progress");
    require(outcome.stats.preemptions == 1,
            "fresh admission paused a resident instead of using the free lane");
    require_membership(harness.observed_stats(), {}, "deadline admission cleanup");
}

void prefill_pacing_and_disabled_schedule() {
    for (const auto [budget, rounds] : {std::pair{0U, 0U}, std::pair{0U, 4U},
                                       std::pair{25U, 4U}}) {
        Harness harness(2, false, budget, rounds, 16384);
        auto stream = harness.submit(Script::PacingDecode, 256);
        harness.events().await([](const Events::State& state) { return state.decode_held; },
                               "the generating neighbour");
        auto prompt = harness.submit(Script::PacingPrefill, 1, false, {}, 4096);
        harness.events().record([](Events::State& state) { state.decode_released = true; });
        const auto result = harness.await(prompt, false, Clock::now() + kEventTimeout);
        require_finished(result, FinishReason::OutputLimit, "paced prefill");
        require_finished(harness.await(stream), FinishReason::OutputLimit, "paced decoder");
        const auto steps = harness.events().snapshot().prefill_steps;
        require(steps.size() == (budget ? 32U : 4U), "pacing lost or duplicated prompt chunks");
        for (std::size_t i = 0; i < steps.size(); ++i) {
            require(steps[i].first == (budget ? 128U : kPrefillChunk),
                    "Engine did not propagate the text cap");
            if (i != 0) {
                const auto expected_rounds = budget || rounds == 0 ? 1U : rounds;
                require(steps[i].second - steps[i - 1].second == expected_rounds,
                        "Engine changed disabled alternation or ignored decode cadence");
            }
        }
        const auto stats = harness.observed_stats();
        require(stats.computed_prefill_tokens == 4100, "paced work was counted more than once");
        require((stats.paced_prefill_steps != 0) == (budget != 0) &&
                    (stats.prefill_cadence_deferrals != 0) == (rounds != 0 && budget == 0),
                "pacing counters disagreed with actual execution");
    }
}

void short_arrival_during_staged_prefill() {
    // The base already admits during staged prefill. Both off and on must preserve it.
    for (const auto budget : {0U, 250U}) {
        Harness harness(2, false, budget, budget ? 64U : 0U);
        auto long_prompt = harness.submit(Script::Stall, 1);
        harness.events().await([](const Events::State& state) { return state.bound == 1; },
                               "staged long prefill");
        auto arrival = harness.submit(Script::Complete, 1);
        const auto result = harness.await(arrival, false, Clock::now() + kEventTimeout);
        require_finished(result, FinishReason::OutputLimit, "short prefill arrival");
        require_membership(result.stats, {.running = 1}, "arrival beside staged prefill");
        harness.release_stalls();
        require_finished(harness.await(long_prompt), FinishReason::OutputLimit, "staged prefill");
    }
}

void control_rejection() {
    Harness harness(1);
    auto request = harness.submit(Script::ControlRejected, 4, true);
    const auto outcome = harness.await(request);
    (void)require_error<ninfer::text::GrammarViolation>(outcome, "control rejection");
    require_membership(outcome.stats, {}, "control rejection");
    require(harness.core().is_available(), "control rejection failed the Engine");
}

void constraint_dead_end() {
    Harness harness(1);
    auto request = harness.submit(Script::DeadEnd, 1, true);
    const auto outcome = harness.await(request);
    require_request_error(outcome, RequestErrorKind::ConstraintDeadEnd, "constraint dead end");
    require_membership(outcome.stats, {}, "constraint dead end");
    require(harness.core().is_available(), "constraint dead end failed the Engine");
}

void engine_failure() {
    StderrCapture capture;
    Harness harness(1);
    auto request = harness.submit(Script::DecodeFailure, 4, true);
    auto waiting = harness.submit(Script::Complete, 1);
    harness.events().await([](const Events::State& state) { return state.planned == 2; },
                           "the waiting admission scan");
    harness.release_stalls();
    const auto outcome = harness.await(request);
    require(std::string(require_error<std::logic_error>(outcome, "engine failure").what()) ==
                "injected decode failure",
            "engine failure delivered another error");
    require_membership(outcome.stats, {}, "engine failure");
    require(!harness.core().is_available(), "internal decode failure left the Engine available");
    require(std::string(require_error<std::logic_error>(
                harness.await(waiting), "engine failure queue").what()) == "injected decode failure",
            "waiting request lost the original worker failure");
    const auto diagnostic = capture.text();
    require(diagnostic.find("NInfer worker failed: injected decode failure") != std::string::npos &&
                diagnostic.find("request=1 lane=0") != std::string::npos &&
                diagnostic.find("NInfer worker failed:", diagnostic.find("NInfer worker failed:") + 1) ==
                    std::string::npos,
            "first worker failure was not logged once with pre-cleanup membership");
}

void normal_shutdown_is_quiet() {
    StderrCapture capture;
    { Harness harness(1); }
    require(capture.text().find("NInfer worker failed:") == std::string::npos,
            "ordinary shutdown was reported as a worker failure");
}

void numerical_failure_is_request_local() {
    for (const auto script : {Script::NumericalPrefill, Script::NumericalDecode,
                              Script::OutOfDomainDecode}) {
        Harness harness(2);
        auto sibling = harness.submit(Script::Stall, 1);
        harness.events().await([](const Events::State& s) { return s.bound == 1; }, "sibling binding");
        auto faulted = harness.submit(script, 4);
        const auto failed = harness.await(faulted);
        require_request_error(failed, RequestErrorKind::NumericalFailure, "numerical request");
        require(harness.core().is_available(), "numerical failure latched the Engine");
        harness.release_stalls();
        require_finished(harness.await(sibling), FinishReason::OutputLimit, "numerical sibling");
        auto next = harness.submit(Script::Complete, 1);
        require_finished(harness.await(next), FinishReason::OutputLimit, "post-numerical request");
    }
}

void stop_retains_durable_catalog() {
    Harness harness(2, true);
    auto completed = harness.submit_turn(Script::Complete, "acknowledged");
    require_finished(harness.await(completed), FinishReason::OutputLimit, "shutdown seed");
    harness.core().response_stored(kSession, "acknowledged");
    auto resident = harness.submit(Script::Stall, 4);
    harness.events().await([](const Events::State& s) { return s.bound == 2; }, "shutdown resident");
    auto pending = harness.submit(Script::HoldBinding, 4);
    harness.core().request_stop();
    harness.core().request_stop();
    const auto deadline = Clock::now() + 2s;
    require_request_error(harness.await(resident, false, deadline), RequestErrorKind::Unavailable,
                          "shutdown resident");
    require_request_error(harness.await(pending, false, deadline), RequestErrorKind::Unavailable,
                          "shutdown pending");
    require(!harness.core().is_available(), "stop left generation available");
    require(harness.save("acknowledged", kSession, rt::DurableSaveIntent::Final).state ==
                rt::DurableExportState::Exported,
            "stop discarded the catalog or stopped checkpoint commands before final_save");
    bool refused = false;
    try { auto next = harness.submit(Script::Complete, 1); }
    catch (const RequestError& error) { refused = error.kind() == RequestErrorKind::Unavailable; }
    require(refused, "stop admitted new generation");
}

void isolated_admission_refusal() {
    Harness harness(1);
    auto request = harness.submit(Script::IsolatedMiss, 1, true);
    const auto outcome = harness.await(request);
    require_request_error(outcome, RequestErrorKind::Unavailable, "isolated capacity miss");
    require_membership(outcome.stats, {}, "isolated capacity miss");
    require(harness.core().is_available(), "isolated capacity miss failed the Engine");
    auto next = harness.submit(Script::Complete, 1);
    require_finished(harness.await(next), FinishReason::OutputLimit,
                     "request after an isolated capacity miss");
}

void isolated_restore_refusal() {
    Harness harness(2);
    auto pressure = harness.submit(Script::Pressure, 1);
    auto request  = harness.submit(Script::VictimPause, 1, true);
    harness.events().await([](const Events::State& state) { return state.paused; },
                           "the published pause");
    harness.release_stalls();
    // An impossible restore must settle even while the fresh-admission wake-up remains set.
    // Cancellation only bounds a regression; it cannot satisfy the required Unavailable result.
    const auto outcome = harness.await(request, false, Clock::now() + kEventTimeout);
    require_request_error(outcome, RequestErrorKind::Unavailable, "isolated restore miss");
    require_membership(outcome.stats, {}, "isolated restore miss");
    require(harness.core().is_available(), "isolated restore miss failed the Engine");
    require_finished(harness.await(pressure), FinishReason::OutputLimit, "pause requester");
    auto next = harness.submit(Script::Complete, 1);
    require_finished(harness.await(next), FinishReason::OutputLimit,
                     "request after an isolated restore miss");
}

void isolated_restore_refusal_preserves_deferred_admission() {
    Harness harness(2);
    auto pressure = harness.submit(Script::Pressure, 1);
    auto paused = harness.submit(Script::VictimPause, 1, true);
    harness.events().await([](const Events::State& state) { return state.paused; },
                           "the published pause before fresh arrival");
    auto fresh = harness.submit(Script::Complete, 1);
    harness.release_stalls();
    const auto deadline = Clock::now() + kEventTimeout;
    const auto refused = harness.await(paused, false, deadline);
    require_request_error(refused, RequestErrorKind::Unavailable, "isolated paused owner");
    require_finished(harness.await(pressure, false, deadline), FinishReason::OutputLimit,
                     "completed pressure owner");
    require_finished(harness.await(fresh, false, deadline), FinishReason::OutputLimit,
                     "fresh arrival after isolated restore refusal");
    require_membership(harness.observed_stats(), {}, "isolated restore with deferred admission");
    require(harness.events().snapshot().bound == 3 &&
                harness.observed_stats().generated_tokens == 2,
            "isolated refusal lost or duplicated deferred fresh work");
    require(harness.core().is_available(), "isolated restore refusal failed the Engine");
}

void isolated_state_slot_refusal() {
    Harness harness(1);
    auto request = harness.submit(Script::StateSlotMiss, 1, true);
    const auto outcome = harness.await(request);
    require_request_error(outcome, RequestErrorKind::Unavailable, "isolated state slot miss");
    require(std::string(require_error<RequestError>(outcome, "isolated state slot miss").what())
                .find("state=1") != std::string::npos,
            "state slot refusal did not identify the missing resource");
    require_membership(outcome.stats, {}, "isolated state slot miss");
    require(harness.core().is_available(), "isolated state slot miss failed the Engine");
    auto next = harness.submit(Script::Complete, 1);
    require_finished(harness.await(next), FinishReason::OutputLimit,
                     "request after an isolated state slot miss");
}

void durable_import_commit() {
    Harness harness(1, true);
    auto restore = harness.prepare_restore();
    require(restore.prepared.state == rt::DurablePrepareState::Prepared,
            "durable restore was not prepared");
    require_durable(harness.observed_stats(), {.host = 1, .reserved = 1},
                    "durable import reservation");
    const auto committed = restore.prepared.commit();
    const auto stats     = harness.observed_stats();
    require(committed.state == rt::DurableImportState::Imported, "durable restore did not commit");
    require_durable(stats, {.catalog = 1, .host = 1}, "durable import commit");
}

void durable_import_abort() {
    Harness harness(1, true);
    auto restore = harness.prepare_restore();
    require(restore.prepared.state == rt::DurablePrepareState::Prepared,
            "durable restore was not prepared");
    (void)harness.observed_stats();
    restore.prepared.abort();
    require_durable(harness.observed_stats(), {}, "durable import abort");
}

void durable_export_release() {
    Harness harness(1, true);
    auto restore = harness.prepare_restore();
    require(restore.prepared.state == rt::DurablePrepareState::Prepared,
            "durable restore was not prepared");
    (void)harness.observed_stats();
    require(restore.prepared.commit().state == rt::DurableImportState::Imported,
            "durable restore did not commit");
    (void)harness.observed_stats();
    const auto saved = harness.save("restored");
    const auto stats = harness.observed_stats();
    require(saved.state == rt::DurableExportState::Exported, "durable save was not exported");
    require_durable(stats, {.catalog = 1, .host = 1}, "durable export lease retirement");
}

// A completed sibling's off-worker export can pin the only reclaimable KV while a
// resident needs its next unit. This is temporary pressure, not native impossibility.
void resident_waits_for_export() {
    Harness harness(2, true);
    auto completed = harness.submit_turn(Script::Complete, "exported");
    require_finished(harness.await(completed), FinishReason::OutputLimit, "export source");
    harness.core().response_stored(kSession, "exported");
    auto resident = harness.submit(Script::ExportPressure, 1);
    harness.events().await([](const Events::State& state) { return state.bound == 2; },
                           "the export-blocked resident");
    CallbackWriter writer([&] {
        harness.events().await([](const Events::State& state) { return state.export_blocked; },
                               "resident pressure while the export lease is held");
        // Waiting for the lease must not freeze rows whose next unit already fits.
        auto sibling = harness.submit(Script::Complete, 1);
        require_finished(harness.await(sibling), FinishReason::OutputLimit,
                         "runnable sibling during an export");
        require(harness.core().is_available(), "export pressure failed the Engine");
    });
    const auto saved = harness.core().export_session(kSession, "exported", writer, {});
    require(saved.state == rt::DurableExportState::Exported, "held export did not complete");
    harness.release_stalls();
    require_finished(harness.await(resident), FinishReason::OutputLimit,
                     "resident after export pin retirement");
    require(harness.core().is_available(), "export retirement failed the Engine");
}

// An older turn that completes after a newer turn was acknowledged can never become the session's
// committed endpoint. It is not named for the session, so saving it reports absence instead of a
// retryable tag race; a newer unacknowledged turn keeps its name and the acknowledged endpoint
// stays exportable.
void late_turn_after_acknowledgement() {
    Harness harness(2, true);
    auto older = harness.submit_turn(Script::Stall, "turn-1");
    harness.events().await([](const Events::State& state) { return state.bound == 1; },
                           "the older turn's binding");
    auto acknowledged = harness.submit_turn(Script::Complete, "turn-2");
    require_finished(harness.await(acknowledged), FinishReason::OutputLimit, "acknowledged turn");
    harness.core().response_stored(kSession, "turn-2");
    auto newer = harness.submit_turn(Script::Complete, "turn-3");
    require_finished(harness.await(newer), FinishReason::OutputLimit, "newer turn");
    harness.release_stalls();
    require_finished(harness.await(older), FinishReason::OutputLimit, "older turn");
    const auto late = harness.save("turn-1");
    require(late.state == rt::DurableExportState::Refused &&
                late.reason == rt::DurableReason::SessionNotIndexed,
            "an older turn completing after a newer acknowledgement stayed named for its session");
    const auto unacknowledged = harness.save("turn-3");
    require(unacknowledged.state == rt::DurableExportState::Stale &&
                unacknowledged.reason == rt::DurableReason::TagMismatch,
            "a newer unacknowledged turn lost its durable name");
    require(harness.save("turn-2").state == rt::DurableExportState::Exported,
            "the acknowledged endpoint stopped being exportable");
    (void)harness.observed_stats();
}

void acknowledged_turn(Harness& h, std::string_view session, const std::string& tag) {
    auto turn = h.submit_turn(Script::Complete, tag, session);
    require_finished(h.await(turn), FinishReason::OutputLimit, "reclaim fixture turn");
    h.core().response_stored(session, tag);
}

class PendingHandler final : public rt::PressureCheckpointHandler {
public:
    unsigned calls = 0;
    std::function<void()> callback;
    void save_before_eviction(std::span<rt::PressureCheckpointVictim>) noexcept override {
        ++calls;
        if (callback) callback();
    }
};

class DeferredPressureHandler final : public rt::PressureCheckpointHandler {
public:
    explicit DeferredPressureHandler(Events& events) : events_(events) {}

    std::atomic<bool> settled{false};

    void save_before_eviction(std::span<rt::PressureCheckpointVictim> victims) noexcept override {
        if (settled.load()) {
            for (auto& victim : victims) { victim.outcome = rt::PressureCheckpointOutcome::Settled; }
        }
        events_.record([&](Events::State& state) {
            ++state.pressure_saves;
            state.pressure_save_named = victims.size() == 1 &&
                victims.front().session_sha256 == "victim" &&
                victims.front().checkpoint_tag == "saved";
        });
    }

private:
    Events& events_;
};

void pending_pressure_save(bool have_older_resident) {
    Harness h(have_older_resident ? 3U : 2U, true);
    acknowledged_turn(h, "victim", "saved");
    auto handler = std::make_shared<DeferredPressureHandler>(h.events());
    h.core().set_pressure_checkpoint_handler(handler);
    h.configure([](Program& p) {
        p.host_capacity = kCheckpointHostBytes;
        p.pause_needs_host = true;
    });
    auto owner = h.submit(Script::Pressure, 1);
    std::optional<Submission> older;
    if (have_older_resident) { older.emplace(h.submit(Script::Stall, 1)); }
    auto youngest = h.submit(Script::VictimResume, 1);
    h.events().await([](const Events::State& state) { return state.pressure_saves != 0; },
                     "the Host victim's pending durable pressure save");
    // Worker-command barriers let the failed pause return and another reservation retry run.
    // At F the two-resident case instead returns the Engine's legal-unit logic_error here.
    for (unsigned turn = 0; turn != 2; ++turn) {
        h.configure([&](Program& p) {
            const auto state = h.events().snapshot();
            require(state.pressure_saves == 1 && state.pressure_save_named,
                    "Pending save was retried without progress or selected the wrong Host victim");
            require(state.pause_lanes.empty(),
                    "Pending youngest victim save paused an older resident");
            require(p.valid_checkpoint({1}), "Pending save released its Host checkpoint");
        });
    }
    require(h.core().is_available(), "Pending pressure save failed the Engine");

    handler->settled.store(true);
    h.core().set_reclaimable_session_oracle(std::make_shared<VictimOracle>());
    h.core().notify_checkpoint_progress();
    h.events().await([](const Events::State& state) { return state.pause_started; },
                     "the youngest victim's pause after its durable save settled");
    const auto state = h.events().snapshot();
    require(state.pause_lanes == std::vector<std::uint32_t>{have_older_resident ? 2U : 1U} &&
                state.host_pause_saved && state.pressure_saves == 2,
            "settled save did not resume the youngest victim's Host pause");
    h.configure([](Program& p) {
        require(!p.valid_checkpoint({1}), "settled Host victim was not released");
    });
    h.release_stalls();
    require_finished(h.await(owner), FinishReason::OutputLimit, "pressure save owner");
    if (older) { require_finished(h.await(*older), FinishReason::OutputLimit, "older resident"); }
    require_finished(h.await(youngest), FinishReason::OutputLimit, "restored youngest victim");
    require_membership(h.observed_stats(), {}, "pending pressure save cleanup");
}

void resident_waits_for_pending_pressure_save() { pending_pressure_save(false); }

void pending_pressure_save_preserves_older_resident() { pending_pressure_save(true); }

// --- Fork #102: bounded fresh admission at context-operation boundaries -----------------------
// A resident whose every unit needs a page group demotes catalog history each turn, so Native's
// single context slot is occupied whenever the ordinary fresh scan follows the resident
// reservation. Seeds that history, starts the resident and returns once it is progressing
// through its own demotions.
[[nodiscard]] Submission start_demoting_resident(Harness& harness) {
    auto history = harness.submit_published(Script::HistorySeed, 4);
    require_finished(harness.await(history), FinishReason::OutputLimit, "history seed");
    auto resident = harness.submit(Script::DemotingPrefill, 1, false, {}, kLongPrompt);
    harness.events().await(
        [](const Events::State& state) { return state.demotes >= 2 && state.demoting_steps >= 2; },
        "the resident progressing through its own demotions");
    return resident;
}

// The resident still progresses through its own demotions after the arrival settled; it is then
// released and finishes. Callers run this before asserting, so a regression stays bounded.
void finish_demoting_resident(Harness& harness, Submission& resident,
                              const Events::State& settled) {
    harness.events().await(
        [&](const Events::State& state) {
            return state.demoting_steps >= settled.demoting_steps + 2 &&
                   state.demotes >= settled.demotes + 2;
        },
        "resident demotions after the arrival settled");
    harness.release_stalls();
    require_finished(harness.await(resident), FinishReason::OutputLimit, "demoting resident");
}

void fresh_arrival_during_resident_demotion() {
    Harness harness(2, true, 0, 0, kLongContext);
    auto resident = start_demoting_resident(harness);
    auto arrival  = harness.submit(Script::Complete, 1, false, Clock::now() + 1s);
    const auto outcome = harness.await(arrival);
    finish_demoting_resident(harness, resident, harness.events().snapshot());
    require_finished(outcome, FinishReason::OutputLimit,
                     "fresh arrival while every resident unit starts a demotion");
    require(outcome.result->scheduling.preemptions == 0 && outcome.stats.preemptions == 0 &&
                outcome.stats.running_requests == 1 && outcome.stats.prefilling_requests == 1,
            "the arrival's admission paused or displaced the prefilling resident");
    require_membership(harness.observed_stats(), {}, "resident demotion cleanup");
}

// The arrival's first binding is a page group short, so its own demotion runs. It must bind at
// that demotion's completion boundary, before the resident's next reservation takes the page.
void fresh_arrival_rebinds_after_its_own_demotion() {
    Harness harness(2, true, 0, 0, kLongContext);
    auto resident = start_demoting_resident(harness);
    auto arrival  = harness.submit(Script::CreditArrival, 1, false, Clock::now() + 1s);
    const auto outcome = harness.await(arrival);
    const auto settled = harness.events().snapshot();
    finish_demoting_resident(harness, resident, settled);
    require_finished(outcome, FinishReason::OutputLimit, "arrival that needs its own demotion");
    require(settled.credit_shortages == 1,
            "the resident took the page group the arrival's own demotion freed");
    require(outcome.stats.preemptions == 0 && outcome.stats.running_requests == 1 &&
                outcome.stats.prefilling_requests == 1,
            "the arrival's own demotion paused or displaced the prefilling resident");
    require_membership(harness.observed_stats(), {}, "own demotion cleanup");
}

void fresh_arrival_keeps_its_preferred_source() {
    Harness harness(2, true, 0, 0, kLongContext);
    auto seed = harness.submit_published(Script::SourceSeed, kSourceFrontier);
    require_finished(harness.await(seed), FinishReason::OutputLimit, "source seed");
    auto resident = start_demoting_resident(harness);
    auto arrival  = harness.submit(Script::SourceArrival, 1, false, Clock::now() + 1s);
    const auto outcome = harness.await(arrival);
    finish_demoting_resident(harness, resident, harness.events().snapshot());
    require_finished(outcome, FinishReason::OutputLimit, "arrival preferring a retained prefix");
    require(outcome.result->admission.preferred_reused_tokens == kSourceFrontier &&
                outcome.result->reused_prompt_tokens == kSourceFrontier &&
                outcome.result->computed_prefill_tokens == 1 &&
                outcome.result->admission.fallback_reason == ninfer::AdmissionFallbackReason::None,
            "the arrival did not bind its preferred source");
    require_membership(harness.observed_stats(), {}, "preferred source cleanup");
}

// The resident's own reclaim leaves a named victim's pressure save Pending. Every fresh request
// waits while it is Pending, the resident does not progress, and nothing is released or paused.
void pending_pressure_save_blocks_fresh_arrival() {
    Harness harness(2, true, 0, 0, kLongContext);
    acknowledged_turn(harness, "victim", "saved");
    auto handler = std::make_shared<DeferredPressureHandler>(harness.events());
    harness.core().set_pressure_checkpoint_handler(handler);
    auto resident = start_demoting_resident(harness);
    harness.configure([](Program& p) { p.release_named_victim = true; });
    harness.events().await([](const Events::State& state) { return state.pressure_saves != 0; },
                           "the resident's Pending pressure save");
    const auto pending = harness.events().snapshot();
    auto arrival = harness.submit(Script::Complete, 1, false, Clock::now() + 300ms);
    const auto outcome = harness.await(arrival);
    const auto settled = harness.events().snapshot();
    harness.configure([](Program& p) {
        require(p.valid_checkpoint({1}), "a Pending pressure save released its named checkpoint");
        p.release_named_victim = false;
    });
    harness.core().notify_checkpoint_progress();
    finish_demoting_resident(harness, resident, settled);
    require_request_error(outcome, RequestErrorKind::QueueTimeout,
                          "fresh arrival during a Pending pressure save");
    require(settled.bound == pending.bound && settled.demoting_steps == pending.demoting_steps &&
                settled.pressure_save_named && settled.pause_lanes.empty(),
            "a Pending pressure save admitted the arrival, ran the resident or paused it");
    require_membership(harness.observed_stats(), {}, "pending pressure save cleanup");
}

// An arrival that can never bind receives a bounded number of attempts while the resident keeps
// progressing, then expires at its own deadline.
void fresh_priority_attempts_are_bounded() {
    Harness harness(2, true, 0, 0, kLongContext);
    auto resident = start_demoting_resident(harness);
    const auto before = harness.events().snapshot();
    auto arrival = harness.submit(Script::StateArrival, 1, false, Clock::now() + 500ms);
    const auto outcome = harness.await(arrival);
    const auto settled = harness.events().snapshot();
    finish_demoting_resident(harness, resident, settled);
    require_request_error(outcome, RequestErrorKind::QueueTimeout, "arrival that can never bind");
    require(settled.state_binds <= kPriorityAttempts &&
                settled.demoting_steps > before.demoting_steps,
            "arrival attempts exceeded their bound or stopped the resident");
    require_membership(harness.observed_stats(), {}, "bounded attempts cleanup");
}

// While a holder occupies the other lane, the first arrival waits without an attempt. It expires
// or is cancelled; the next arrival is then admitted on its own attempt once the lane frees.
void departed_priority_request_spares_the_next_arrival() {
    for (const bool cancel : {false, true}) {
        Harness harness(2, true, 0, 0, kLongContext);
        auto holder = harness.submit(Script::LaneHold, 1);
        harness.events().await([](const Events::State& state) { return state.bound == 1; },
                               "the lane holder");
        auto resident = start_demoting_resident(harness);
        const auto now = Clock::now();
        auto first = harness.submit(Script::Complete, 1, false,
                                    cancel ? Clock::time_point{} : now + 300ms);
        const auto departed =
            harness.await(first, false, cancel ? now + 300ms : Clock::time_point{});
        auto next = harness.submit(Script::Complete, 1, false, Clock::now() + 1s);
        harness.release_hold();
        const auto admitted = harness.await(next);
        const auto held     = harness.await(holder);
        finish_demoting_resident(harness, resident, harness.events().snapshot());
        if (cancel) {
            require_finished(departed, FinishReason::Cancelled, "cancelled waiting arrival");
        } else {
            require_request_error(departed, RequestErrorKind::QueueTimeout, "expired waiting arrival");
        }
        require_finished(held, FinishReason::OutputLimit, "lane holder");
        require_finished(admitted, FinishReason::OutputLimit,
                         "arrival after the earlier waiting arrival departed");
        require_membership(harness.observed_stats(), {}, "departed arrival cleanup");
    }
}

// An arrival that exhausted its attempts and expired leaves the next arrival its full bound: the
// next one needs its own demotion and a second attempt to bind.
void exhausted_priority_request_spares_the_next_arrival() {
    Harness harness(2, true, 0, 0, kLongContext);
    auto resident = start_demoting_resident(harness);
    auto first    = harness.submit(Script::StateArrival, 1, false, Clock::now() + 500ms);
    const auto expired = harness.await(first);
    auto next = harness.submit(Script::CreditArrival, 1, false, Clock::now() + 1s);
    const auto admitted = harness.await(next);
    const auto settled  = harness.events().snapshot();
    finish_demoting_resident(harness, resident, settled);
    require_request_error(expired, RequestErrorKind::QueueTimeout, "arrival that can never bind");
    require_finished(admitted, FinishReason::OutputLimit,
                     "arrival after an exhausted arrival expired");
    require(settled.state_binds <= kPriorityAttempts && settled.credit_shortages == 1,
            "attempts leaked between arrivals or the resident took the freed page group");
    require_membership(harness.observed_stats(), {}, "exhausted arrival cleanup");
}

// While an export holds its lease, the waiting arrival is not attempted, so the export spends
// none of its bound; it binds after the lease retires.
void fresh_priority_waits_out_an_export() {
    Harness harness(2, true, 0, 0, kLongContext);
    acknowledged_turn(harness, kSession, "exported");
    auto resident = start_demoting_resident(harness);
    std::optional<Submission> arrival;
    Events::State submitted, during;
    ninfer::RuntimeStats during_stats;
    CallbackWriter writer([&] {
        submitted = harness.events().snapshot();
        arrival.emplace(harness.submit(Script::Complete, 1, false, Clock::now() + 2s));
        harness.events().await(
            [&](const Events::State& state) { return state.demotes >= submitted.demotes + 3; },
            "resident demotions while the export lease is held");
        during       = harness.events().snapshot();
        during_stats = harness.observed_stats();
    });
    const auto saved = harness.core().export_session(kSession, "exported", writer, {});
    const auto outcome = harness.await(*arrival);
    finish_demoting_resident(harness, resident, harness.events().snapshot());
    require(saved.state == rt::DurableExportState::Exported, "held export did not complete");
    require(during.bound == submitted.bound && during_stats.waiting_requests == 1,
            "the arrival bound while an export lease was held");
    require_finished(outcome, FinishReason::OutputLimit, "arrival after the export lease retired");
    require_membership(harness.observed_stats(), {}, "export lease cleanup");
}

// A Device-staged export (fork #89: the Host arena cannot take its copy) holds the same lease
// for its whole off-lock write. The armed arrival is not attempted meanwhile, so the staged
// write spends none of its bound; it binds once the export retires.
void fresh_priority_waits_out_a_device_staged_export() {
    Harness harness(2, true, 0, 0, kLongContext);
    acknowledged_turn(harness, kSession, "staged");
    auto resident = start_demoting_resident(harness);
    harness.configure([](Program& p) {
        p.forced_refusal    = Program::HostKind::HostContiguity;
        p.stage_from_device = true;
    });
    std::optional<Submission> arrival;
    Events::State submitted, during;
    ninfer::RuntimeStats during_stats;
    CallbackWriter writer([&] {
        submitted = harness.events().snapshot();
        arrival.emplace(harness.submit(Script::Complete, 1, false, Clock::now() + 2s));
        harness.events().await(
            [&](const Events::State& state) { return state.demotes >= submitted.demotes + 3; },
            "resident demotions while the Device-staged export is written");
        during       = harness.events().snapshot();
        during_stats = harness.observed_stats();
    });
    const auto saved = harness.core().export_session(kSession, "staged", writer, {});
    const auto outcome = harness.await(*arrival);
    finish_demoting_resident(harness, resident, harness.events().snapshot());
    require(saved.state == rt::DurableExportState::Exported && saved.reclaim.prepare_attempts == 1 &&
                saved.reclaim.device_staging_attempted && saved.reclaim.device_staged_states != 0,
            "the export did not complete through the Device-staged route");
    require(during.bound == submitted.bound && during_stats.waiting_requests == 1,
            "the arrival bound while a Device-staged export held its lease");
    require_finished(outcome, FinishReason::OutputLimit, "arrival after the staged export retired");
    require_membership(harness.observed_stats(), {}, "staged export cleanup");
}

// A save requested while a priority binding is in flight is held for the next operation-free
// boundary, not refused as a busy transaction. Issued from another thread, it does not return
// while the binding is in flight; it is prepared once the binding completes, before the
// resident's next demotion or any other operation starts, and exports without a busy refusal.
void priority_binding_defers_a_save_until_it_completes() {
    Harness harness(2, true, 0, 0, kLongContext);
    acknowledged_turn(harness, kSession, "saved");
    harness.configure([](Program& p) { p.busy_during_context = true; });
    auto resident = start_demoting_resident(harness);
    auto arrival  = harness.submit(Script::HeldArrival, 1, false, Clock::now() + 2s);
    harness.events().await([](const Events::State& state) { return state.binding_held; },
                           "the arrival's binding in flight");
    std::uint64_t before = 0, during = 0, after = 0, busy = 0;
    harness.configure([&](Program& p) { before = p.prepare_calls; });
    // Only this thread releases the binding, so a save issued here would never return.
    auto saving = std::async(std::launch::async, [&] { return harness.save("saved"); });
    const bool held = saving.wait_for(300ms) == std::future_status::timeout;
    harness.configure([&](Program& p) { during = p.prepare_calls; });
    harness.release_hold();
    const auto saved   = saving.get();
    const auto outcome = harness.await(arrival);
    const auto settled = harness.events().snapshot();
    harness.configure([&](Program& p) {
        after = p.prepare_calls;
        busy  = p.busy_refusals;
    });
    finish_demoting_resident(harness, resident, settled);
    require(held && during == before,
            "a save returned or was prepared while the arrival's binding was in flight");
    require(saved.state == rt::DurableExportState::Exported &&
                saved.reason == rt::DurableReason::None && busy == 0 && after == before + 1,
            "a save met the in-flight binding instead of waiting for its completion");
    require(settled.held_binding_completed && settled.prepared_after_held_binding &&
                settled.demotes_at_prepare == settled.demotes_at_held_binding,
            "another operation started between the binding's completion and the held save");
    require_finished(outcome, FinishReason::OutputLimit, "arrival whose binding was held");
    require_membership(harness.observed_stats(), {}, "held save cleanup");
}

// A prepare held behind a priority attempt's own demotion takes the boundary that completes it.
// The arrival's next attempt is ready there but yields: the prepare runs first and the arrival
// binds afterwards, so the prepare waits for one operation, not for the arrival's chain.
void priority_attempt_yields_to_a_held_prepare() {
    Harness harness(2, true, 0, 0, kLongContext);
    acknowledged_turn(harness, kSession, "saved");
    harness.configure([](Program& p) { p.busy_during_context = true; });
    auto resident = start_demoting_resident(harness);
    auto arrival  = harness.submit(Script::HeldDemotionArrival, 1, false, Clock::now() + 2s);
    harness.events().await(
        [](const Events::State& state) { return state.arrival_demotion_started; },
        "the arrival's own demotion in flight");
    auto saving = std::async(std::launch::async, [&] { return harness.save("saved"); });
    const bool held = saving.wait_for(300ms) == std::future_status::timeout;
    harness.release_hold();
    const auto saved   = saving.get();
    const auto outcome = harness.await(arrival);
    const auto settled = harness.events().snapshot();
    std::uint64_t busy = 0;
    harness.configure([&](Program& p) { busy = p.busy_refusals; });
    finish_demoting_resident(harness, resident, settled);
    require(held && saved.state == rt::DurableExportState::Exported &&
                saved.reason == rt::DurableReason::None && busy == 0,
            "a save met the arrival's in-flight demotion instead of waiting for it");
    require(settled.prepares == 1 && settled.prepares_at_arrival_bind == 1,
            "the arrival's next attempt started an operation before the held prepare ran");
    require_finished(outcome, FinishReason::OutputLimit, "arrival that yielded to a held prepare");
    require_membership(harness.observed_stats(), {}, "yielded attempt cleanup");
}
// --- End fork #102 ------------------------------------------------------------------------------

void export_proven_victim() {
    Harness h(1, true);
    acknowledged_turn(h, "victim", "a");
    acknowledged_turn(h, kSession, "b");
    auto oracle = std::make_shared<VictimOracle>();
    auto pending = std::make_shared<PendingHandler>();
    h.core().set_reclaimable_session_oracle(oracle);
    h.core().set_pressure_checkpoint_handler(pending);
    h.configure([](Program& p) { p.host_capacity = 2 * kCheckpointHostBytes; });
    const auto clock = h.core().checkpoint_progress();
    const auto saved = h.save("b", kSession, rt::DurableSaveIntent::Final);
    require(saved.state == rt::DurableExportState::Exported && oracle->calls == 2 &&
                pending->calls == 0 && saved.proof_pins.size() == 2 &&
                saved.reclaim.prepare_attempts == 2 && saved.reclaim.reclaimed_owners == 1 &&
                saved.reclaim.reclaimed_host_bytes == kCheckpointHostBytes &&
                saved.reclaim.reclaimed_cache_steps == 0,
            "fresh proof must bypass stopped/Pending handler and retain both generation pins");
    require(h.core().checkpoint_progress() == clock + 1,
            "prepared export retirement did not advance observed progress exactly once");
    require(h.core().publish_export(kSession, "b", saved.captured_epoch, [] { return true; }).state ==
                rt::DurableExportState::Exported, "reclaim lost the publication gate");
    require_durable(h.observed_stats(), {.catalog = 1, .host = 1}, "proven victim release stats");
}

void export_proof_failures_and_intents() {
    for (unsigned failure = 0; failure != 10; ++failure) {
        Harness h(1, true);
        acknowledged_turn(h, "victim", "a");
        acknowledged_turn(h, kSession, "b");
        auto oracle = std::make_shared<VictimOracle>();
        oracle->missing = failure == 1;
        oracle->reader.fresh_available = failure != 2;
        oracle->reader.readable = failure != 3;
        oracle->reader.complete = failure != 4;
        oracle->missing_current = failure == 5;
        oracle->changed = failure == 6;
        if (failure == 9) oracle->required_tag = "newer-tag";
        if (failure) h.core().set_reclaimable_session_oracle(oracle);
        h.configure([](Program& p) { p.host_capacity = 2 * kCheckpointHostBytes; });
        const auto intent = failure == 7 ? rt::DurableSaveIntent::Automatic :
                            failure == 8 ? rt::DurableSaveIntent::Survivor : rt::DurableSaveIntent::Explicit;
        const auto result = h.save("b", kSession, intent);
        require(result.reason == rt::DurableReason::Capacity && result.reclaim.prepare_attempts == 1 &&
                    result.reclaim.reclaimed_owners == 0 && result.reclaim.reclaimed_host_bytes == 0,
                "unproved or policy-disabled victim was reclaimed");
        if (failure == 7 || failure == 8) require(oracle->calls == 0, "automatic/survivor save performed disk proof");
        require_durable(h.observed_stats(), {.catalog = 2, .host = 2}, "declined victim remains resident");
    }
}

void export_classification_and_contiguity() {
    for (auto kind : {Program::HostKind::Busy, Program::HostKind::NoHostArena,
                      Program::HostKind::Prepared, Program::HostKind::HostContiguity}) {
        for (bool cleanup : {false, true}) {
            Harness h(1, true);
            acknowledged_turn(h, "victim", "a");
            acknowledged_turn(h, kSession, "b");
            auto oracle = std::make_shared<VictimOracle>();
            h.core().set_reclaimable_session_oracle(oracle);
            h.configure([&](Program& p) {
                // Prepared is the unclassified non-Host Capacity site in this fixture.
                p.forced_refusal = kind == Program::HostKind::Prepared ? Program::HostKind::Busy : kind;
                p.refusal_reason = kind == Program::HostKind::Prepared
                    ? rt::DurableReason::TransactionBusy : rt::DurableReason::Capacity;
                p.redundant_bytes = cleanup ? kCheckpointHostBytes : 0;
            });
            const auto result = h.save("b", kSession, rt::DurableSaveIntent::Final);
            const bool lossless = kind == Program::HostKind::HostContiguity && cleanup;
            require(result.state == rt::DurableExportState::Refused && oracle->calls == 0 &&
                        result.reclaim.prepare_attempts == (lossless ? 2 : 1) &&
                        result.reclaim.reclaimed_owners == 0 && result.reclaim.reclaimed_cache_steps == 0 &&
                        result.reclaim.reclaimed_host_bytes == (lossless ? kCheckpointHostBytes : 0),
                    "non-byte shortage performed destructive reclaim or unbacked prepare");
        }
    }
}

void export_zero_progress_and_residual() {
    {
        Harness h(1, true);
        acknowledged_turn(h, "first-victim", "a");
        acknowledged_turn(h, "second-victim", "b");
        acknowledged_turn(h, kSession, "c");
        auto oracle = std::make_shared<VictimOracle>();
        h.core().set_reclaimable_session_oracle(oracle);
        h.configure([](Program& p) { p.host_capacity = 3 * kCheckpointHostBytes; p.zero_arena_gain = true; });
        const auto result = h.save("c", kSession, rt::DurableSaveIntent::Final);
        require(result.state == rt::DurableExportState::Exported &&
                    result.reclaim.reclaimed_owners == 2 && result.reclaim.prepare_attempts == 2 &&
                    result.reclaim.reclaimed_host_bytes == kCheckpointHostBytes,
                "zero arena gain ended the fixed scan or authorized a prepare");
    }
    {
        Harness h(1, true);
        acknowledged_turn(h, kSession, "old");
        acknowledged_turn(h, kSession, "current"); // old becomes ordinary cache
        h.configure([](Program& p) {
            p.host_capacity = 2 * kCheckpointHostBytes;
            p.redundant_bytes = kCheckpointHostBytes / 2;
        });
        const auto clock = h.core().checkpoint_progress();
        const auto result = h.save("current");
        require(result.state == rt::DurableExportState::Exported &&
                    result.reclaim.reclaimed_cache_steps == 1 &&
                    result.reclaim.reclaimed_host_bytes == 3 * kCheckpointHostBytes / 2 &&
                    result.reclaim.prepare_attempts == 2 && h.core().checkpoint_progress() == clock + 1,
                "ordinary all-or-nothing plan was given the original rather than residual deficit");
    }
    {
        Harness h(1, true);
        acknowledged_turn(h, "protected", "a");
        acknowledged_turn(h, kSession, "b");
        h.configure([](Program& p) {
            p.host_capacity = 2 * kCheckpointHostBytes;
            p.redundant_bytes = kCheckpointHostBytes / 2;
        });
        const auto clock = h.core().checkpoint_progress();
        const auto result = h.save("b");
        require(result.reason == rt::DurableReason::Capacity &&
                    result.reclaim.prepare_attempts == 2 &&
                    result.reclaim.reclaimed_host_bytes == kCheckpointHostBytes / 2 &&
                    h.core().checkpoint_progress() == clock,
                "partially reclaiming automatic refusal woke its own deferred retry");
    }
}

void export_exception_and_writer_epoch() {
    {
        Harness h(1, true);
        acknowledged_turn(h, "first", "a");
        acknowledged_turn(h, "second", "b");
        acknowledged_turn(h, kSession, "c");
        h.core().set_reclaimable_session_oracle(std::make_shared<VictimOracle>());
        unsigned validations = 0;
        h.configure([&](Program& p) {
            p.host_capacity = 3 * kCheckpointHostBytes;
            p.zero_arena_gain = true;
            p.validation_callback = [&] { if (++validations == 2) throw std::bad_alloc(); };
        });
        const auto clock = h.core().checkpoint_progress();
        const auto result = h.save("c", kSession, rt::DurableSaveIntent::Final);
        require(result.reason == rt::DurableReason::Capacity && result.reclaim.prepare_attempts == 1 &&
                    result.reclaim.reclaimed_owners == 1 && result.reclaim.reclaimed_host_bytes == 0 &&
                    result.proof_pins.size() == 2 && h.core().checkpoint_progress() == clock,
                "optional allocation failure lost earlier release pins or fabricated progress");
        require_durable(h.observed_stats(), {.catalog = 2, .host = 3},
                        "catalog mutation before optional reclaim failure");
    }
    {
        Harness h(1, true);
        acknowledged_turn(h, "victim", "a");
        acknowledged_turn(h, kSession, "b");
        h.core().set_reclaimable_session_oracle(std::make_shared<VictimOracle>());
        h.configure([](Program& p) { p.host_capacity = 2 * kCheckpointHostBytes; });
        CallbackWriter writer([&] {
            // Waiting for this native work proves the writer is not holding execution.
            acknowledged_turn(h, kSession, "newer");
        });
        const auto result = h.core().export_session(
            kSession, "b", writer, {}, rt::DurableSaveIntent::Explicit);
        bool published = false;
        const auto final = h.core().publish_export(kSession, "b", result.captured_epoch,
            [&] { published = true; return true; });
        require(result.state == rt::DurableExportState::Exported && result.proof_pins.size() == 2 &&
                    result.reclaim.reclaimed_owners == 1 && !published &&
                    final.state == rt::DurableExportState::Stale,
                "writer assignment discarded reclaim evidence or bypassed epoch publication");
    }
}

// ---- E89 block (fork #89, folds #84): durable save without Host-arena room. E89-owned. ----

// A pressure save that stays Pending for Capacity, as the automatic coordinator reports a
// retryable refusal; settles only when told to.
class E89CapacityPendingHandler final : public rt::PressureCheckpointHandler {
public:
    explicit E89CapacityPendingHandler(Events& events) : events_(events) {}

    std::atomic<bool> settled{false};

    void save_before_eviction(std::span<rt::PressureCheckpointVictim> victims) noexcept override {
        const bool done = settled.load();
        for (auto& victim : victims) {
            victim.outcome = done ? rt::PressureCheckpointOutcome::Settled
                                  : rt::PressureCheckpointOutcome::Pending;
            victim.reason = done ? rt::DurableReason::None : rt::DurableReason::Capacity;
        }
        events_.record([](Events::State& state) { ++state.pressure_saves; });
    }

private:
    Events& events_;
};

// Characterization of the #89 latch (invariant 1 unchanged): while a pressure save stays
// Pending for Capacity, a fresh request is deferred until its own queue deadline instead of
// being admitted or refused, and the unsaved victim stays resident. E89 removes the Host-arena
// cause of such Capacity refusals; it does not change this rule.
void e89_pending_capacity_save_defers_fresh_request() {
    Harness h(3, true);
    acknowledged_turn(h, "victim", "saved");
    auto handler = std::make_shared<E89CapacityPendingHandler>(h.events());
    h.core().set_pressure_checkpoint_handler(handler);
    h.configure([](Program& p) {
        p.host_capacity   = kCheckpointHostBytes;
        p.pause_needs_host = true;
    });
    auto owner  = h.submit(Script::Pressure, 1);
    auto victim = h.submit(Script::VictimResume, 1);
    h.events().await([](const Events::State& state) { return state.pressure_saves != 0; },
                     "the victim's Pending Capacity pressure save");
    auto fresh = h.submit(Script::Complete, 1, false, Clock::now() + 250ms);
    require_request_error(h.await(fresh), RequestErrorKind::QueueTimeout,
                          "fresh request behind a Pending Capacity pressure save");
    h.configure([](Program& p) {
        require(p.valid_checkpoint({1}), "Pending Capacity save released its unsaved victim");
    });
    require(h.core().is_available(), "Pending Capacity save failed the Engine");

    handler->settled.store(true);
    h.core().set_reclaimable_session_oracle(std::make_shared<VictimOracle>());
    h.core().notify_checkpoint_progress();
    h.events().await([](const Events::State& state) { return state.pause_started; },
                     "the victim's pause after its save settled");
    h.release_stalls();
    require_finished(h.await(owner), FinishReason::OutputLimit, "pressure owner after settlement");
    require_finished(h.await(victim), FinishReason::OutputLimit, "victim after settlement");
    require_membership(h.observed_stats(), {}, "pending capacity save cleanup");
}

// E89 review condition (c): an export's native pins retire even when the Engine is stopped
// while its off-lock writer runs. Host-route and Device-staged leases share this retirement.
void e89_export_pins_retire_when_stopped_mid_write() {
    Harness h(1, true);
    acknowledged_turn(h, kSession, "stopping");
    CallbackWriter writer([&] { h.core().request_stop(); });
    const auto saved = h.core().export_session(kSession, "stopping", writer, {});
    require(saved.state == rt::DurableExportState::Exported, "export stopped mid-write did not complete");
    require(!h.core().is_available(), "stop during the export write did not take effect");
    require_durable(h.observed_stats(), {.catalog = 1, .host = 1}, "export pins after a stop mid-write");
    require(h.save("stopping", kSession, rt::DurableSaveIntent::Final).state == rt::DurableExportState::Exported,
            "final save after a stop mid-write met leftover pins or a stopped checkpoint worker");
}

// ---- end E89 block ----

// ---- ESAVE block (fork #89 class, busy causes): a save never meets an in-flight context
// operation and a busy refusal never parks a pressure save. ESAVE-owned. ----

[[nodiscard]] std::string esave_reason(rt::DurableReason reason) {
    return std::string(rt::durable_reason_name(reason));
}

[[nodiscard]] std::string esave_state(rt::DurableExportState state) {
    return state == rt::DurableExportState::Exported ? "exported"
           : state == rt::DurableExportState::Refused ? "refused"
                                                      : "stale";
}

// How a request ended, for failure messages that record the observed behaviour.
[[nodiscard]] std::string esave_outcome(const Outcome& outcome) {
    if (!outcome.error) {
        if (!outcome.result) { return "none"; }
        return outcome.result->finish_reason == FinishReason::OutputLimit ? "OutputLimit"
               : outcome.result->finish_reason == FinishReason::Cancelled ? "Cancelled"
                                                                           : "other finish";
    }
    try {
        std::rethrow_exception(outcome.error);
    } catch (const RequestError& error) {
        return error.kind() == RequestErrorKind::QueueTimeout ? "QueueTimeout" : "RequestError";
    } catch (const std::exception& error) {
        return std::string("error: ") + error.what();
    } catch (...) {
        return "error";
    }
}

// The production coordinator as the Engine's pressure handler. Its save exports through the
// Engine as the checkpoint service does, and every attempt's outcome is recorded.
class ESaveCoordinator final : public rt::PressureCheckpointHandler {
public:
    struct Counts {
        unsigned requests = 0, calls = 0, busy = 0, completed = 0;
        rt::DurableReason last = rt::DurableReason::None;
        // Answers to requests made after a probe was installed, and the probe value at each
        // Pending answer.
        unsigned pending_answers = 0, settled_answers = 0;
        std::vector<std::uint64_t> pending_probes;
    };

    explicit ESaveCoordinator(Harness& harness)
        : harness_(harness),
          coordinator_(
              [this](std::string_view session, std::optional<std::string_view> tag) {
                  // A hook set by the test runs once inside the write, while the export is in flight.
                  CallbackWriter writer([this] {
                      std::function<void()> hook;
                      {
                          std::lock_guard lock(mutex_);
                          hook = std::exchange(on_write_, nullptr);
                      }
                      if (hook) { hook(); }
                  });
                  const auto result =
                      harness_.core().export_session(session, tag.value_or(""), writer, {});
                  record([&](Counts& counts) {
                      ++counts.calls;
                      counts.busy += result.reason == rt::DurableReason::TransactionBusy ? 1U : 0U;
                      counts.last = result.reason;
                  });
                  return ninfer::serve::AutomaticCheckpointResult{std::string(tag.value_or("")),
                                                                  result.reason};
              },
              [this] {
                  return ninfer::serve::CheckpointSaveProgress{harness_.core().checkpoint_progress()};
              },
              [this] {
                  harness_.core().notify_checkpoint_progress();
                  record([](Counts& counts) { ++counts.completed; });
              }) {}

    void save_before_eviction(std::span<rt::PressureCheckpointVictim> victims) noexcept override {
        coordinator_.save_before_eviction(victims);
        std::function<std::uint64_t()> probe;
        {
            std::lock_guard lock(mutex_);
            probe = probe_;
        }
        const auto value = probe ? probe() : 0;
        record([&](Counts& counts) {
            ++counts.requests;
            if (!probe) { return; }
            for (const auto& victim : victims) {
                if (victim.outcome == rt::PressureCheckpointOutcome::Settled) {
                    ++counts.settled_answers;
                } else {
                    ++counts.pending_answers;
                    counts.pending_probes.push_back(value);
                }
            }
        });
    }

    // Every later request is answered and then probed on the requesting (worker) thread.
    void probe_requests(std::function<std::uint64_t()> probe) {
        std::lock_guard lock(mutex_);
        probe_ = std::move(probe);
    }

    [[nodiscard]] Counts counts() {
        std::lock_guard lock(mutex_);
        return counts_;
    }

    // Whether the predicate held before the timeout.
    template <class Predicate>
    bool wait(Predicate predicate, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return predicate(counts_); });
    }

    void drain() noexcept { coordinator_.drain(); }

    // Runs once inside the next export's write, on the coordinator thread.
    void on_write(std::function<void()> hook) {
        std::lock_guard lock(mutex_);
        on_write_ = std::move(hook);
    }

private:
    template <class Update>
    void record(Update update) noexcept {
        {
            std::lock_guard lock(mutex_);
            update(counts_);
        }
        changed_.notify_all();
    }

    Harness& harness_;
    std::mutex mutex_;
    std::condition_variable changed_;
    Counts counts_;
    std::function<std::uint64_t()> probe_;
    std::function<void()> on_write_;
    // Last: its worker uses every member above, and it is destroyed (drained) first.
    ninfer::serve::AutomaticCheckpointCoordinator coordinator_;
};

// An export that meets an in-flight context operation is neither refused nor prepared while the
// operation runs; it prepares exactly once at the next operation-free boundary.
void esave_export_waits_for_operation_free_boundary() {
    Harness h(2, true);
    acknowledged_turn(h, kSession, "saved");
    h.configure([](Program& p) { p.busy_during_context = true; });
    auto held = h.submit(Script::HoldBinding, 1);
    h.events().await([](const Events::State& state) { return state.binding_held; },
                     "the held binding");
    std::uint64_t before = 0, during = 0, after = 0;
    h.configure([&](Program& p) { before = p.prepare_calls; });
    auto saving = std::async(std::launch::async, [&] { return h.save("saved"); });
    const bool waited = saving.wait_for(300ms) == std::future_status::timeout;
    h.configure([&](Program& p) { during = p.prepare_calls; });
    // Cancellation ends the binding's operation; its completion is the next boundary.
    const auto binding = h.await(held, true);
    const auto saved   = saving.get();
    h.configure([&](Program& p) { after = p.prepare_calls; });
    require_finished(binding, FinishReason::Cancelled, "held binding");
    require(waited && during == before && saved.state == rt::DurableExportState::Exported &&
                saved.reason == rt::DurableReason::None && after == before + 1,
            "export during an in-flight context operation: waited=" + std::to_string(waited) +
                " prepared while in flight=" + std::to_string(during - before) +
                " result=" + esave_state(saved.state) + "/" + esave_reason(saved.reason) +
                " prepares=" + std::to_string(after - before));
    require_membership(h.observed_stats(), {}, "held export cleanup");
}

// A Final save issued while a context operation is in flight terminates at shutdown: the stop
// drain ends the operation and the save is prepared at that boundary instead of being refused.
void esave_final_save_waits_out_operation_at_stop() {
    Harness h(2, true);
    acknowledged_turn(h, kSession, "final");
    h.configure([](Program& p) { p.busy_during_context = true; });
    auto held = h.submit(Script::HoldBinding, 1);
    h.events().await([](const Events::State& state) { return state.binding_held; },
                     "the held binding");
    auto saving = std::async(std::launch::async, [&] {
        return h.save("final", kSession, rt::DurableSaveIntent::Final);
    });
    const bool waited = saving.wait_for(300ms) == std::future_status::timeout;
    h.core().request_stop();
    const bool terminated = saving.wait_for(kEventTimeout) == std::future_status::ready;
    require(terminated, "a Final save did not terminate while the stop drained a context operation");
    const auto saved = saving.get();
    (void)h.await(held);
    require(waited && saved.state == rt::DurableExportState::Exported &&
                saved.reason == rt::DurableReason::None,
            "Final save during an in-flight context operation: waited=" + std::to_string(waited) +
                " result=" + esave_state(saved.state) + "/" + esave_reason(saved.reason));
}

// Fork #89 with a busy cause. A pressure save is requested while a context operation is in
// flight and other sessions keep completing turns (observed Engine progress). At F3 every
// attempt is refused as a busy transaction until the retry budget is spent; the save then stays
// Pending for good, the victim is never released and a fresh request behind it expires. Fixed:
// the save waits for the operation, exports once at the next operation-free boundary, the victim
// is released when a pause needs its Host room, and the fresh request is served.
void esave_pressure_save_outlasts_busy_operation() {
    Harness h(3, true);
    acknowledged_turn(h, "victim", "saved");
    auto handler = std::make_shared<ESaveCoordinator>(h);
    h.core().set_pressure_checkpoint_handler(handler);
    h.core().set_reclaimable_session_oracle(std::make_shared<VictimOracle>());
    // On every exit: the stop drain ends any operation a held save waits for, then the
    // coordinator stops before the Engine it saves through.
    struct Stop {
        Harness& h;
        ESaveCoordinator& coordinator;
        ~Stop() {
            h.core().request_stop();
            coordinator.drain();
        }
    } stop{h, *handler};
    h.configure([](Program& p) { p.busy_during_context = true; });
    auto held = h.submit(Script::HoldBinding, 1);
    h.events().await([](const Events::State& state) { return state.binding_held; },
                     "the held binding");
    std::uint64_t prepares_before = 0, prepares_during = 0;
    h.configure([&](Program& p) { prepares_before = p.prepare_calls; });
    // A reclaim that needs the named victim released asks for its save, as checkpoint_pressure does.
    rt::PressureCheckpointVictim request{"victim", "saved"};
    handler->save_before_eviction({&request, 1});
    (void)handler->wait([](const auto& counts) { return counts.completed != 0; }, 500ms);
    for (unsigned turn = 0; turn != ninfer::serve::kTransientCheckpointRetries + 2; ++turn) {
        const auto seen = handler->counts().completed;
        h.core().response_stored("other-session", "turn");
        (void)handler->wait([&](const auto& counts) { return counts.completed > seen; }, 200ms);
    }
    const auto during = handler->counts();
    h.configure([&](Program& p) { prepares_during = p.prepare_calls; });
    const auto binding = h.await(held, true);
    (void)handler->wait([](const auto& counts) {
        return counts.completed != 0 && counts.last == rt::DurableReason::None;
    }, 2s);
    const auto saved = handler->counts();

    // The paused victim's Host room requires releasing the named victim checkpoint (E89 pattern).
    h.configure([](Program& p) {
        p.host_capacity    = kCheckpointHostBytes;
        p.pause_needs_host = true;
    });
    auto owner  = h.submit(Script::Pressure, 1);
    auto paused = h.submit(Script::VictimResume, 1);
    (void)handler->wait([&](const auto& counts) { return counts.requests > saved.requests; },
                        kEventTimeout);
    auto fresh = h.submit(Script::Complete, 1, false, Clock::now() + 2s);
    h.release_stalls();
    const auto arrival = h.await(fresh);
    const bool served  = !arrival.error;
    const auto owner_outcome  = h.await(owner, !served);
    const auto paused_outcome = h.await(paused, !served);
    bool victim_kept = true;
    h.configure([&](Program& p) { victim_kept = p.valid_checkpoint({1}); });
    const auto final_counts = handler->counts();
    require_finished(binding, FinishReason::Cancelled, "held binding");
    require(during.busy == 0 && during.completed == 0 && prepares_during == prepares_before &&
                final_counts.calls == 1 && final_counts.busy == 0 &&
                final_counts.last == rt::DurableReason::None && served && !victim_kept &&
                esave_outcome(owner_outcome) == "OutputLimit" &&
                esave_outcome(paused_outcome) == "OutputLimit",
            "observed: busy refusals while in flight=" + std::to_string(during.busy) +
                " save attempts=" + std::to_string(final_counts.calls) +
                " last=" + esave_reason(final_counts.last) +
                " prepared while in flight=" + std::to_string(prepares_during - prepares_before) +
                " arrival=" + esave_outcome(arrival) + " victim kept=" + std::to_string(victim_kept) +
                " owner=" + esave_outcome(owner_outcome) + " paused=" + esave_outcome(paused_outcome));
}

// A pressure owner that would start an operation at every reservation (it pauses a victim) and a
// pressure save requested while an operation is in flight. While the save is held no reservation
// starts an operation, no reservation asks for the victim before the save has prepared, and the
// victim is released after at most one Pending answer: the worker may ask between the export's
// retirement and the coordinator recording the finished save; the next progress change is that
// recording itself (stalled residents, no waiting requests). The export stages a Host copy, so
// Host has room for it; the room is taken away during its write, so the next pause must release
// the named victim.
void esave_pressure_save_prepared_before_resident_reserves() {
    Harness h(3, true);
    acknowledged_turn(h, "victim", "saved");
    auto handler = std::make_shared<ESaveCoordinator>(h);
    h.core().set_pressure_checkpoint_handler(handler);
    h.core().set_reclaimable_session_oracle(std::make_shared<VictimOracle>());
    struct Stop {
        Harness& h;
        ESaveCoordinator& coordinator;
        ~Stop() {
            h.core().request_stop();
            coordinator.drain();
        }
    } stop{h, *handler};
    h.configure([](Program& p) {
        p.busy_during_context = true;
        p.host_capacity       = 2 * kCheckpointHostBytes;
        p.pause_needs_host    = true;
    });
    // The older victim needs the named checkpoint's Host room to pause; the younger one's pause,
    // started first, stays in flight until it is cancelled.
    auto resumable = h.submit(Script::VictimResume, 1);
    auto holding   = h.submit(Script::VictimHold, 1);
    auto owner     = h.submit(Script::Pressure, 1);
    h.events().await([](const Events::State& state) { return state.pause_started; },
                     "the held victim pause");
    std::uint64_t before = 0;
    h.configure([&](Program& p) { before = p.prepare_calls; });
    handler->on_write([&h] { h.configure([](Program& p) { p.host_capacity = kCheckpointHostBytes; }); });
    rt::PressureCheckpointVictim request{"victim", "saved"};
    handler->save_before_eviction({&request, 1});
    const bool held = !handler->wait([](const auto& counts) { return counts.completed != 0; }, 300ms);
    handler->probe_requests([&h] {
        std::uint64_t calls = 0;
        h.configure([&](Program& p) { calls = p.prepare_calls; });
        return calls;
    });
    const auto cancelled = h.await(holding, true);
    (void)handler->wait([](const auto& counts) { return counts.settled_answers != 0; }, kEventTimeout);
    h.events().await([](const Events::State& state) { return state.paused; },
                     "the resumable victim's pause");
    h.release_stalls();
    const auto owner_outcome     = h.await(owner);
    const auto resumable_outcome = h.await(resumable);
    bool victim_kept = true;
    h.configure([&](Program& p) { victim_kept = p.valid_checkpoint({1}); });
    const auto counts = handler->counts();
    const bool prepared_first =
        std::all_of(counts.pending_probes.begin(), counts.pending_probes.end(),
                    [&](std::uint64_t calls) { return calls == before + 1; });
    require_finished(cancelled, FinishReason::Cancelled, "held victim pause");
    require(held && counts.busy == 0 && counts.calls == 1 && counts.pending_answers <= 1 &&
                prepared_first && counts.settled_answers != 0 && !victim_kept &&
                esave_outcome(owner_outcome) == "OutputLimit" &&
                esave_outcome(resumable_outcome) == "OutputLimit",
            "observed: held=" + std::to_string(held) + " busy refusals=" + std::to_string(counts.busy) +
                " attempts=" + std::to_string(counts.calls) +
                " pending answers=" + std::to_string(counts.pending_answers) +
                " prepared before every pending answer=" + std::to_string(prepared_first) +
                " victim kept=" + std::to_string(victim_kept) + " owner=" + esave_outcome(owner_outcome) +
                " resumable=" + esave_outcome(resumable_outcome));
}

// A restore that meets an in-flight context operation waits for the next operation-free boundary
// and commits; the restored continuation is the session's live, exportable endpoint. Before, the
// import was refused as a busy transaction once, with no retry: the service then publishes no
// Responses lineage and the arriving continuation fails with response_not_found.
void esave_restore_waits_for_operation_free_boundary() {
    Harness h(2, true);
    h.configure([](Program& p) { p.busy_during_context = true; });
    auto held = h.submit(Script::HoldBinding, 1);
    h.events().await([](const Events::State& state) { return state.binding_held; },
                     "the held binding");
    struct Restored {
        rt::DurablePrepareState prepared = rt::DurablePrepareState::PolicyDisabled;
        rt::DurableReason reason         = rt::DurableReason::None;
        rt::DurableImportState committed = rt::DurableImportState::PolicyDisabled;
    };
    auto restoring = std::async(std::launch::async, [&] {
        auto restore = h.prepare_restore();
        Restored result{restore.prepared.state, restore.prepared.reason};
        if (restore.prepared.state == rt::DurablePrepareState::Prepared) {
            result.committed = restore.prepared.commit().state;
        }
        return result;
    });
    const bool waited  = restoring.wait_for(300ms) == std::future_status::timeout;
    const auto binding = h.await(held, true);
    const auto result  = restoring.get();
    const auto stats   = h.observed_stats();
    require_finished(binding, FinishReason::Cancelled, "held binding");
    require(waited && result.prepared == rt::DurablePrepareState::Prepared &&
                result.committed == rt::DurableImportState::Imported,
            "restore during an in-flight context operation: waited=" + std::to_string(waited) +
                " reason=" + esave_reason(result.reason) +
                " imported=" + std::to_string(result.committed == rt::DurableImportState::Imported));
    require_durable(stats, {.catalog = 1, .host = 1}, "restore after the held import");
    require(h.save("restored").state == rt::DurableExportState::Exported,
            "the restored continuation is not the session's live endpoint");
}

// An Engine failure while a prepare is held releases it: the failed Engine runs it at once and its
// caller returns io_unavailable instead of waiting for an operation nothing will complete.
void esave_held_export_returns_after_engine_failure() {
    StderrCapture capture;
    Harness h(2, true);
    acknowledged_turn(h, kSession, "saved");
    h.configure([](Program& p) { p.busy_during_context = true; });
    auto failing = h.submit(Script::DecodeFailure, 4);
    h.events().await([](const Events::State& state) { return state.bound >= 2; },
                     "the failing resident's binding");
    auto held = h.submit(Script::HoldBinding, 1);
    h.events().await([](const Events::State& state) { return state.binding_held; },
                     "the held binding");
    auto saving = std::async(std::launch::async, [&] { return h.save("saved"); });
    const bool waited = saving.wait_for(300ms) == std::future_status::timeout;
    h.release_stalls();
    const bool returned = saving.wait_for(kEventTimeout) == std::future_status::ready;
    require(returned, "a held export did not return after the Engine failed");
    const auto saved = saving.get();
    (void)h.await(failing);
    (void)h.await(held);
    require(waited && saved.reason == rt::DurableReason::IoUnavailable && !h.core().is_available(),
            "held export at an Engine failure: waited=" + std::to_string(waited) +
                " result=" + esave_state(saved.state) + "/" + esave_reason(saved.reason));
}

// Per-site bound of a hold. The fixture holds one context operation until the test releases it;
// released, it completes at the first poll after a decode round (a decoding resident supplies
// the round): mid-turn, never at a turn's first poll. After the release marker `w` the trace must
// read: decode rounds, that operation's completion, the held prepare, the starter's operation,
// with nothing in between. So the prepare waited for exactly the one operation in flight when it
// arrived, ran at the next turn's runner, and the starter started in that same turn. Captures
// have no fixture support; their two start sites are guarded in code only.
constexpr std::uint32_t kESaveDecodeTokens = 1U << 22;

// Releases the held operation and starts the trace, in one worker command.
void esave_release_held(Harness& h) {
    h.configure([&h](Program& p) {
        p.trace_operations       = true;
        p.release_held_operation = true;
        h.events().record([](Events::State& state) { state.trace.push_back('w'); });
    });
}

[[nodiscard]] bool esave_mid_turn_shape(const std::string& trace, char prepared, char started) {
    const auto completed = trace.find('B');
    return completed != std::string::npos && completed >= 2 && trace.size() > completed + 2 &&
           trace.front() == 'w' && trace.find_first_not_of('D', 1) == completed &&
           trace[completed + 1] == prepared && trace[completed + 2] == started;
}

// Resident reclaim. A restore is held behind a pressure owner's own binding; the binding
// completes mid-turn and the owner's reservation right after that poll needs a victim paused.
// The restore prepares at the next turn's runner, before any pause or demotion starts, and the
// pause starts in that turn. Before, the restore met the binding and was refused as a busy
// transaction. A restore rather than an export: an export lease in flight would itself defer the
// reclaim until it retired.
void esave_held_prepare_bounds_resident_reclaim() {
    Harness h(3, true);
    h.configure([](Program& p) { p.busy_during_context = true; });
    auto decoding = h.submit(Script::Complete, kESaveDecodeTokens);
    h.events().await([](const Events::State& state) { return state.bound == 1; },
                     "the decoding resident's binding");
    auto victim = h.submit(Script::VictimHold, 1);
    h.events().await([](const Events::State& state) { return state.bound == 2; },
                     "the victim's binding");
    h.configure([](Program& p) { p.hold_next_operation = true; });
    auto owner = h.submit(Script::Pressure, 1);
    h.events().await([](const Events::State& state) { return state.held_operation; },
                     "the pressure owner's held binding");
    auto restoring = std::async(std::launch::async, [&h] {
        auto restore = h.prepare_restore();
        return std::pair{restore.prepared.state, restore.prepared.reason};
    });
    const bool waited = restoring.wait_for(300ms) == std::future_status::timeout;
    esave_release_held(h);
    // A restore held again behind the pause would wait until the victim ends.
    std::optional<Outcome> cancelled;
    const bool prepared = restoring.wait_for(5s) == std::future_status::ready;
    if (!prepared) { cancelled = h.await(victim, true); }
    const auto [prepare_state, prepare_reason] = restoring.get();
    h.events().await(
        [](const Events::State& state) { return state.trace.find('p') != std::string::npos; },
        "the victim's pause");
    const auto trace = h.events().snapshot().trace;
    if (!cancelled) { cancelled = h.await(victim, true); }
    h.release_stalls();
    const auto owner_outcome = h.await(owner);
    (void)h.await(decoding, true);
    require(waited && prepared && prepare_state == rt::DurablePrepareState::Prepared &&
                prepare_reason == rt::DurableReason::None && esave_mid_turn_shape(trace, 'I', 'p') &&
                esave_outcome(owner_outcome) == "OutputLimit",
            "restore held behind a mid-turn completion, resident reclaim ready: waited=" +
                std::to_string(waited) + " prepared before the victim ended=" +
                std::to_string(prepared) + " reason=" + esave_reason(prepare_reason) +
                " trace=" + trace + " owner=" + esave_outcome(owner_outcome));
}

// Admission. An export is held behind a binding; the binding completes mid-turn while a fresh
// arrival waits in the queue behind it. The export prepares at the next turn's runner and
// returns before the arrival binds, and the arrival binds in that turn. Before, the export met
// the binding and was refused as a busy transaction.
void esave_held_prepare_bounds_admission() {
    Harness h(3, true);
    acknowledged_turn(h, kSession, "saved");
    h.configure([](Program& p) { p.busy_during_context = true; });
    auto decoding = h.submit(Script::Complete, kESaveDecodeTokens);
    h.events().await([](const Events::State& state) { return state.bound == 2; },
                     "the decoding resident's binding");
    h.configure([](Program& p) { p.hold_next_operation = true; });
    auto held = h.submit(Script::Complete, 1);
    h.events().await([](const Events::State& state) { return state.held_operation; },
                     "the held binding");
    auto arrival = h.submit(Script::Complete, 1);
    std::uint64_t before = 0, after = 0;
    h.configure([&](Program& p) { before = p.prepare_calls; });
    auto saving = std::async(std::launch::async, [&h] { return h.save("saved"); });
    const bool waited = saving.wait_for(300ms) == std::future_status::timeout;
    esave_release_held(h);
    h.events().await(
        [](const Events::State& state) { return state.trace.find('b') != std::string::npos; },
        "the arrival's binding");
    const auto trace = h.events().snapshot().trace;
    (void)h.await(decoding, true);
    const auto saved = saving.get();
    h.configure([&](Program& p) { after = p.prepare_calls; });
    const auto arrival_outcome = h.await(arrival);
    const auto held_outcome    = h.await(held);
    require(waited && saved.state == rt::DurableExportState::Exported &&
                saved.reason == rt::DurableReason::None && after == before + 1 &&
                esave_mid_turn_shape(trace, 'E', 'b') &&
                esave_outcome(arrival_outcome) == "OutputLimit" &&
                esave_outcome(held_outcome) == "OutputLimit",
            "export held behind a mid-turn completion, arrival queued: waited=" +
                std::to_string(waited) + " result=" + esave_state(saved.state) + "/" +
                esave_reason(saved.reason) + " prepares=" + std::to_string(after - before) +
                " trace=" + trace + " arrival=" + esave_outcome(arrival_outcome) +
                " held=" + esave_outcome(held_outcome));
}

// ---- end ESAVE block ----

} // namespace

int main() {
    struct Case {
        const char* name;
        void (*run)();
    };
    constexpr std::array cases{
        Case{"completed_response", completed_response},
        Case{"resident_cancellation", resident_cancellation},
        Case{"waiting_cancellation", waiting_cancellation},
        Case{"waiting_expiry", waiting_expiry},
        Case{"materializing_cancellation", materializing_cancellation},
        Case{"materializing_failure", materializing_failure},
        Case{"pausing_cancellation", pausing_cancellation},
        Case{"paused_cancellation", paused_cancellation},
        Case{"younger_pressure_owner_progresses_before_older_finishes",
             younger_pressure_owner_progresses_before_older_finishes},
        Case{"fresh_arrival_waits_behind_paused_admitted_work",
             fresh_arrival_waits_behind_paused_admitted_work},
        Case{"fresh_deadline_after_pressure_progress", fresh_deadline_after_pressure_progress},
        Case{"prefill_pacing_and_disabled_schedule", prefill_pacing_and_disabled_schedule},
        Case{"short_arrival_during_staged_prefill", short_arrival_during_staged_prefill},
        Case{"control_rejection", control_rejection},
        Case{"constraint_dead_end", constraint_dead_end},
        Case{"engine_failure", engine_failure},
        Case{"normal_shutdown_is_quiet", normal_shutdown_is_quiet},
        Case{"numerical_failure_is_request_local", numerical_failure_is_request_local},
        Case{"stop_retains_durable_catalog", stop_retains_durable_catalog},
        Case{"isolated_admission_refusal", isolated_admission_refusal},
        Case{"isolated_restore_refusal", isolated_restore_refusal},
        Case{"isolated_restore_refusal_preserves_deferred_admission",
             isolated_restore_refusal_preserves_deferred_admission},
        Case{"isolated_state_slot_refusal", isolated_state_slot_refusal},
        Case{"durable_import_commit", durable_import_commit},
        Case{"durable_import_abort", durable_import_abort},
        Case{"durable_export_release", durable_export_release},
        Case{"resident_waits_for_export", resident_waits_for_export},
        Case{"resident_waits_for_pending_pressure_save", resident_waits_for_pending_pressure_save},
        Case{"pending_pressure_save_preserves_older_resident",
             pending_pressure_save_preserves_older_resident},
        Case{"fresh_arrival_during_resident_demotion", fresh_arrival_during_resident_demotion},
        Case{"fresh_arrival_rebinds_after_its_own_demotion",
             fresh_arrival_rebinds_after_its_own_demotion},
        Case{"fresh_arrival_keeps_its_preferred_source", fresh_arrival_keeps_its_preferred_source},
        Case{"pending_pressure_save_blocks_fresh_arrival",
             pending_pressure_save_blocks_fresh_arrival},
        Case{"fresh_priority_attempts_are_bounded", fresh_priority_attempts_are_bounded},
        Case{"departed_priority_request_spares_the_next_arrival",
             departed_priority_request_spares_the_next_arrival},
        Case{"exhausted_priority_request_spares_the_next_arrival",
             exhausted_priority_request_spares_the_next_arrival},
        Case{"fresh_priority_waits_out_an_export", fresh_priority_waits_out_an_export},
        Case{"fresh_priority_waits_out_a_device_staged_export",
             fresh_priority_waits_out_a_device_staged_export},
        Case{"priority_binding_defers_a_save_until_it_completes",
             priority_binding_defers_a_save_until_it_completes},
        Case{"priority_attempt_yields_to_a_held_prepare", priority_attempt_yields_to_a_held_prepare},
        Case{"late_turn_after_acknowledgement", late_turn_after_acknowledgement},
        Case{"export_proven_victim", export_proven_victim},
        Case{"export_proof_failures_and_intents", export_proof_failures_and_intents},
        Case{"export_classification_and_contiguity", export_classification_and_contiguity},
        Case{"export_zero_progress_and_residual", export_zero_progress_and_residual},
        Case{"export_exception_and_writer_epoch", export_exception_and_writer_epoch},
        // E89 block cases.
        Case{"e89_pending_capacity_save_defers_fresh_request", e89_pending_capacity_save_defers_fresh_request},
        Case{"e89_export_pins_retire_when_stopped_mid_write", e89_export_pins_retire_when_stopped_mid_write},
        // ESAVE block cases.
        Case{"esave_export_waits_for_operation_free_boundary",
             esave_export_waits_for_operation_free_boundary},
        Case{"esave_final_save_waits_out_operation_at_stop",
             esave_final_save_waits_out_operation_at_stop},
        Case{"esave_pressure_save_outlasts_busy_operation",
             esave_pressure_save_outlasts_busy_operation},
        Case{"esave_pressure_save_prepared_before_resident_reserves",
             esave_pressure_save_prepared_before_resident_reserves},
        Case{"esave_restore_waits_for_operation_free_boundary",
             esave_restore_waits_for_operation_free_boundary},
        Case{"esave_held_export_returns_after_engine_failure",
             esave_held_export_returns_after_engine_failure},
        Case{"esave_held_prepare_bounds_resident_reclaim",
             esave_held_prepare_bounds_resident_reclaim},
        Case{"esave_held_prepare_bounds_admission", esave_held_prepare_bounds_admission},
    };
    int failures = 0;
    for (const auto& test : cases) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
