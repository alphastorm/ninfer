#include "runtime/contract/checkpoint_timing.h"
#include "runtime/engine/checkpoint_control.h"
#include "runtime/engine/context_cache/types.h"
#include "serve/automatic_checkpoint_queue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
// Caller injection reaches real task/queue allocations. Post-writer injection also
// covers the worker, including the next command drain after an inline export.
thread_local std::ptrdiff_t allocations_before_failure = -1;
thread_local std::size_t allocation_calls = 0;
thread_local bool checkpoint_worker_thread = false;
std::atomic<bool> fail_worker_allocations{false};
std::atomic<std::size_t> injected_worker_drains{0};
}

void* operator new(std::size_t size) {
    ++allocation_calls;
    if (allocations_before_failure == 0 ||
        (checkpoint_worker_thread && fail_worker_allocations.load())) {
        allocations_before_failure = -1;
        throw std::bad_alloc();
    }
    if (allocations_before_failure > 0) { --allocations_before_failure; }
    if (void* memory = std::malloc(size ? size : 1)) { return memory; }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using ninfer::RuntimeCheckpointRestoreStats;
using namespace ninfer::runtime;
using namespace std::chrono_literals;

void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

// Instantiate the production adapter with host-native ownership below. No CUDA,
// disk payload codec or HTTP behavior is claimed by this fixture.
struct Contract {
    struct CacheSessionKey {
        std::array<char, 128> bytes;
        std::uint16_t size = 0;
    };
    struct Program {};
    struct Instance { Program* program = nullptr; };
    struct Resources {};
    struct Request {
        bool finished = false;
        std::string checkpoint_session = "session";
        std::string checkpoint_tag = "unfinished-turn";
        bool is_model_finished() const { return finished; }
    };
};

DurableCheckpointStats checkpoint_stats() {
    DurableCheckpointStats stats;
    stats.frontier_tokens = 64;
    stats.restored_tokens = 64;
    stats.payload_bytes = 1;
    stats.points = 3;
    stats.states = 3;
    return stats;
}

struct ExportProgram {
    struct Usage { struct { std::size_t host_bytes = 0; } occupied; };
    std::size_t occupied_host = 0;
    Usage physical_usage() const { return {{occupied_host}}; }
    unsigned redundant_calls = 0;
    unsigned reclaim_calls = 0;
    // Lossless cleanup that frees this much Host room ends a Host shortage (E89 Host-first tests).
    std::size_t redundant_room = 0;
    std::size_t release_redundant_host(std::span<const int>) {
        ++redundant_calls;
        if (!redundant_room) { return 0; }
        occupied_host -= std::exchange(redundant_room, 0);
        host_refusal = false;
        return 1;
    }
    struct Image {
        bool device = false;
        unsigned readers = 0;
        bool reclaimable = true;
    };
    std::vector<Image> images;
    bool refuse_export = false;
    bool host_refusal = false;
    bool serialize_exports = false;
    unsigned export_acquisitions = 0;
    unsigned export_retirements = 0;
    bool off_worker_retirement = false;
    unsigned staged_prepares = 0;
    static constexpr unsigned device_capacity = 3;
    // A native context operation in flight: preparation refuses as a busy transaction, as
    // checkpoint_api.cpp does for exports and imports alike.
    bool operation_in_flight = false;
    unsigned export_prepares = 0;
    unsigned import_prepares = 0;
    bool has_context_operation() const { return operation_in_flight; }

    struct Reservation {
        std::size_t count;
        DurableReason refusal = DurableReason::None;
        DurablePrepareState state() const {
            return refusal == DurableReason::None ? DurablePrepareState::Prepared
                                                  : DurablePrepareState::CapacityRefused;
        }
        DurableReason reason() const { return refusal; }
        DurableCheckpointStats stats() const { return checkpoint_stats(); }
    };

    struct Lease {
        ExportProgram* owner = nullptr;
        std::vector<int> handles;
        std::thread::id worker;
        bool host_refusal = false;
        bool busy = false;
        bool pinned = false;
        std::size_t staged_states = 0;
        Lease(ExportProgram& program, std::span<const int> points, bool device_staging = false) {
            ++program.export_prepares;
            // A Device-staged attempt reads Device-only images from Device: no Host shortage.
            host_refusal = program.host_refusal && !device_staging;
            busy = program.operation_in_flight ||
                   (program.serialize_exports && program.export_acquisitions != program.export_retirements);
            if (busy) { return; }
            if (program.refuse_export) { return; }
            if (host_refusal) { return; }
            // Like StateImageStore, a Device image another export reads is Busy for staging.
            pinned = device_staging && std::any_of(points.begin(), points.end(), [&](int point) {
                return program.images.at(point).device && program.images.at(point).readers;
            });
            if (pinned) { return; }
            handles.assign(points.begin(), points.end());
            owner = &program;
            worker = std::this_thread::get_id();
            ++owner->export_acquisitions;
            for (const int point : handles) { ++owner->images.at(point).readers; }
            if (device_staging) {
                ++owner->staged_prepares;
                staged_states = static_cast<std::size_t>(std::count_if(handles.begin(), handles.end(),
                    [&](int point) { return owner->images.at(point).device; }));
            }
        }
        Lease(Lease&& other) noexcept
            : owner(std::exchange(other.owner, nullptr)), handles(std::move(other.handles)),
              worker(other.worker), host_refusal(other.host_refusal), busy(other.busy),
              pinned(other.pinned), staged_states(other.staged_states) {}
        ~Lease() {
            if (owner) {
                owner->off_worker_retirement |= std::this_thread::get_id() != worker;
                ++owner->export_retirements;
                for (const int point : handles) { --owner->images.at(point).readers; }
            }
        }
        DurablePrepareState state() const {
            return owner ? DurablePrepareState::Prepared : DurablePrepareState::CapacityRefused;
        }
        DurableReason reason() const {
            return owner ? DurableReason::None : busy ? DurableReason::TransactionBusy : DurableReason::Capacity;
        }
        enum class HostKind { Prepared, Busy, HostCapacity, HostContiguity };
        HostKind host_shortage_kind() const {
            return pinned ? HostKind::Busy : host_refusal ? HostKind::HostCapacity : HostKind::Prepared;
        }
        std::size_t host_shortage_bytes() const { return host_refusal ? 1 : 0; }
        std::size_t device_staged_states() const { return staged_states; }
        DurableExportResult write(ContinuationCheckpointWriter& writer) {
            if (!owner) { return {.reason = DurableReason::Capacity}; }
            const std::array<std::byte, 1> bytes{};
            if (!writer.write_file("engine/index.cbor", 0, 1, bytes, 1) || !writer.finish()) {
                return {.reason = DurableReason::IoUnavailable};
            }
            return {.state = DurableExportState::Exported, .stats = checkpoint_stats()};
        }
    };

    static DurableCheckpointSet decode_checkpoint_index(std::span<const std::byte>,
                                                         const DurableLimits&) {
        DurableCheckpointSet set;
        set.points.resize(3);
        return set;
    }
    DurableNativeLayout checkpoint_layout() const { return {}; }
    Reservation prepare_checkpoint_import(const DurableCheckpointSet& set,
                                            const ContinuationCheckpointReader&,
                                            const DurableLimits&) {
        ++import_prepares;
        return {set.points.size(), operation_in_flight ? DurableReason::TransactionBusy : DurableReason::None};
    }
    // This fixture never reclaims a disk victim during Host-only import.
    struct Validation {
        enum State { Validated } state = Validated;
        DurableReason reason = DurableReason::None;
    };
    Validation validate_checkpoint_import(const DurableCheckpointSet&,
                                            const ContinuationCheckpointReader&,
                                            const DurableLimits&) const { return {}; }
    std::vector<int> commit_import(Reservation&& reservation) {
        std::vector<int> points;
        for (std::size_t index = 0; index < reservation.count; ++index) {
            points.push_back(static_cast<int>(images.size()));
            images.push_back({});
        }
        return points;
    }
    Lease prepare_checkpoint_export(std::span<const int> points, const DurableLimits&,
                                    bool device_staging = false) {
        return Lease(*this, points, device_staging);
    }
    unsigned device_slots() const {
        return static_cast<unsigned>(std::count_if(images.begin(), images.end(),
                                                  [](const Image& image) { return image.device; }));
    }
    unsigned pinned_device_slots() const {
        return static_cast<unsigned>(std::count_if(images.begin(), images.end(),
            [](const Image& image) { return image.device && image.readers; }));
    }
    void complete_first_resume(std::vector<int>& points) {
        require(points.size() == 3 && device_slots() == 0, "first restored fixture is not Host-only");
        images.at(points.back()).device = true;
        for (unsigned capture = 0; capture < 2; ++capture) {
            points.push_back(static_cast<int>(images.size()));
            images.push_back({.device = true});
        }
    }
    bool reserve_after_lossless_reclaim() {
        if (device_slots() < device_capacity) { return true; }
        for (auto& image : images) {
            if (image.device && !image.readers && image.reclaimable) {
                image.device = false; // Preserve the complete image on Host, never drop it.
                return true;
            }
        }
        return false;
    }
};

struct ExportResources {
    struct Publication { std::string digest; std::string tag; };
    struct Owner { std::string tag; std::vector<int> points; };
    struct Victim { std::string session; std::string tag; };
    using DurableVictim = Victim;
    struct ReclaimCursor { std::vector<int> victims; };
    struct Shortage { std::size_t host_bytes = 0; };
    bool release_guard_enabled = false;
    void enable_release_guard(bool enabled) { release_guard_enabled = enabled; }
    ReclaimCursor begin_reclaim(ExportProgram&, std::nullopt_t, ReclaimRights) { return {{0}}; }
    std::vector<int> export_reclaim_exclusions(std::span<const int> points, bool) const {
        return {points.begin(), points.end()};
    }
    std::vector<int> proven_victim_closure(ExportProgram&, const Victim&, const ReclaimCursor&,
                                         std::span<const int>, std::span<const int>) { return {}; }
    bool release_proven_durable_victim(ExportProgram&, const Victim&, std::span<const int>,
        ReclaimCursor&, std::span<const int>, std::span<const int>) { return false; }
    ReclaimProgress reclaim(ExportProgram& program, Shortage, std::span<const int>, ReclaimCursor&) {
        ++program.reclaim_calls;
        return ReclaimProgress::Blocked;
    }
    std::map<std::string, Owner, std::less<>> owners;
    Publication prepare_import_publication(const Contract::CacheSessionKey&,
        std::optional<Contract::CacheSessionKey>, std::string digest, std::string tag, std::size_t) {
        return {std::move(digest), std::move(tag)};
    }
    void publish_import(ExportProgram&, Publication&& publication, std::span<const int> points,
                         std::uint64_t) {
        owners[publication.digest] = {std::move(publication.tag),
                                       std::vector<int>(points.begin(), points.end())};
    }
    std::vector<int> session_points(const ExportProgram&, std::string_view digest,
                                    std::string_view tag, bool& pending) const {
        pending = false;
        const auto found = owners.find(digest);
        return found != owners.end() && found->second.tag == tag ? found->second.points
                                                                 : std::vector<int>{};
    }
    std::vector<Victim> durable_victims(std::string_view) const { return {}; }
    bool release_durable_victim(ExportProgram&, const Victim&) { return false; }
    bool holds_durable_session(std::string_view digest) const { return owners.contains(digest); }
};

struct ExportContract : Contract {
    using Program = ExportProgram;
    struct Instance { Program* program = nullptr; };
    using Resources = ExportResources;
};

struct IndexReader final : ContinuationCheckpointReader {
    std::optional<std::uint64_t> file_size(std::string_view path) const override {
        return path == "engine/index.cbor" ? std::optional<std::uint64_t>(1) : std::nullopt;
    }
    std::optional<DurablePayload> file_descriptor(std::string_view) const override { return {}; }
    bool read_file(std::string_view, std::uint64_t, std::span<std::byte> destination) const override {
        std::fill(destination.begin(), destination.end(), std::byte{});
        return true;
    }
    bool verify_complete() const override { return true; }
    DurableReason failure_reason() const noexcept override { return DurableReason::None; }
    std::shared_ptr<const ContinuationCheckpointReader> fresh_reader() const override {
        return std::make_shared<IndexReader>();
    }
};

struct WriterFailure final : std::runtime_error {
    WriterFailure() : std::runtime_error("injected export writer failure") {}
};

class BlockingWriter final : public ContinuationCheckpointWriter {
    std::promise<void> entered_;
    std::future<void> entered_future_ = entered_.get_future();
    std::promise<void> release_;
    std::shared_future<void> released_ = release_.get_future().share();
    std::atomic<bool> signalled_{false};
    bool fail_;
public:
    explicit BlockingWriter(bool fail = false) : fail_(fail) {}
    void release() {
        if (!signalled_.exchange(true)) { release_.set_value(); }
    }
    void wait_until_blocked() {
        require(entered_future_.wait_for(2s) == std::future_status::ready,
                "export never reached the off-worker writer");
    }
    bool write_file(std::string_view, std::uint64_t, std::uint64_t,
                    std::span<const std::byte>, std::uint32_t) override {
        entered_.set_value();
        released_.wait();
        if (fail_) { throw WriterFailure(); }
        return true;
    }
    bool finish() override { return true; }
};

struct ReleaseWriter {
    BlockingWriter& writer;
    ~ReleaseWriter() { writer.release(); }
};

struct NullWriter final : ContinuationCheckpointWriter {
    bool write_file(std::string_view, std::uint64_t, std::uint64_t, std::span<const std::byte>,
                    std::uint32_t) override { return true; }
    bool finish() override { return true; }
};

struct AllocationFailureWriter final : ContinuationCheckpointWriter {
    bool fail;
    WriterFailure failure;
    explicit AllocationFailureWriter(bool writer_throws) : fail(writer_throws) {}
    ~AllocationFailureWriter() override { fail_worker_allocations.store(false); }
    bool write_file(std::string_view, std::uint64_t, std::uint64_t, std::span<const std::byte>,
                    std::uint32_t) override { return true; }
    bool finish() override {
        fail_worker_allocations.store(true);
        allocations_before_failure = 0;
        if (fail) { throw failure; }
        return true;
    }
};

template <class ModelContract>
class AdapterFixture {
    using Checkpoint = int;
    using Request = typename ModelContract::Request;
    typename ModelContract::Instance instance_;
    typename ModelContract::Resources resources_;
    CheckpointControl checkpoints_;
    std::mutex checkpoint_provider_mutex_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread worker_;
    std::unique_lock<std::mutex>* checkpoint_execution_lock_ = nullptr;
    std::size_t checkpoint_exports_in_flight_ = 0;
    std::size_t checkpoint_pressure_depth_ = 0;
    std::exception_ptr checkpoint_publication_failure_;
    ninfer::RuntimeStats cumulative_stats_{};
    std::atomic<bool> admission_check_pending_{false};
    bool stopping_ = false;
    bool failed_ = false;
    bool checkpoint_cache_enabled_ = true;
    std::uint64_t next_publication_order_ = 1;
    unsigned admission_notifications_ = 0;
    std::vector<int> paused_;
    bool resident_empty() const { return true; }
    void request_admission_check() {
        checkpoints_.notify_progress();
        admission_check_pending_.store(true);
        ++admission_notifications_;
    }
    // Runtime statistics publication belongs to the EngineCore harness, not this fixture.
    void publish_runtime_stats() {}
#include "runtime/engine/engine_checkpoint.inl"
private:
    std::list<CheckpointCommand> checkpoint_commands_;
    std::list<CheckpointCommand> checkpoint_command_batch_;
    std::list<CheckpointCommand> checkpoint_held_commands_;
public:
    AdapterFixture() = default;
    explicit AdapterFixture(typename ModelContract::Program& program) : instance_{&program} {
        worker_ = std::thread([this] {
            checkpoint_worker_thread = true;
            for (;;) {
                {
                    std::unique_lock lock(queue_mutex_);
                    queue_cv_.wait(lock, [&] { return stopping_ || !checkpoint_commands_.empty(); });
                    if (stopping_) { return; }
                }
                std::mutex execution;
                std::unique_lock lock(execution);
                checkpoint_execution_lock_ = &lock;
                if (fail_worker_allocations.load()) { ++injected_worker_drains; }
                run_checkpoint_commands();
                // As after the Engine's top-of-turn poll: commands held behind an operation that
                // this batch ended run now.
                run_held_checkpoint_commands();
            }
        });
    }
    ~AdapterFixture() {
        if (worker_.joinable()) {
            {
                std::lock_guard lock(queue_mutex_);
                stopping_ = true;
            }
            queue_cv_.notify_all();
            worker_.join();
        }
    }
    template <class Function>
    auto on_worker(Function&& function) {
        return checkpoint_call(std::forward<Function>(function));
    }
    void run_pressure() {
        on_worker([&] {
            PressureCheckpointVictim victim{"first", "turn"};
            (void)checkpoint_pressure({&victim, 1});
        });
    }
    void pressure_handler(std::shared_ptr<PressureCheckpointHandler> handler) {
        set_pressure_checkpoint_handler(std::move(handler));
    }
    // Restore whose epoch was captured before its disk load; None means Imported.
    DurableReason restore_captured(const std::string& session, DurableSessionEpoch captured) {
        IndexReader reader;
        DurableImportTarget target{.session_sha256 = session, .expected_tag = "turn",
            .expected_epoch = std::move(captured), .allow_prefix_reuse = true,
            .cache_participation = true};
        auto prepared = prepare_import(target, reader, {}, checkpoint_stats(), {});
        if (prepared.state != DurablePrepareState::Prepared || !prepared.commit) {
            return prepared.reason;
        }
        const auto committed = prepared.commit();
        return committed.state == DurableImportState::Imported ? DurableReason::None
                                                               : committed.reason;
    }
    void restore(std::string session) {
        require(restore_captured(session, session_epoch(session)) == DurableReason::None,
                "actual adapter did not prepare and publish the Host-only import");
    }
    DurableSessionEpoch lookup(const std::string& session) { return session_epoch(session); }
    // An Engine submission takes the session gate on its caller thread before queueing.
    void submit(const std::string& session, std::uint64_t order) {
        const auto gate = checkpoints_.session(session);
        std::lock_guard lock(gate->mutex);
        gate->submitted(order);
    }
    void evict(const std::string& session) {
        on_worker([&] { resources_.owners.erase(session); });
    }
    auto reserve_turn() { return reserve_checkpoint_command([] {}); }
    void turn() { on_worker([] {}); }
    std::size_t gates() const { return checkpoints_.gate_count(); }
    void complete_first_resume() {
        on_worker([&] { instance_.program->complete_first_resume(resources_.owners.at("first").points); });
    }
    DurableExportResult save(const std::string& session, ContinuationCheckpointWriter& writer,
                             DurableSaveIntent intent = DurableSaveIntent::Automatic) {
        return export_session(session, "turn", writer, {}, intent);
    }
    DurableExportResult save(ContinuationCheckpointWriter& writer) { return save("first", writer); }
    DurableExportResult save_with_summary(const std::string& session, ContinuationCheckpointWriter& writer,
                                          DurableExportReclaim& summary) {
        return export_session(session, "turn", writer, {}, DurableSaveIntent::Automatic, &summary);
    }
    std::size_t exports_in_flight() { return on_worker([&] { return checkpoint_exports_in_flight_; }); }
    // Gives `to` the same native points as `from`, so their exports read one set of images.
    void share_points(const std::string& from, const std::string& to) {
        on_worker([&] { resources_.owners[to].points = resources_.owners.at(from).points; });
    }
    bool isolated_admission() {
        return on_worker([&] {
            if (instance_.program->reserve_after_lossless_reclaim()) { return true; }
            // Invoke the exact production decision used after a capacity-possible
            // binding fails, with no resident/paused work and no pressure callback.
            if (admission_capacity_progress_pending(false)) { return false; }
            throw std::logic_error("isolated request misclassified finite export pins");
        });
    }
    std::pair<std::uint64_t, unsigned> progress() {
        return on_worker([&] {
            return std::pair(checkpoint_progress(), admission_notifications_);
        });
    }
    std::uint64_t observed_progress() const { return checkpoint_progress(); }
    void save_completed() { notify_checkpoint_progress(); }
    // Observed engine progress, as a completed native context operation or admission reports.
    void admission_progress() { on_worker([&] { request_admission_check(); }); }
    bool capacity_pending() {
        return on_worker([&] { return admission_capacity_progress_pending(false); });
    }
    bool settle(Request request, std::shared_ptr<PressureCheckpointHandler> handler) {
        checkpoints_.set_handler(std::move(handler));
        std::mutex execution;
        std::unique_lock lock(execution);
        checkpoint_execution_lock_ = &lock;
        return checkpoint_request_settled(request);
    }
};

void isolated_import_waits_for_export(bool writer_throws) {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.complete_first_resume();
    BlockingWriter writer(writer_throws);
    auto saving = std::async(std::launch::async, [&] { return core.save(writer); });
    ReleaseWriter release{writer};
    writer.wait_until_blocked();
    core.restore("second");
    require(core.on_worker([&] { return program.device_slots() == 3 &&
        program.pinned_device_slots() == 3 && program.images.size() == 8; }),
        "two-session fixture did not hold exactly three Device images and five Host images");
    const auto before = core.progress();
    require(!core.isolated_admission(), "blocked export admitted before its native pins retired");
    writer.release();
    bool threw = false;
    try {
        require(saving.get().state == DurableExportState::Exported, "export failed unexpectedly");
    } catch (const WriterFailure&) { threw = true; }
    require(threw == writer_throws, "writer exception was swallowed or manufactured");
    const auto after = core.progress();
    require(after.first == before.first + 1 && after.second == before.second + 1,
            "export retirement did not publish one observed-progress/admission event");
    require(!core.capacity_pending() && core.on_worker([&] { return program.pinned_device_slots() == 0; }),
            "completed export still owns a native reader or pending-capacity claim");
    require(core.isolated_admission() && core.on_worker([&] { return program.images.size() == 8; }),
            "second session could not admit by lossless Host demotion after export retirement");
}

void export_retirement_survives_allocation_failure(bool writer_throws, bool inline_worker = false) {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.complete_first_resume();
    const auto before = core.progress();
    AllocationFailureWriter writer(writer_throws);
    // Queue this turn without allocating after injection starts. Inline retirement
    // must also survive a fresh worker drain, not just the current callback.
    auto drain = core.reserve_turn();
    const auto drains_before = injected_worker_drains.load();
    DurableExportResult result;
    bool writer_failed = false;
    bool allocation_failed = false;
    const auto save = [&] {
        try { result = core.save(writer); }
        catch (const WriterFailure&) { writer_failed = true; }
        catch (const std::bad_alloc&) { allocation_failed = true; }
        return std::exchange(allocations_before_failure, -1) == 0;
    };
    const bool allocation_unused = inline_worker ? core.on_worker(save) : save();
    drain();
    fail_worker_allocations.store(false);
    require(injected_worker_drains.load() > drains_before,
            "post-writer allocation injection did not reach a worker command drain");
    require(!allocation_failed && allocation_unused,
            "export retirement allocated after taking its native lease");
    require(writer_failed == writer_throws &&
                (writer_throws || result.state == DurableExportState::Exported),
            "allocation failure replaced the writer result or exception");
    const auto after = core.progress();
    require(after.first == before.first + 1 && after.second == before.second + 1,
            "allocation-safe retirement did not release capacity exactly once");
    require(core.on_worker([&] {
        return program.export_acquisitions == 1 && program.export_retirements == 1 &&
               !program.off_worker_retirement && program.pinned_device_slots() == 0;
    }) && !core.capacity_pending() && core.isolated_admission(),
            "allocation-safe retirement lost worker ownership or native capacity");
}

void export_command_allocation_failures_precede_native_lease() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.complete_first_resume();
    NullWriter writer;
    const auto start = allocation_calls;
    require(core.save(writer).state == DurableExportState::Exported,
            "allocation-counting export did not complete");
    const auto allocations = allocation_calls - start;
    require(allocations != 0, "export did not exercise caller command allocation");
    const auto before = core.progress();
    for (std::size_t index = 0; index < allocations; ++index) {
        allocations_before_failure = static_cast<std::ptrdiff_t>(index);
        bool failed = false;
        try { (void)core.save(writer); }
        catch (const std::bad_alloc&) { failed = true; }
        allocations_before_failure = -1;
        require(failed, "injected command allocation failure was not exercised");
        require(core.progress() == before && !core.capacity_pending() &&
                    core.on_worker([&] {
                        return program.export_acquisitions == 1 &&
                               program.export_retirements == 1 &&
                               !program.off_worker_retirement &&
                               program.pinned_device_slots() == 0;
                    }),
                "command allocation failure acquired or retired a native export lease");
    }
    require(core.save(writer).state == DurableExportState::Exported &&
                core.progress() == std::pair(before.first + 1, before.second + 1),
            "export did not recover after command allocation failure");
}

