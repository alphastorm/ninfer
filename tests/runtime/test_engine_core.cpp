// Host-native EngineCore lifecycle contracts. A scripted model contract drives the production
// Engine worker, Scheduler, ResourceManager and response delivery without a GPU, artifact or
// CUDA work. Native physical behavior belongs to the model's own transaction tests.
#include "runtime/engine/engine_core.h"

#include "cuda_interpose.h"
#include "runtime/engine/context_cache/context_cost.h"
#include "text/grammar.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
// A publication that correctly precedes its response is held this long, while no consumer can
// return; a response delivered first is read by its consumer within this interval.
constexpr auto kPublicationHold = 500ms;
constexpr auto kEventTimeout    = 10s;

void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

// The observed request's planning state is released immediately before it becomes terminal.
// The next statistics publication then waits until that request's consumer has read statistics.
// A response delivered before publication lets its consumer read the earlier snapshot.
class PublicationGate {
public:
    void planning_retired() {
        std::lock_guard lock(mutex_);
        retired_ = true;
    }

    void hold_publication() {
        std::unique_lock lock(mutex_);
        if (!retired_ || released_) { return; }
        released_ = true;
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
    bool retired_       = false;
    bool released_      = false;
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

    // Stalled prefills make no progress until released.
    std::atomic<bool> stall_released{false};

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    State state_;
};

enum class Script : std::uint8_t {
    Complete,        // One prefill chunk; the first committed token reaches the output limit.
    Stall,           // Prefill makes no progress until released, then completes.
    HoldBinding,     // Binding stays in flight until cancellation aborts it.
    ControlRejected, // The first token requests target control; its preview violates grammar.
    DeadEnd,         // The first token's constraint mask has no legal token.
    DecodeFailure,   // Decode raises an internal error that fails the Engine.
    Pressure,        // Stall; its unit cannot be reserved while a victim is active.
    VictimHold,      // Stall; its pause stays in flight until cancellation aborts it.
    VictimPause,     // Stall; its pause publishes Replay state.
};

[[nodiscard]] bool stalls(Script script) noexcept {
    return script == Script::Stall || script == Script::DecodeFailure ||
           script == Script::Pressure || script == Script::VictimHold ||
           script == Script::VictimPause;
}

[[nodiscard]] bool victim(Script script) noexcept {
    return script == Script::VictimHold || script == Script::VictimPause;
}

using Resources = rt::ContextResources<1>;

struct Prompt {
    Script script        = Script::Complete;
    std::uint32_t tokens = 4;
    std::shared_ptr<PublicationGate> observed;
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
        if (observed_) { observed_->planning_retired(); }
    }

    [[nodiscard]] Script script() const noexcept { return script_; }

    [[nodiscard]] const rt::RequestPlanSummary& summary() const noexcept { return summary_; }

    [[nodiscard]] const Advice& context_cache() const noexcept { return advice_; }

    [[nodiscard]] std::vector<std::uint32_t> capture_frontiers() const { return {}; }

