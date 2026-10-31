// Host-native EngineCore lifecycle contracts. A scripted model contract drives the production
// Engine worker, Scheduler, ResourceManager, durable checkpoint adapter and response delivery
// without a GPU, artifact or CUDA work. Native physical behavior belongs to the model's own
// transaction tests.
#include "runtime/engine/engine_core.h"

#include "cuda_interpose.h"
#include "runtime/contract/continuation_checkpoint.h"
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
// Every scripted checkpoint, import reservation destination and staged export copy occupies
// this much counted Host memory.
constexpr std::size_t kCheckpointHostBytes = 4096;
constexpr std::size_t kHostCapacityBytes   = 1U << 20;
constexpr std::uint32_t kDurableFrontier   = 4;
constexpr auto kSession                    = "session";
// A publication that correctly precedes its result is held this long, while no observer can
// return; a result delivered first is read by its observer within this interval.
constexpr auto kPublicationHold = 500ms;
constexpr auto kEventTimeout    = 10s;

void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

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
        bool export_blocked   = false;
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
    InvalidBinding,  // Binding publishes without a sequence; Engine must deliver its error.
    ControlRejected, // The first token requests target control; its preview violates grammar.
    DeadEnd,         // The first token's constraint mask has no legal token.
    DecodeFailure,   // Decode raises an internal error that fails the Engine.
    IsolatedMiss,    // The first legal unit never fits although total capacity could hold it.
    StateSlotMiss,   // No Device state can be reclaimed for an isolated request.
    Pressure,        // Stall; its unit cannot be reserved while a victim is active.
    VictimHold,      // Stall; its pause stays in flight until cancellation aborts it.
    VictimPause,     // Stall; its pause publishes Replay state, whose restore never fits.
    ExportPressure, // Stall; released units need the capacity pinned by a checkpoint export.
};

[[nodiscard]] bool stalls(Script script) noexcept {
    return script == Script::Stall || script == Script::DecodeFailure ||
           script == Script::Pressure || script == Script::VictimHold ||
           script == Script::VictimPause || script == Script::ExportPressure;
}