void overlapping_exports_retain_progress_until_last_release() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.complete_first_resume();
    BlockingWriter first, second;
    auto saving_first = std::async(std::launch::async, [&] { return core.save(first); });
    ReleaseWriter release_first{first};
    first.wait_until_blocked();
    auto saving_second = std::async(std::launch::async, [&] { return core.save(second); });
    ReleaseWriter release_second{second};
    second.wait_until_blocked();
    const auto before = core.progress();
    require(!core.isolated_admission(), "overlapping exports did not defer isolated admission");
    first.release();
    require(saving_first.get().state == DurableExportState::Exported, "first overlapping export failed");
    require(core.progress() == std::pair(before.first + 1, before.second + 1) &&
                core.capacity_pending() && !core.isolated_admission(),
            "first export retirement forgot the other export's native pins");
    second.release();
    require(saving_second.get().state == DurableExportState::Exported, "second overlapping export failed");
    const auto after = core.progress();
    require(after.first == before.first + 2 && after.second == before.second + 2 &&
                !core.capacity_pending() && core.isolated_admission(),
            "last export retirement failed to unblock lossless admission");
}

void automatic_retry_after_other_export_retires() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.restore("second");
    core.on_worker([&] { program.serialize_exports = true; });
    BlockingWriter writer;
    auto saving = std::async(std::launch::async, [&] { return core.save(writer); });
    ReleaseWriter release{writer};
    writer.wait_until_blocked();
    std::mutex mutex;
    std::condition_variable cv;
    unsigned calls = 0, completed = 0, probes = 0;
    DurableReason reason = DurableReason::None;
    const auto wait = [&](const auto& predicate) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 2s, predicate);
    };
    ninfer::serve::AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view session, std::optional<std::string_view> tag) {
            NullWriter output;
            const auto result = core.save(std::string(session), output);
            std::lock_guard lock(mutex);
            ++calls;
            reason = result.reason;
            return ninfer::serve::AutomaticCheckpointResult{std::string(*tag), result.reason};
        },
        [&] {
            std::lock_guard lock(mutex);
            ++probes;
            cv.notify_all();
            return ninfer::serve::CheckpointSaveProgress{core.observed_progress()};
        },
        [&] {
            core.save_completed();
            std::lock_guard lock(mutex);
            ++completed;
            cv.notify_all();
        });
    PressureCheckpointVictim victim{"second", "turn"};
    coordinator.save_before_eviction({&victim, 1});
    require(wait([&] { return completed == 1 && probes >= 4; }),
            "automatic save did not defer while another export held the lease");
    {
        std::lock_guard lock(mutex);
        require(calls == 1 && reason == DurableReason::TransactionBusy &&
                    victim.outcome == PressureCheckpointOutcome::Pending,
                "held export did not produce a non-spinning TransactionBusy pressure retry");
    }
    const auto clock = core.observed_progress();
    writer.release();
    require(saving.get().state == DurableExportState::Exported, "blocking export did not finish");
    require(wait([&] { return completed == 2; }),
            "another export's retirement left the automatic save Pending without engine activity");
    coordinator.save_before_eviction({&victim, 1});
    {
        std::lock_guard lock(mutex);
        require(calls == 2 && reason == DurableReason::None &&
                    victim.outcome == PressureCheckpointOutcome::Settled &&
                    core.observed_progress() == clock + 2,
                "export retirement did not retry and settle the deferred pressure save exactly once");
    }
    coordinator.drain();
}