    [[nodiscard]] std::optional<std::uint64_t> prefix_shortlist_key(std::uint32_t) const noexcept {
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

    [[nodiscard]] bool constraint_failed(std::size_t row) const {
        return constraint_failed_.at(row) != 0;
    }

    [[nodiscard]] rt::ExecutionTiming execution_timing() const noexcept { return {}; }

    [[nodiscard]] Sequence sequence(std::size_t row) const { return sequences_.at(row); }

private:
    std::vector<Sequence> sequences_;
    std::vector<TokenId> tokens_;
    std::vector<unsigned char> constraint_failed_;
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
    std::size_t host_bytes = 0;
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
    std::size_t host_bytes = 0;
    std::vector<Checkpoint> sources;
    Resources released;
};

struct Release {
    std::vector<Checkpoint> sources;
    Resources released;
};

// The scripted contract never publishes a checkpoint, so every catalog plan is empty.
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

// Scripted native ownership: lanes hold sequences, one context operation is in flight at a time,
// and every sequence is released exactly once by commit, finish, abort or its context operation.
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
            .publish_continuation    = false,
        };
        events_.record([](Events::State& state) { ++state.planned; });
        return Plan(prompt.script, summary, std::move(prompt.observed));
    }

    [[nodiscard]] std::optional<Source> inspect_source(const Plan& base,
                                                       std::optional<Checkpoint> checkpoint,
                                                       bool = false,
                                                       std::span<const Checkpoint> = {},
                                                       std::span<const Checkpoint> = {}) const {
        if (checkpoint) { return std::nullopt; }
        Source source;
        source.remaining_work =
            rt::make_prefill_work(0, base.summary().prompt_tokens, 0, 0, kPrefillChunk);
        return source;
    }

    [[nodiscard]] BindingReservation start_binding(const Plan& base, rt::LaneId lane,
                                                   const Source& source, Resume* = nullptr,
                                                   UnitKind = UnitKind::Decode,
                                                   std::uint32_t = 1) {
        require(!operation_ && !lanes_.at(lane.value).active && !source.checkpoint,
                "scripted binding overlaps native ownership");
        operation_ = Operation{.kind          = ContextKind::Bind,
                               .lane          = lane.value,
                               .script        = base.script(),
                               .prompt_tokens = base.summary().prompt_tokens};
        if (base.script() == Script::HoldBinding) {
            events_.record([](Events::State& state) { state.binding_held = true; });
        }
        return {.reserved = true};
    }

    [[nodiscard]] ContextProgress poll_context(rt::CancellationFlagView cancellation) {
        require(operation_.has_value(), "context progress has no operation");
        const auto operation = *operation_;
        const bool held = (operation.kind == ContextKind::Bind &&
                           operation.script == Script::HoldBinding) ||
                          (operation.kind == ContextKind::Pause &&
                           operation.script == Script::VictimHold);
        ContextProgress progress;
        progress.kind = operation.kind;
        if (held && !cancellation.requested()) { return progress; }
        operation_.reset();
        progress.complete = true;
        auto& lane        = lanes_.at(operation.lane);
        if (operation.kind == ContextKind::Bind) {
            if (held) { return progress; }
            lane = Lane{.active        = true,
                        .script        = operation.script,
                        .epoch         = next_epoch_++,
                        .prompt_tokens = operation.prompt_tokens};
            progress.published = true;
            progress.sequence  = Sequence{operation.lane, lane.epoch};
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
        if (live(units.front().sequence).script == Script::Pressure &&
            std::any_of(lanes_.begin(), lanes_.end(),
                        [](const Lane& lane) { return lane.active && victim(lane.script); })) {
            return {.shortage = {.kv_page_groups = {1}}};
        }
        return {.reserved = true};
    }

    [[nodiscard]] PrefillProgress advance_prefill(Sequence sequence,
                                                  rt::ExecutionTiming* = nullptr,
                                                  rt::TokenMaskProvider* = nullptr) {
        const auto& lane = live(sequence);
        if (stalls(lane.script) && !events_.stall_released.load()) { return {}; }
        PrefillProgress progress;
        progress.summary                 = {.prompt_tokens = lane.prompt_tokens};
        progress.processed_prompt_tokens = lane.prompt_tokens;
        progress.complete                = true;
        progress.pending.emplace(std::span<const Sequence>(&sequence, 1),
                                 lane.script == Script::DeadEnd);
        return progress;
    }

    [[nodiscard]] ReplayProgress advance_replay(Sequence, rt::ExecutionTiming* = nullptr) {
        throw std::logic_error("scripted Program has no Replay");
    }

    [[nodiscard]] PendingBatch decode(std::span<const Sequence> sequences,
                                      std::span<const rt::RoundBudget>,
                                      rt::ExecutionTiming* = nullptr,
                                      rt::TokenMaskProvider* = nullptr) {
        for (const auto sequence : sequences) {
            if (live(sequence).script == Script::DecodeFailure) {
                throw std::logic_error("injected decode failure");
            }
        }
        return PendingBatch(sequences, false);
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
            (void)live(sequence);
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
        return {.status = release(sequence)};
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
        usage.capacity = {.state_slots = ninfer::kMaximumConcurrency,
                          .kv_page_groups = {kPageGroups}};
        for (const auto& lane : lanes_) {
            usage.occupied.state_slots += lane.active ? 1U : 0U;
            usage.occupied.kv_page_groups[0] += lane.active ? 1U : 0U;
        }
        return usage;
    }

    [[nodiscard]] std::optional<std::size_t> pause_host_bytes(Sequence) const {
        return std::nullopt;
    }

    [[nodiscard]] std::size_t release_redundant_host(std::span<const Checkpoint>,
                                                     std::optional<Sequence> = std::nullopt) {
        return 0;
    }

    [[nodiscard]] bool start_pause(Sequence sequence, bool, rt::ExecutionTiming* = nullptr) {
        const auto& lane = live(sequence);
        require(!operation_, "pause overlaps a context operation");
        operation_ = Operation{.kind          = ContextKind::Pause,
                               .lane          = sequence.lane,
                               .script        = lane.script,
                               .prompt_tokens = lane.prompt_tokens};
        events_.record([](Events::State& state) { state.pause_started = true; });
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

    [[nodiscard]] bool valid_checkpoint(Checkpoint) const noexcept { return false; }

    [[nodiscard]] bool release_checkpoint(Checkpoint) noexcept { return false; }

    [[nodiscard]] Metadata checkpoint_metadata(Checkpoint) const {
        throw std::logic_error("scripted Program has no checkpoints");
    }

    [[nodiscard]] std::uint64_t checkpoint_key(Checkpoint, std::uint32_t) const {
        throw std::logic_error("scripted Program has no checkpoints");
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

    [[nodiscard]] std::size_t host_bytes_released(std::span<const Checkpoint>) const { return 0; }

    [[nodiscard]] ReclaimPlan plan_reclaim(std::span<const Checkpoint>, std::span<const Checkpoint>,
                                           Resources) const {
        return {};
    }

    [[nodiscard]] std::vector<Release> plan_releases(std::span<const Checkpoint>,
                                                     std::span<const Checkpoint>,
                                                     Resources) const {
        return {};
    }

    [[nodiscard]] bool start_demote(const Demotion&) { return false; }

    [[nodiscard]] ninfer::MemorySummary memory_summary() const { return {}; }

    void reset_memory_peaks() noexcept {}

private:
    struct Lane {
        bool active                 = false;
        Script script               = Script::Complete;
        std::uint64_t epoch         = 0;
        std::uint32_t prompt_tokens = 0;
    };

    struct Operation {
        ContextKind kind            = ContextKind::Bind;
        std::uint32_t lane          = 0;
        Script script               = Script::Complete;
        std::uint32_t prompt_tokens = 0;
    };

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

    Events& events_;
    std::shared_ptr<PublicationGate> gate_;
    std::array<rt::KvPoolDescriptor, 1> pools_{
        rt::KvPoolDescriptor{.id = {0}, .label = "main", .device_page_groups = kPageGroups}};
    std::array<Lane, ninfer::kMaximumConcurrency> lanes_{};
    std::optional<Operation> operation_;
    std::uint64_t next_epoch_ = 1;
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

class Harness {
public:
    explicit Harness(std::uint32_t concurrency) {
        instance_.program = std::make_unique<Program>(events_, gate_);
        ninfer::EngineOptions options;
        options.max_concurrency       = concurrency;
        options.max_context           = 64;
        options.context_cache.enabled = false;
        core_.emplace(instance_, device(), options, rt::generic_context_machine_cost_model());
    }

    Harness(const Harness&)            = delete;
    Harness& operator=(const Harness&) = delete;

    [[nodiscard]] Submission submit(Script script, std::uint32_t output_tokens,
                                     bool observed = false,
                                     Clock::time_point pending_deadline = {}) {
        Prompt prompt{.script = script, .observed = observed ? gate_ : nullptr};
        const ninfer::PromptSummary summary{.prompt_tokens = prompt.tokens};
        rt::ResolvedRequestOptions options;
        options.execution.requested_output_tokens = output_tokens;
        return core_->submit(std::move(prompt), summary, 0.0, std::move(options),
                             ninfer::OutputConsumerMode::Aggregate, {}, pending_deadline);
    }

    // Waits as an HTTP handler would, then reads runtime statistics before anything else runs.
    [[nodiscard]] Outcome await(Submission& submission, bool cancel = false) {
        Outcome outcome;
        try {
            outcome.result = submission.wait(
                nullptr, cancel ? ninfer::CancellationView([] { return true; })
                                : ninfer::CancellationView{});
        } catch (...) { outcome.error = std::current_exception(); }
        outcome.stats = core_->runtime_stats();
        gate_->consumer_read();
        return outcome;
    }

    void release_stalls() { events_.stall_released.store(true); }

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
    (void)require_error<std::logic_error>(harness.await(waiting), "engine failure queue");
}

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
        Case{"pausing_cancellation", pausing_cancellation},
        Case{"paused_cancellation", paused_cancellation},
        Case{"control_rejection", control_rejection},
        Case{"constraint_dead_end", constraint_dead_end},
        Case{"engine_failure", engine_failure},
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