[[nodiscard]] bool victim(Script script) noexcept {
    return script == Script::VictimHold || script == Script::VictimPause;
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

// No scripted scenario reclaims a checkpoint, so every catalog plan is empty.
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
        if (checkpoint) { return std::nullopt; }
        Source source;
        source.remaining_work =
            rt::make_prefill_work(0, base.summary().prompt_tokens, 0, 0, kPrefillChunk);
        return source;
    }

    [[nodiscard]] BindingReservation start_binding(const Plan& base, rt::LaneId lane,
                                                   const Source& source, Resume* resume = nullptr,
                                                   UnitKind = UnitKind::Decode,
                                                   std::uint32_t = 1) {
        require(!operation_ && !lanes_.at(lane.value).active && !source.checkpoint,
                "scripted binding overlaps native ownership");
        if (base.script() == Script::IsolatedMiss ||
            (resume != nullptr && base.script() == Script::VictimPause)) {
            return {.capacity_possible = true, .shortage = {.kv_page_groups = {1}}};
        }
        if (base.script() == Script::StateSlotMiss) {
            return {.capacity_possible = true, .shortage = {.state_slots = 1}};
        }
        operation_ = Operation{.kind          = ContextKind::Bind,
                               .lane          = lane.value,
                               .script        = base.script(),
                               .prompt_tokens = base.summary().prompt_tokens,
                               .publish       = base.summary().publish_continuation};
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
                        .prompt_tokens = operation.prompt_tokens,
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
        FinishResult result;
        if (sequence.lane < lanes_.size() && lanes_[sequence.lane].active &&
            lanes_[sequence.lane].epoch == sequence.epoch && lanes_[sequence.lane].publish) {
            result.checkpoint = publish_checkpoint(lanes_[sequence.lane].prompt_tokens);
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
                          .host_bytes     = kHostCapacityBytes};
        for (const auto& lane : lanes_) {
            usage.occupied.state_slots += lane.active ? 1U : 0U;
            usage.occupied.kv_page_groups[0] += lane.active ? 1U : 0U;
        }
        const auto checkpoints = static_cast<std::size_t>(std::count_if(
            records_.begin(), records_.end(), [](const Record& record) { return record.valid; }));
        usage.host_reserved_bytes = reserved_points_ * kCheckpointHostBytes;
        usage.occupied.host_bytes =
            (checkpoints + staged_exports_) * kCheckpointHostBytes + usage.host_reserved_bytes;
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

    [[nodiscard]] bool valid_checkpoint(Checkpoint checkpoint) const noexcept {
        return checkpoint.index < records_.size() && records_[checkpoint.index].valid;
    }

    // Like native checkpoint storage, a record pinned by an export lease cannot be erased.
    [[nodiscard]] bool release_checkpoint(Checkpoint checkpoint) noexcept {
        if (!valid_checkpoint(checkpoint) || records_[checkpoint.index].pins != 0) { return false; }
        records_[checkpoint.index].valid = false;
        return true;
    }

    [[nodiscard]] Metadata checkpoint_metadata(Checkpoint checkpoint) const {
        const auto& record = live_record(checkpoint);
        return {.frontier = record.frontier, .leased = record.pins != 0};
    }

    // Distinct checkpoints never share a prefix key, so no request reuses another's checkpoint.
    [[nodiscard]] std::uint64_t checkpoint_key(Checkpoint checkpoint,
                                               std::uint32_t position) const {
        (void)live_record(checkpoint);
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

    // Host-only import destinations held until commit consumes or destruction releases them.
    class ImportReservation {
    public:
        ImportReservation(Program& program, std::uint32_t points) noexcept
            : program_(&program), points_(points) {}

        ImportReservation(ImportReservation&& other) noexcept
            : program_(std::exchange(other.program_, nullptr)), points_(other.points_) {}

        ImportReservation& operator=(ImportReservation&&) = delete;

        ~ImportReservation() {
            if (program_ != nullptr) { program_->release_reservation(points_); }
        }

        [[nodiscard]] rt::DurablePrepareState state() const noexcept {
            return rt::DurablePrepareState::Prepared;
        }

        [[nodiscard]] rt::DurableReason reason() const noexcept { return rt::DurableReason::None; }

        [[nodiscard]] rt::DurableCheckpointStats stats() const noexcept { return durable_stats(); }

    private:
        friend class Program;
        Program* program_     = nullptr;
        std::uint32_t points_ = 0;
    };

    // Pins its checkpoints and stages their copy in counted Host memory until it retires.
    class ExportLease {
    public:
        ExportLease(Program& program, std::span<const Checkpoint> points)
            : program_(&program), points_(points.begin(), points.end()) {
            for (const auto point : points_) { ++program.live_record(point).pins; }
            ++program.staged_exports_;
        }

        ExportLease(ExportLease&& other) noexcept
            : program_(std::exchange(other.program_, nullptr)), points_(std::move(other.points_)) {}

        ExportLease& operator=(ExportLease&&) = delete;

        ~ExportLease() {
            if (program_ != nullptr) { program_->retire_export(points_); }
        }

        [[nodiscard]] rt::DurablePrepareState state() const noexcept {
            return rt::DurablePrepareState::Prepared;
        }

        [[nodiscard]] rt::DurableReason reason() const noexcept { return rt::DurableReason::None; }

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
    };

    [[nodiscard]] rt::DurableNativeLayout checkpoint_layout() const { return {}; }

    [[nodiscard]] static rt::DurableCheckpointSet
    decode_checkpoint_index(std::span<const std::byte>, const rt::DurableLimits&) {
        rt::DurableCheckpointSet set;
        set.points.resize(1);
        return set;
    }

    struct ImportValidation {
        enum class State : std::uint8_t { Validated } state = State::Validated;
        rt::DurableReason reason = rt::DurableReason::None;
    };

    [[nodiscard]] ImportValidation validate_checkpoint_import(const rt::DurableCheckpointSet&,
                                                              const rt::ContinuationCheckpointReader&,
                                                              const rt::DurableLimits&) const {
        return {};
    }

    [[nodiscard]] ImportReservation prepare_checkpoint_import(const rt::DurableCheckpointSet& set,
                                                              const rt::ContinuationCheckpointReader&,
                                                              const rt::DurableLimits&) {
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

private:
    struct Lane {
        bool active                 = false;
        Script script               = Script::Complete;
        std::uint64_t epoch         = 0;
        std::uint32_t prompt_tokens = 0;
        bool publish                = false;
    };

    struct Operation {
        ContextKind kind            = ContextKind::Bind;
        std::uint32_t lane          = 0;
        Script script               = Script::Complete;
        std::uint32_t prompt_tokens = 0;
        bool publish                = false;
    };

    struct Record {
        std::uint32_t frontier = 0;
        std::uint32_t pins     = 0;
        bool valid             = false;
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

    void retire_export(std::span<const Checkpoint> points) noexcept {
        for (const auto point : points) { --records_[point.index].pins; }
        --staged_exports_;
        gate_->changed();
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

    Events& events_;
    std::shared_ptr<PublicationGate> gate_;
    std::array<rt::KvPoolDescriptor, 1> pools_{
        rt::KvPoolDescriptor{.id = {0}, .label = "main", .device_page_groups = kPageGroups}};
    std::array<Lane, ninfer::kMaximumConcurrency> lanes_{};
    std::optional<Operation> operation_;
    std::uint64_t next_epoch_ = 1;
    // Index 0 never names a checkpoint.
    std::vector<Record> records_{Record{}};
    std::size_t reserved_points_ = 0;
    std::size_t staged_exports_  = 0;
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
    [[nodiscard]] std::optional<std::uint64_t> file_size(std::string_view path) const override {
        return path == "engine/index.cbor" ? std::optional<std::uint64_t>(1) : std::nullopt;
    }

    [[nodiscard]] std::optional<rt::DurablePayload> file_descriptor(std::string_view) const override {
        return std::nullopt;
    }

    bool read_file(std::string_view, std::uint64_t, std::span<std::byte> destination) const override {
        std::fill(destination.begin(), destination.end(), std::byte{});
        return true;
    }

    [[nodiscard]] bool verify_complete() const override { return true; }

    [[nodiscard]] rt::DurableReason failure_reason() const noexcept override {
        return rt::DurableReason::None;
    }

    [[nodiscard]] std::shared_ptr<const rt::ContinuationCheckpointReader>
    fresh_reader() const override {
        return std::make_shared<IndexReader>();
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
    explicit Harness(std::uint32_t concurrency, bool context_cache = false) {
        instance_.program = std::make_unique<Program>(events_, gate_);
        ninfer::EngineOptions options;
        options.max_concurrency       = concurrency;
        options.max_context           = 64;
        options.context_cache.enabled = context_cache;
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

    // A turn of the durable session: its completed continuation becomes a named checkpoint owner.
    [[nodiscard]] Submission submit_turn(Script script, const std::string& tag) {
        Prompt prompt{.script = script, .publish_continuation = true};
        const ninfer::PromptSummary summary{.prompt_tokens = prompt.tokens};
        rt::ResolvedRequestOptions options;
        options.execution.requested_output_tokens = 1;
        return core_->submit(std::move(prompt), summary, 0.0, std::move(options),
                             ninfer::OutputConsumerMode::Aggregate, {}, {}, kSession, tag);
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
    [[nodiscard]] rt::DurableExportResult save(const std::string& tag) {
        NullWriter writer;
        return core_->export_session(kSession, tag, writer, {});
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
    const auto outcome = harness.await(request);
    require_request_error(outcome, RequestErrorKind::Unavailable, "isolated restore miss");
    require_membership(outcome.stats, {}, "isolated restore miss");
    require(harness.core().is_available(), "isolated restore miss failed the Engine");
    require_finished(harness.await(pressure), FinishReason::OutputLimit, "pause requester");
    auto next = harness.submit(Script::Complete, 1);
    require_finished(harness.await(next), FinishReason::OutputLimit,
                     "request after an isolated restore miss");
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
        Case{"control_rejection", control_rejection},
        Case{"constraint_dead_end", constraint_dead_end},
        Case{"engine_failure", engine_failure},
        Case{"isolated_admission_refusal", isolated_admission_refusal},
        Case{"isolated_restore_refusal", isolated_restore_refusal},
        Case{"isolated_state_slot_refusal", isolated_state_slot_refusal},
        Case{"durable_import_commit", durable_import_commit},
        Case{"durable_import_abort", durable_import_abort},
        Case{"durable_export_release", durable_export_release},
        Case{"resident_waits_for_export", resident_waits_for_export},
        Case{"late_turn_after_acknowledgement", late_turn_after_acknowledgement},
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