void unchanged_reclaim_refusal_does_not_notify() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.on_worker([&] { program.refuse_export = true; program.host_refusal = true; });
    const auto before = core.progress();
    const auto clock = core.observed_progress();
    NullWriter writer;
    const auto result = core.save(writer);
    require(result.reason == DurableReason::Capacity && result.reclaim.considered &&
                result.reclaim.prepare_attempts == 1 && result.reclaim.reclaimed_owners == 0 &&
                result.reclaim.reclaimed_cache_steps == 0 && result.reclaim.reclaimed_host_bytes == 0 &&
                program.redundant_calls == 1 && program.reclaim_calls == 1,
            "unchanged refusal did not exhaust the lossless and blocked ordinary tiers");
    require(core.progress() == before && core.observed_progress() == clock,
            "unchanged reclaim refusal emitted a capacity/admission notification");
}

void genuine_capacity_and_unprepared_export_do_not_defer() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.complete_first_resume();
    core.on_worker([&] {
        for (auto& image : program.images) { image.reclaimable = false; }
        program.refuse_export = true;
    });
    const auto before = core.progress();
    BlockingWriter unused;
    require(core.save(unused).state == DurableExportState::Refused &&
                core.progress() == before && !core.capacity_pending(),
            "unprepared export manufactured capacity progress");
    bool refused = false;
    try { (void)core.isolated_admission(); }
    catch (const std::logic_error&) { refused = true; }
    require(refused, "genuine isolated capacity impossibility was silently deferred");
}

