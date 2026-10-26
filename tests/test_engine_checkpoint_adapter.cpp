#include "runtime/engine/checkpoint_control.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
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
    struct Scheduler {};
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
    struct Image {
        bool device = false;
        unsigned readers = 0;
        bool reclaimable = true;
    };
    std::vector<Image> images;
    bool refuse_export = false;
    static constexpr unsigned device_capacity = 3;

    struct Reservation {
        std::size_t count;
        DurablePrepareState state() const { return DurablePrepareState::Prepared; }
        DurableReason reason() const { return DurableReason::None; }
        DurableCheckpointStats stats() const { return checkpoint_stats(); }
    };

    struct Lease {
        ExportProgram* owner = nullptr;
        std::vector<int> handles;
        explicit Lease(ExportProgram& program, std::span<const int> points) {
            if (program.refuse_export) { return; }
            handles.assign(points.begin(), points.end());
            owner = &program;
            for (const int point : handles) { ++owner->images.at(point).readers; }
        }
        Lease(Lease&& other) noexcept
            : owner(std::exchange(other.owner, nullptr)), handles(std::move(other.handles)) {}
        ~Lease() {
            if (owner) {
                for (const int point : handles) { --owner->images.at(point).readers; }
            }
        }
        DurablePrepareState state() const {
            return owner ? DurablePrepareState::Prepared : DurablePrepareState::CapacityRefused;
        }
        DurableReason reason() const { return owner ? DurableReason::None : DurableReason::Capacity; }
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
        return {set.points.size()};
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
    Lease prepare_checkpoint_export(std::span<const int> points, const DurableLimits&) {
        return Lease(*this, points);
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
};

struct ExportContract : Contract {
    using Program = ExportProgram;
    struct Instance { Program* program = nullptr; };
    using Resources = ExportResources;
    struct Scheduler {
        unsigned capacity_events = 0;
        void capacity_released() { ++capacity_events; }
    };
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

template <class ModelContract>
class AdapterFixture {
    using Checkpoint = int;
    using Request = typename ModelContract::Request;
    typename ModelContract::Instance instance_;
    typename ModelContract::Resources resources_;
    typename ModelContract::Scheduler scheduler_;
    CheckpointControl checkpoints_;
    std::mutex checkpoint_provider_mutex_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::function<void()>> checkpoint_commands_;
    std::thread worker_;
    std::unique_lock<std::mutex>* checkpoint_execution_lock_ = nullptr;
    std::size_t checkpoint_exports_in_flight_ = 0;
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
#include "runtime/engine/engine_checkpoint.inl"
public:
    AdapterFixture() = default;
    explicit AdapterFixture(typename ModelContract::Program& program) : instance_{&program} {
        worker_ = std::thread([this] {
            for (;;) {
                {
                    std::unique_lock lock(queue_mutex_);
                    queue_cv_.wait(lock, [&] { return stopping_ || !checkpoint_commands_.empty(); });
                    if (stopping_) { return; }
                }
                std::mutex execution;
                std::unique_lock lock(execution);
                checkpoint_execution_lock_ = &lock;
                run_checkpoint_commands();
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
    void restore(std::string session) {
        IndexReader reader;
        DurableImportTarget target{.session_sha256 = session, .expected_tag = "turn",
            .expected_epoch = session_epoch(session), .allow_prefix_reuse = true,
            .cache_participation = true};
        auto prepared = prepare_import(target, reader, {}, checkpoint_stats(), {});
        require(prepared.state == DurablePrepareState::Prepared && prepared.commit,
                "actual adapter did not prepare the Host-only import");
        require(prepared.commit().state == DurableImportState::Imported,
                "actual adapter did not publish the Host-only import");
    }
    void complete_first_resume() {
        on_worker([&] { instance_.program->complete_first_resume(resources_.owners.at("first").points); });
    }
    DurableExportResult save(ContinuationCheckpointWriter& writer) {
        return export_session("first", "turn", writer, {});
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
    std::pair<unsigned, unsigned> progress() {
        return on_worker([&] {
            return std::pair(scheduler_.capacity_events, admission_notifications_);
        });
    }
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
            "export retirement did not publish one capacity/admission event");
    require(!core.capacity_pending() && core.on_worker([&] { return program.pinned_device_slots() == 0; }),
            "completed export still owns a native reader or pending-capacity claim");
    require(core.isolated_admission() && core.on_worker([&] { return program.images.size() == 8; }),
            "second session could not admit by lossless Host demotion after export retirement");
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
    require(core.capacity_pending() && !core.isolated_admission(),
            "first export retirement forgot the other export's native pins");
    second.release();
    require(saving_second.get().state == DurableExportState::Exported, "second overlapping export failed");
    const auto after = core.progress();
    require(after.first == before.first + 2 && after.second == before.second + 2 &&
                !core.capacity_pending() && core.isolated_admission(),
            "last export retirement failed to unblock lossless admission");
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
}

int main() {
    const std::pair<const char*, void (*)()> cases[] = {
        {"paused_request_settlement", paused_request_settles_by_native_replay},
        {"export_success", [] { isolated_import_waits_for_export(false); }},
        {"export_writer_exception", [] { isolated_import_waits_for_export(true); }},
        {"overlapping_exports", overlapping_exports_retain_progress_until_last_release},
        {"genuine_capacity", genuine_capacity_and_unprepared_export_do_not_defer},
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