void paused_request_settles_by_native_replay() {
    struct Handler final : PressureCheckpointHandler {
        unsigned calls = 0;
        void save_before_eviction(std::span<PressureCheckpointVictim> victims) noexcept override {
            ++calls;
            for (auto& victim : victims) { victim.outcome = PressureCheckpointOutcome::Pending; }
        }
    };
    auto handler = std::make_shared<Handler>();
    AdapterFixture<Contract> core;
    if (!core.settle({}, handler) || handler->calls != 0) {
        throw std::runtime_error("paused request waited for its unpublished response instead of native replay");
    }
    if (core.settle({.finished = true}, handler) || handler->calls != 1) {
        throw std::runtime_error("completed response bypassed its HTTP publication guard");
    }
}

void unseen_sessions_leave_bounded_gates() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    NullWriter writer;
    bool absent = true;
    for (std::size_t i = 0; i < 16 * CheckpointControl::kGateSweepFloor; ++i) {
        // A restore lookup whose disk load finds nothing, then a save of a session this
        // Engine never cached. Each save is also a worker turn.
        (void)core.lookup("missing-" + std::to_string(i));
        const auto save = core.save("uncached-" + std::to_string(i), writer);
        absent &= save.state == DurableExportState::Refused &&
                  save.reason == DurableReason::SessionNotIndexed;
    }
    require(core.gates() <= CheckpointControl::kGateSweepFloor,
            "unseen sessions permanently retained Engine publication gates");
    require(absent, "a never-cached session was not reported as absent");
}

void evicted_session_gate_retires_and_resident_one_stays() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("evicted");
    core.restore("resident");
    NullWriter writer;
    require(core.save("evicted", writer).state == DurableExportState::Exported,
            "fixture endpoint was not exportable before eviction");
    core.evict("evicted");
    for (std::size_t i = 0; i < 4 * CheckpointControl::kGateSweepFloor; ++i) {
        (void)core.lookup("missing-" + std::to_string(i));
        core.turn();
    }
    require(core.gates() <= CheckpointControl::kGateSweepFloor,
            "lookups and worker turns left an unbounded gate table");
    require(core.lookup("evicted").value == 0, "an evicted session kept its publication gate");
    const auto evicted = core.save("evicted", writer);
    require(evicted.state == DurableExportState::Refused &&
                evicted.reason == DurableReason::SessionNotIndexed,
            "a retired gate turned an evicted endpoint into a tag race");
    require(core.save("resident", writer).state == DurableExportState::Exported,
            "a sweep dropped the committed endpoint of a resident session");
}

void pressure_callback_export_does_not_reclaim() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    struct Handler final : PressureCheckpointHandler {
        std::function<void()> callback;
        unsigned calls = 0;
        void save_before_eviction(std::span<PressureCheckpointVictim>) noexcept override {
            ++calls;
            callback();
        }
    };
    auto handler = std::make_shared<Handler>();
    DurableExportResult result;
    NullWriter writer;
    handler->callback = [&] {
        result = core.save("first", writer, DurableSaveIntent::Explicit);
    };
    core.pressure_handler(handler);
    core.on_worker([&] { program.refuse_export = true; program.host_refusal = true; });
    core.run_pressure();
    require(handler->calls == 1 && result.reason == DurableReason::Capacity &&
                result.reclaim.prepare_attempts == 1 && !result.reclaim.considered &&
                program.redundant_calls == 0,
            "inline pressure save recursively changed an outer quoted plan");
}

void restore_lookup_pins_its_gate_through_sweeps() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    // Both restores capture their epochs before their disk loads. During the loads a turn
    // runs on one session, and unrelated lookups and worker turns sweep idle gates.
    const auto interrupted = core.lookup("interrupted");
    const auto quiet = core.lookup("quiet");
    core.submit("interrupted", 7);
    for (std::size_t i = 0; i < 4 * CheckpointControl::kGateSweepFloor; ++i) {
        (void)core.lookup("missing-" + std::to_string(i));
        core.turn();
    }
    require(core.restore_captured("interrupted", interrupted) == DurableReason::StalePublication,
            "a restore published over a turn that ran during its disk load");
    require(core.restore_captured("quiet", quiet) == DurableReason::None,
            "gate sweeps invalidated a restore with no intervening turn");
}

// E89 (fork #89): after the unchanged Host route and its reclaim are exhausted with a Host
// shortage, one Device-staged attempt saves the session.
void staged_export_after_host_route_exhausted() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.complete_first_resume();
    core.on_worker([&] { program.host_refusal = true; });
    const auto before = core.progress();
    NullWriter writer;
    DurableExportReclaim summary;
    const auto result = core.save_with_summary("first", writer, summary);
    require(result.state == DurableExportState::Exported && result.reason == DurableReason::None,
            "Host-capacity shortage was not saved through the Device-staged route");
    require(result.reclaim.considered && result.reclaim.prepare_attempts == 1 &&
                result.reclaim.device_staging_attempted && result.reclaim.device_staged_states == 3 &&
                result.reclaim.host_shortage == DurableHostShortage::HostCapacity &&
                result.reclaim.capacity_site == DurableCapacitySite::None &&
                program.redundant_calls == 1 && program.reclaim_calls == 1 && program.staged_prepares == 1,
            "staged attempt was not one attempt after the unchanged Host route and its reclaim");
    require(summary.device_staging_attempted && summary.device_staged_states == 3,
            "summary sink missed the staged attempt");
    require(core.exports_in_flight() == 0 &&
                core.on_worker([&] { return program.pinned_device_slots() == 0; }) &&
                core.progress().first == before.first + 1,
            "staged export retirement leaked Device readers or skipped its observed progress");
}

// E89 review condition (a): the staged attempt never runs when the Host route succeeds, with
// or without the reclaim that makes room for it.
void host_route_first_never_stages() {
    for (const bool shortage : {false, true}) {
        ExportProgram program;
        AdapterFixture<ExportContract> core(program);
        core.restore("first");
        core.complete_first_resume();
        core.on_worker([&] {
            program.host_refusal = shortage;
            program.occupied_host = 4;
            program.redundant_room = shortage ? 4 : 0;
        });
        NullWriter writer;
        const auto result = core.save(writer);
        require(result.state == DurableExportState::Exported && program.staged_prepares == 0 &&
                    !result.reclaim.device_staging_attempted && result.reclaim.device_staged_states == 0 &&
                    result.reclaim.prepare_attempts == (shortage ? 2U : 1U),
                "Device staging ran although the Host route, after its reclaim, succeeded");
    }
}

// E89 review condition (c) and #84: a throwing staged write releases every Device reader and
// the in-flight gate, and the caller still holds the reclaim and staging summary.
void staged_pins_retire_on_writer_failure() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.complete_first_resume();
    core.on_worker([&] { program.host_refusal = true; });
    BlockingWriter writer(true);
    DurableExportReclaim summary;
    auto saving = std::async(std::launch::async, [&] { return core.save_with_summary("first", writer, summary); });
    ReleaseWriter release{writer};
    writer.wait_until_blocked();
    require(core.on_worker([&] { return program.pinned_device_slots() == 3; }) && core.exports_in_flight() == 1,
            "staged export did not hold its Device readers through the off-lock write");
    writer.release();
    bool threw = false;
    try { (void)saving.get(); } catch (const WriterFailure&) { threw = true; }
    require(threw, "staged writer failure was swallowed or converted");
    require(core.on_worker([&] { return program.pinned_device_slots() == 0; }) && core.exports_in_flight() == 0,
            "throwing staged write left Device readers or the in-flight gate behind");
    require(summary.considered && summary.prepare_attempts == 1 && summary.device_staging_attempted &&
                summary.device_staged_states == 3 && summary.host_shortage == DurableHostShortage::HostCapacity,
            "throwing write lost the reclaim and staging summary");
}

// E89 review condition (b): a second export that meets Device images pinned by a staged export
// is a retryable Capacity refusal; the first export's retirement retries and completes it.
void staged_busy_export_retries_after_first_retires() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.complete_first_resume();
    core.restore("second");
    core.share_points("first", "second");
    core.on_worker([&] { program.host_refusal = true; });
    BlockingWriter writer;
    auto saving = std::async(std::launch::async, [&] { return core.save(writer); });
    ReleaseWriter release{writer};
    writer.wait_until_blocked();
    std::mutex mutex;
    std::condition_variable cv;
    unsigned calls = 0, completed = 0, probes = 0;
    DurableReason reason = DurableReason::None;
    const auto wait = [&](const auto& predicate) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 2s, predicate);
    };
    ninfer::serve::AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view session, std::optional<std::string_view> tag) {
            NullWriter output;
            const auto result = core.save(std::string(session), output);
            std::lock_guard lock(mutex);
            ++calls;
            reason = result.reason;
            return ninfer::serve::AutomaticCheckpointResult{std::string(*tag), result.reason};
        },
        [&] {
            std::lock_guard lock(mutex);
            ++probes;
            cv.notify_all();
            return ninfer::serve::CheckpointSaveProgress{core.observed_progress()};
        },
        [&] {
            core.save_completed();
            std::lock_guard lock(mutex);
            ++completed;
            cv.notify_all();
        });
    PressureCheckpointVictim victim{"second", "turn"};
    coordinator.save_before_eviction({&victim, 1});
    require(wait([&] { return completed == 1 && probes >= 4; }),
            "staged save did not defer while another staged export pinned its Device images");
    {
        std::lock_guard lock(mutex);
        require(calls == 1 && reason == DurableReason::Capacity &&
                    victim.outcome == PressureCheckpointOutcome::Pending && program.staged_prepares == 1,
                "pinned Device image did not refuse the second staged export as retryable Capacity");
    }
    writer.release();
    require(saving.get().state == DurableExportState::Exported, "first staged export did not finish");
    require(wait([&] { return completed == 2; }),
            "first export's retirement left the Busy staged save Pending without engine activity");
    coordinator.save_before_eviction({&victim, 1});
    {
        std::lock_guard lock(mutex);
        require(calls == 2 && reason == DurableReason::None &&
                    victim.outcome == PressureCheckpointOutcome::Settled && program.staged_prepares == 2,
                "retired staged export did not let the deferred staged save complete exactly once");
    }
    coordinator.drain();
}

// A pressure save refused as a busy transaction (here another export holds the native
// transaction) waits for each observed progress change without spending the retry budget: it is
// retried past the budget, and once the busy cause clears it saves and settles. (Replaces E89's
// characterization of the busy budget latch; Capacity keeps its budget.)
void transaction_busy_pressure_save_settles_past_budget() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.restore("second");
    core.on_worker([&] { program.serialize_exports = true; });
    BlockingWriter writer;
    auto saving = std::async(std::launch::async, [&] { return core.save(writer); });
    ReleaseWriter release{writer};
    writer.wait_until_blocked();
    std::mutex mutex;
    std::condition_variable cv;
    unsigned calls = 0, completed = 0, busy = 0;
    const auto wait = [&](const auto& predicate, auto timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, predicate);
    };
    ninfer::serve::AutomaticCheckpointCoordinator coordinator(
        [&](std::string_view session, std::optional<std::string_view> tag) {
            NullWriter output;
            const auto result = core.save(std::string(session), output);
            std::lock_guard lock(mutex);
            ++calls;
            busy += result.reason == DurableReason::TransactionBusy;
            return ninfer::serve::AutomaticCheckpointResult{std::string(*tag), result.reason};
        },
        [&] { return ninfer::serve::CheckpointSaveProgress{core.observed_progress()}; },
        [&] {
            core.save_completed();
            std::lock_guard lock(mutex);
            ++completed;
            cv.notify_all();
        });
    PressureCheckpointVictim victim{"second", "turn"};
    coordinator.save_before_eviction({&victim, 1});
    constexpr unsigned refusals = 2 * ninfer::serve::kTransientCheckpointRetries;
    unsigned reached = 0;
    for (unsigned attempt = 1; attempt <= refusals; ++attempt) {
        if (!wait([&] { return completed == attempt; }, 2s)) { break; }
        reached = attempt;
        // Observed progress ends each busy wait; the last refusal waits for the export's retirement.
        if (attempt != refusals) { core.admission_progress(); }
    }
    writer.release();
    require(saving.get().state == DurableExportState::Exported, "busy-causing export did not finish");
    const bool retried = wait([&] { return completed == refusals + 1; }, 2s);
    coordinator.save_before_eviction({&victim, 1});
    {
        std::lock_guard lock(mutex);
        require(reached == refusals && retried && calls == refusals + 1 && busy == refusals &&
                    victim.outcome == PressureCheckpointOutcome::Settled &&
                    victim.reason == DurableReason::None,
                ("busy pressure save: busy refusals=" + std::to_string(busy) + " attempts=" +
                 std::to_string(calls) + " settled=" +
                 std::to_string(victim.outcome == PressureCheckpointOutcome::Settled))
                    .c_str());
    }
    coordinator.drain();
}

// An export command that meets an in-flight context operation is held: neither refused nor
// prepared until the operation completes, then prepared once at that boundary.
void export_waits_for_operation_free_boundary() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.on_worker([&] { program.operation_in_flight = true; });
    NullWriter writer;
    auto saving = std::async(std::launch::async, [&] { return core.save(writer); });
    const bool held = saving.wait_for(300ms) == std::future_status::timeout;
    const auto during = core.on_worker([&] { return program.export_prepares; });
    core.on_worker([&] { program.operation_in_flight = false; });
    const auto result = saving.get();
    const auto prepares = core.on_worker([&] { return program.export_prepares; });
    require(held && during == 0 && result.state == DurableExportState::Exported &&
                result.reason == DurableReason::None && prepares == 1 && core.exports_in_flight() == 0,
            ("export during an in-flight operation: held=" + std::to_string(held) + " reason=" +
             std::string(durable_reason_name(result.reason)) + " prepares=" + std::to_string(prepares))
                .c_str());
}

// A restore-on-demand import that meets an in-flight context operation waits for the next
// operation-free boundary instead of being refused as a busy transaction (a refused restore
// leaves the continuation's parent response missing).
void import_waits_for_operation_free_boundary() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.on_worker([&] { program.operation_in_flight = true; });
    auto restoring = std::async(std::launch::async, [&] {
        return core.restore_captured("first", core.lookup("first"));
    });
    const bool held = restoring.wait_for(300ms) == std::future_status::timeout;
    const auto during = core.on_worker([&] { return program.import_prepares; });
    core.on_worker([&] { program.operation_in_flight = false; });
    const auto reason = restoring.get();
    const auto prepares = core.on_worker([&] { return program.import_prepares; });
    require(held && during == 0 && reason == DurableReason::None && prepares == 1,
            ("import during an in-flight operation: held=" + std::to_string(held) + " reason=" +
             std::string(durable_reason_name(reason)) + " prepares=" + std::to_string(prepares))
                .c_str());
}

// A finished export's retirement is never held: it runs while another export waits for an
// in-flight context operation, and the waiting export prepares only once the operation ends.
void retirement_runs_while_an_export_is_held() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.restore("second");
    BlockingWriter writer;
    auto first = std::async(std::launch::async, [&] { return core.save("first", writer); });
    ReleaseWriter release{writer};
    writer.wait_until_blocked();
    core.on_worker([&] { program.operation_in_flight = true; });
    NullWriter output;
    auto second = std::async(std::launch::async, [&] { return core.save("second", output); });
    const bool held = second.wait_for(300ms) == std::future_status::timeout;
    writer.release();
    const auto saved = first.get();
    const auto in_flight = core.exports_in_flight();
    const bool still_held = second.wait_for(100ms) == std::future_status::timeout;
    core.on_worker([&] { program.operation_in_flight = false; });
    const auto later = second.get();
    require(held && saved.state == DurableExportState::Exported && in_flight == 0 && still_held &&
                later.state == DurableExportState::Exported,
            ("retirement beside a held export: held=" + std::to_string(held) + " in flight after retirement=" +
             std::to_string(in_flight) + " still held=" + std::to_string(still_held))
                .c_str());
}

// An export made inline on the worker (a synchronous pressure callback does this) is never held:
// it would wait on its own thread. It keeps the native busy refusal and returns at once.
void inline_worker_export_is_never_held() {
    ExportProgram program;
    AdapterFixture<ExportContract> core(program);
    core.restore("first");
    core.on_worker([&] { program.operation_in_flight = true; });
    auto inline_save = std::async(std::launch::async, [&] {
        return core.on_worker([&] {
            NullWriter writer;
            return core.save(writer);
        });
    });
    const bool returned = inline_save.wait_for(2s) == std::future_status::ready;
    require(returned, "an inline worker export was held behind an operation on its own thread");
    const auto result = inline_save.get();
    require(result.state != DurableExportState::Exported && result.reason == DurableReason::TransactionBusy &&
                core.exports_in_flight() == 0,
            "an inline worker export during an operation did not keep the native busy refusal");
}
}

int main() {
    const std::pair<const char*, void (*)()> cases[] = {
        {"paused_request_settlement", paused_request_settles_by_native_replay},
        {"export_success", [] { isolated_import_waits_for_export(false); }},
        {"export_writer_exception", [] { isolated_import_waits_for_export(true); }},
        {"export_retirement_allocation_failure",
         [] { export_retirement_survives_allocation_failure(false); }},
        {"export_writer_exception_allocation_failure",
         [] { export_retirement_survives_allocation_failure(true); }},
        {"inline_export_retirement_allocation_failure",
         [] { export_retirement_survives_allocation_failure(false, true); }},
        {"inline_export_writer_exception_allocation_failure",
         [] { export_retirement_survives_allocation_failure(true, true); }},
        {"export_command_allocation_failure", export_command_allocation_failures_precede_native_lease},
        {"overlapping_exports", overlapping_exports_retain_progress_until_last_release},
        {"automatic_retry_after_export_retirement", automatic_retry_after_other_export_retires},
        {"unchanged_reclaim_refusal", unchanged_reclaim_refusal_does_not_notify},
        {"pressure_callback_export", pressure_callback_export_does_not_reclaim},
        {"genuine_capacity", genuine_capacity_and_unprepared_export_do_not_defer},
        {"unseen_session_gates", unseen_sessions_leave_bounded_gates},
        {"evicted_session_gate", evicted_session_gate_retires_and_resident_one_stays},
        {"restore_gate_pin", restore_lookup_pins_its_gate_through_sweeps},
        {"e89_staged_export_after_host_route", staged_export_after_host_route_exhausted},
        {"e89_host_route_first_never_stages", host_route_first_never_stages},
        {"e89_staged_writer_failure_retires_pins", staged_pins_retire_on_writer_failure},
        {"e89_staged_busy_retries_after_retirement", staged_busy_export_retries_after_first_retires},
        {"esave_transaction_busy_pressure_save_settles", transaction_busy_pressure_save_settles_past_budget},
        {"esave_export_waits_for_operation_free_boundary", export_waits_for_operation_free_boundary},
        {"esave_import_waits_for_operation_free_boundary", import_waits_for_operation_free_boundary},
        {"esave_retirement_runs_while_an_export_is_held", retirement_runs_while_an_export_is_held},
        {"esave_inline_worker_export_is_never_held", inline_worker_export_is_never_held},
    };
    unsigned failures = 0;
    for (const auto& [name, run] : cases) {
        try {
            run();
            std::cout << "ok: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    return failures ? 1 : 0;
}
