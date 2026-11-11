// Real service/store orchestration with a scripted Engine adapter; native byte proof is tested
// separately by EngineCore. No model, CUDA work, or network service is started here.
#include "ninfer/engine.h"
#include "ninfer/build_info.h"
#include "serve/session_checkpoint_service.h"
#include "runtime/contract/continuation_checkpoint.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <chrono>
#include <future>
#include <cstdlib>
#include <new>
#include <utility>

namespace {
thread_local std::size_t allocation_failure_size = 0;
thread_local unsigned allocation_failures = 0;
}

void* operator new(std::size_t size) {
    if (allocation_failure_size && size == allocation_failure_size) {
        allocation_failure_size = 0;
        ++allocation_failures;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

using namespace ninfer;
using namespace ninfer::runtime;
using namespace ninfer::serve;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Script {
    std::function<DurableExportResult(std::string_view, ContinuationCheckpointWriter&, DurableSaveIntent)> export_now;
    std::function<void()> publishing;
    bool stale = false;
    std::map<std::string, unsigned> calls;
    std::vector<DurableSaveIntent> intents;
    DurableExportReclaim* summary = nullptr;
} script;
struct ReadQueue final : ContinuationCheckpointReadQueue {
    struct Done final : ContinuationCheckpointReadCompletion { void wait() override {} };
    bool available() const noexcept override { return true; }
    bool overlaps_batches() const noexcept override { return false; }
    std::string_view backend_name() const noexcept override { return "service-fixture"; }
    std::string_view unavailable_reason() const noexcept override { return {}; }
    std::unique_ptr<ContinuationCheckpointReadCompletion> submit(
        const std::filesystem::path& path, std::span<const ContinuationCheckpointReadRequest> requests) override {
        std::ifstream input(path, std::ios::binary);
        for (const auto& request : requests) {
            input.seekg(request.file_offset);
            input.read(reinterpret_cast<char*>(request.destination.data()), request.destination.size());
            require(bool(input), "fixture payload read failed");
        }
        return std::make_unique<Done>();
    }
};
DurableExportReclaim summary() {
    return {.considered = true, .prepare_attempts = 2, .reclaimed_owners = 1,
            .reclaim_declined = 3, .reclaimed_cache_steps = 4, .reclaimed_host_bytes = 256};
}
DurableExportResult exported(ContinuationCheckpointWriter& writer) {
    std::array<std::byte, 256> state{};
    std::array<std::byte, 1> index{};
    require(writer.write_file("engine/state/0.bin", 0, state.size(), state, 256) &&
            writer.write_file("engine/index.cbor", 0, index.size(), index, 1) && writer.finish(),
            "fixture export write failed");
    return {.state = DurableExportState::Exported,
            .stats = {.frontier_tokens = 64, .restored_tokens = 64, .payload_bytes = 257,
                      .points = 1, .states = 1}, .reclaim = summary()};
}
StoredOpenAIResponse response(const std::string& session, std::string id, unsigned order) {
    StoredOpenAIResponse out;
    out.id = std::move(id);
    out.session_key = "http:" + session;
    out.client_session_sha256 = session;
    out.publication_order = order;
    out.response = {{"id", out.id}, {"object", "response"}, {"status", "completed"}};
    return out;
}
struct ReceiptLog : std::stringbuf {
    std::size_t fail_after_export = 0;
    std::streamsize xsputn(const char* text, std::streamsize size) override {
        const auto written = std::stringbuf::xsputn(text, size);
        if (fail_after_export &&
            std::string_view(text, size).find("\"state\":\"exported\"") != std::string_view::npos) {
            // Target the next save's session-key copy, outside save_session's inner
            // catch, without failing syncstream buffer teardown after this receipt.
            allocation_failure_size = std::exchange(fail_after_export, 0);
        }
        return written;
    }
};
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("checkpoint-export-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Engine engine{EngineOptions{}};
    ServeOptions options;
    OpenAIResponsesStore responses{16, 1ULL << 20};
    std::unique_ptr<SessionCheckpointService> service;
    ReceiptLog logs;
    std::streambuf* previous = std::clog.rdbuf(&logs);
    explicit Fixture(std::uint64_t quota = 1ULL << 24) {
        script = {};
        options.api_key = std::string(32, 'k');
        options.binary_sha256 = std::string(64, 'b');
        options.artifact_sha256 = std::string(64, 'a');
        options.deployment_profile = "service-export-test";
        options.session_checkpoint_root = root / "checkpoints";
        options.session_checkpoint_quota_bytes = quota;
        options.shutdown_report = (root / "shutdown.json").string();
        service = std::make_unique<SessionCheckpointService>(engine, options, responses, std::nullopt);
    }
    ~Fixture() {
        allocation_failure_size = 0;
        service.reset();
        std::clog.rdbuf(previous);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    nlohmann::json report() {
        std::ifstream input(root / "shutdown.json");
        return nlohmann::json::parse(input);
    }
    std::vector<nlohmann::json> receipts() {
        std::vector<nlohmann::json> out;
        std::istringstream input(logs.str());
        for (std::string line; std::getline(input, line);) {
            const auto value = nlohmann::json::parse(line, nullptr, false);
            if (value.is_object() && value.value("operation", "") == "save") out.push_back(value);
        }
        return out;
    }
};
void has_summary(const nlohmann::json& value) {
    require(value.at("prepare_attempts") == 2 && value.at("reclaimed_owners") == 1 &&
            value.at("reclaim_declined") == 3 && value.at("reclaimed_cache_steps") == 4 &&
            value.at("reclaimed_host_bytes") == 256, "service discarded reclaim summary");
}
void final_second_pass() {
    for (unsigned mode = 0; mode != 4; ++mode) {
        Fixture f;
        const std::string a(64, 'a'), b(64, 'b');
        f.responses.put(response(a, "resp_a", 1));
        f.responses.put(response(b, "resp_b", 2));
        const auto order = f.responses.session_digests();
        bool published = false;
        script.publishing = [&] { published = true; };
        script.export_now = [&](std::string_view session, auto& writer, DurableSaveIntent intent) {
            require(intent == DurableSaveIntent::Final, "final save lost Tier-2 intent");
            const bool first = session == order.front();
            if ((first && (!published || mode == 2)) || mode == 1 || (first && mode == 3))
                return DurableExportResult{.reason = mode == 3 ? DurableReason::IoUnavailable : DurableReason::Capacity,
                                           .reclaim = summary()};
            return exported(writer);
        };
        require(f.service->final_save() == (mode == 0), "final-save result weakened fail-closed stop");
        const auto report = f.report();
        require(report.at("saved") == (mode == 0 ? 2 : mode == 1 ? 0 : 1) &&
                report.at("refused") == (mode == 0 ? 0 : mode == 1 ? 2 : 1) && report.at("skipped") == 0,
                "second pass double-counted session outcomes");
        require(script.calls[order.front()] == ((mode == 0 || mode == 2) ? 2 : 1) &&
                script.calls[order.back()] == 1, "unbounded or non-publication-backed final-save retry");
        for (const auto& receipt : f.receipts()) has_summary(receipt);
    }
}
void final_second_pass_exception_counts_once() {
    Fixture f;
    const std::string a(64, 'a'), b(64, 'b');
    f.responses.put(response(a, "resp_a", 1));
    f.responses.put(response(b, "resp_b", 2));
    const auto order = f.responses.session_digests();
    script.export_now = [&](std::string_view session, auto& writer, DurableSaveIntent intent) {
        require(intent == DurableSaveIntent::Final, "second-pass exception lost final intent");
        if (session == order.front())
            return DurableExportResult{.reason = DurableReason::Capacity, .reclaim = summary()};
        return exported(writer);
    };
    const auto failures = allocation_failures;
    f.logs.fail_after_export = a.size() + 1;
    require(!f.service->final_save(), "second-pass exception weakened fail-closed stop");
    require(allocation_failures == failures + 1 && !f.logs.fail_after_export &&
                !allocation_failure_size && script.calls[order.front()] == 1 &&
                script.calls[order.back()] == 1,
            "exception did not escape second-pass save_session before export");
    const auto report = f.report();
    require(report.at("saved") == 1 && report.at("refused") == 1 &&
                report.at("skipped") == 0 && report.at("complete") == false,
            "second-pass save_session exception counted the refusal twice");
}
void receipts_and_survivors() {
    for (unsigned mode = 0; mode != 6; ++mode) {
        Fixture f(mode == 5 ? 1 : 1ULL << 24);
        const std::string session(64, 'c');
        f.responses.put(response(session, "resp_old", 1));
        f.responses.put(response(session, "resp_current", 2));
        script.stale = mode == 1;
        script.export_now = [&](auto, auto& writer, DurableSaveIntent intent) {
            require(intent == (mode >= 2 ? DurableSaveIntent::Survivor : DurableSaveIntent::Explicit),
                    "service gave survivor export durable-proof authority");
            if (mode == 3) return DurableExportResult{.reason = DurableReason::Capacity, .reclaim = summary()};
            if (mode == 4) return DurableExportResult{.reason = DurableReason::TransactionBusy, .reclaim = summary()};
            return exported(writer);
        };
        if (mode == 3 || mode == 5) {
            bool refused = false;
            try { (void)f.service->erase_response("resp_old", session); }
            catch (const std::exception&) { refused = true; }
            require(refused && f.responses.get_for_session("resp_old", session),
                    "native/store refusal deleted an unacknowledged response");
        }
        else if (mode >= 2) require(f.service->erase_response("resp_old", session), "survivor export/fallback failed");
        else {
            const auto saved = f.service->save(session);
            has_summary(saved);
            require(saved.at("committed") == (mode == 0), "reclaim fields fabricated publication acknowledgement");
        }
        const auto receipts = f.receipts();
        require(!receipts.empty(), "save receipt absent");
        has_summary(receipts.back());
        require(receipts.back().at("committed") == (mode != 1 && mode != 3 && mode != 5),
                "survivor receipt changed committed semantics");
        if (mode == 3 || mode == 5)
            require(receipts.back().at("reason") == (mode == 3 ? "capacity" : "quota"),
                    "survivor receipt replaced its native/store refusal reason");
    }
}

void automatic_receipt() {
    Fixture f;
    const std::string session(64, 'd');
    f.responses.put(response(session, "resp_auto", 1));
    std::promise<void> entered;
    auto ready = entered.get_future();
    script.export_now = [&](auto, auto& writer, DurableSaveIntent intent) {
        require(intent == DurableSaveIntent::Automatic, "automatic save gained proof authority");
        auto result = exported(writer);
        entered.set_value();
        return result;
    };
    f.service->response_stored(session, UINT32_MAX);
    require(ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "automatic save never ran");
    require(f.service->final_save(), "automatic save failed to cover final endpoint");
    require(script.intents.size() == 1, "covered automatic endpoint was saved again");
    has_summary(f.receipts().front());
}

void service_proof_pin_lifetime() {
    const std::string victim(64, 'a'), target(64, 'b');
    std::uint64_t generation_bytes = 0;
    {
        Fixture measure;
        measure.responses.put(response(victim, "resp_a", 1));
        require(measure.service->save(victim).at("committed") == true, "pin fixture did not publish");
        for (const auto& file : std::filesystem::recursive_directory_iterator(
                 measure.root / "checkpoints" / "sessions" / victim / "generations"))
            if (file.is_regular_file()) generation_bytes += file.file_size();
    }
    for (bool tight : {false, true}) {
        Fixture f(tight ? generation_bytes : 1ULL << 24);
        f.responses.put(response(victim, "resp_a", 1));
        f.responses.put(response(target, "resp_b", 2));
        require(f.service->save(victim).at("committed") == true, "victim baseline missing");
        std::weak_ptr<const ContinuationCheckpointReader> lifetime;
        script.export_now = [&](auto, auto& writer, auto) {
            auto pin = f.service->pin_recoverable(victim, "resp_a");
            require(bool(pin), "export callback could not pin victim generation");
            lifetime = pin;
            auto result = exported(writer);
            result.proof_pins.push_back(std::move(pin));
            return result;
        };
        script.publishing = [&] {
            require(!lifetime.expired() && f.service->status(victim).at("state") == "available",
                    "service dropped proof before pointer publication");
        };
        const auto result = f.service->save(target);
        require(result.at("committed") == !tight && lifetime.expired() &&
                    f.service->status(victim).at("state") == "available",
                "service proof pins did not cover full save or outlived it");
        has_summary(result);
        if (tight) {
            require(result.at("reason") == "quota", "pinned victim did not fail quota closed");
            script.export_now = {};
            script.publishing = {};
            require(f.service->save(target).at("committed") == true &&
                        f.service->status(victim).at("state") == "missing",
                    "later independent save could not apply ordinary quota policy");
        }
    }
}

// E89 / #84: the service's summary sink keeps the reclaim and staging summary for the refusal
// receipt when the native write throws; the exception is not turned into a native refusal.
void write_failure_keeps_reclaim_receipt() {
    Fixture f;
    const std::string session(64, 'e');
    f.responses.put(response(session, "resp_e", 1));
    script.export_now = [&](auto, auto&, DurableSaveIntent) -> DurableExportResult {
        require(script.summary != nullptr, "service passed no reclaim summary sink");
        *script.summary = summary();
        script.summary->device_staging_attempted = true;
        script.summary->device_staged_states = 2;
        script.summary->host_shortage = DurableHostShortage::HostCapacity;
        throw std::runtime_error("injected native write failure after reclaim");
    };
    const auto saved = f.service->save(session);
    require(saved.at("committed") == false && saved.at("reason") == "io_unavailable",
            "throwing native write changed its refusal reason or acknowledged a save");
    has_summary(saved);
    require(saved.at("device_staging_attempted") == true && saved.at("device_staged_states") == 2 &&
                saved.at("host_shortage") == "host_capacity",
            "throwing native write lost the staging summary");
    const auto receipts = f.receipts();
    require(!receipts.empty(), "write failure left no save receipt");
    has_summary(receipts.back());
}

// E89: a refused save names its native Capacity site and Host shortage; a Device-staged save
// reports its staged State images and gate hold. Existing receipt keys are unchanged.
void capacity_site_and_staging_receipts() {
    for (const bool staged : {false, true}) {
        Fixture f;
        // Session digests must be lowercase SHA-256 hex.
        const char digit = staged ? '9' : 'f';
        const std::string session(64, digit);
        f.responses.put(response(session, "resp_" + std::string(1, digit), 1));
        script.export_now = [&](auto, auto& writer, DurableSaveIntent) {
            auto reclaim = summary();
            reclaim.host_shortage = DurableHostShortage::HostCapacity;
            reclaim.device_staging_attempted = true;
            if (!staged) {
                reclaim.capacity_site = DurableCapacitySite::HostPreparation;
                return DurableExportResult{.reason = DurableReason::Capacity, .reclaim = reclaim};
            }
            auto result = exported(writer);
            reclaim.device_staged_states = 3;
            reclaim.device_staged_hold_ms = 7;
            result.reclaim = reclaim;
            return result;
        };
        const auto saved = f.service->save(session);
        has_summary(saved);
        require(saved.at("host_shortage") == "host_capacity" && saved.at("device_staging_attempted") == true,
                "save receipt dropped its Host shortage or staging attempt");
        if (staged) {
            require(saved.at("committed") == true && saved.at("device_staged_states") == 3 &&
                        saved.at("device_staged_hold_ms") == 7 && !saved.contains("capacity_site"),
                    "Device-staged save receipt is incomplete");
        } else {
            require(saved.at("committed") == false && saved.at("reason") == "capacity" &&
                        saved.at("capacity_site") == "host_preparation" &&
                        !saved.contains("device_staged_states"),
                    "refused save receipt did not name its Capacity site");
        }
    }
}
}

// Link-time Engine boundary; the test links the real service and store, never ninfer_engine.
namespace ninfer {
class Engine::Impl { public: EngineOptions options; };
Engine::Engine(EngineOptions options) : impl_(std::make_shared<Impl>()) {
    impl_->options = std::move(options);
    impl_->options.context_cache.enabled = true;
    impl_->options.max_context = 4096;
    impl_->options.max_concurrency = 1;
}
Engine::~Engine() = default;
const EngineOptions& Engine::options() const { return impl_->options; }
LoadSummary Engine::load_summary() const { LoadSummary value; value.architecture = "qwen3_5"; value.model_name = "fixture"; return value; }
MemorySummary Engine::memory_summary() const { MemorySummary value; value.kv_capacity = 4096; value.prefill_chunk = 64; return value; }
BuildInfo build_info() noexcept {
    return {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            "host-test", "Release", "test-cxx", "test-cuda", "13.0", "120a", false};
}
}
namespace ninfer::runtime {
DurableNativeLayout CheckpointEngineAccess::checkpoint_layout(const Engine&) {
    return {"qwen3_5/1", std::string(64, 'c'), 256,
            {{0, "main", std::string(64, 'd'), "bf16", 1, 64, 1, 8, 1024, 1024}}};
}
DurableFrontendIdentity CheckpointEngineAccess::frontend_identity(const Engine&) {
    return {std::string(64, 'a'), std::string(64, 'b'), std::string(64, 'c'), "disabled", "test/1", "test/1"};
}
std::shared_ptr<ContinuationCheckpointReadQueue> CheckpointEngineAccess::make_read_queue(Engine&, const std::filesystem::path&) {
    return std::make_shared<ReadQueue>();
}
std::uint64_t CheckpointEngineAccess::checkpoint_progress(const Engine&) noexcept { return 0; }
void CheckpointEngineAccess::notify_checkpoint_progress(Engine&) noexcept {}
void CheckpointEngineAccess::observe_restore(Engine&, const RuntimeCheckpointRestoreStats&) noexcept {
    std::terminate(); // This export-only fixture must not restore.
}
void CheckpointEngineAccess::response_stored(Engine&, std::string_view, std::string_view) noexcept {}
DurableSessionEpoch CheckpointEngineAccess::session_epoch(Engine&, std::string_view) { return {}; }
DurableExportResult CheckpointEngineAccess::export_session(Engine&, std::string_view session, std::string_view,
    ContinuationCheckpointWriter& writer, const DurableLimits&, DurableSaveIntent intent,
    DurableExportReclaim* summary) {
    ++script.calls[std::string(session)];
    script.intents.push_back(intent);
    script.summary = summary;
    return script.export_now ? script.export_now(session, writer, intent) : exported(writer);
}
DurablePublicationResult CheckpointEngineAccess::publish_export(Engine&, std::string_view, std::string_view,
    DurableSessionEpoch, const std::function<bool()>& commit) {
    if (script.stale) return {.state = DurableExportState::Stale, .reason = DurableReason::StalePublication};
    const bool saved = commit();
    if (saved && script.publishing) script.publishing();
    return {.state = saved ? DurableExportState::Exported : DurableExportState::Refused};
}
DurablePrepareResult CheckpointEngineAccess::prepare_import(Engine&, const DurableImportTarget&,
    const ContinuationCheckpointReader&, const RuntimeFingerprintV2&, const DurableCheckpointStats&, const DurableLimits&) {
    throw std::logic_error("service export fixture must not import");
}
struct DurableEngineImport::Impl {};
DurableEngineImport::~DurableEngineImport() = default;
DurableImportResult DurableEngineImport::commit() noexcept { std::terminate(); }
void DurableEngineImport::abort() noexcept { std::terminate(); }
}
namespace ninfer::serve {
std::string resolve_public_model_id(const ServeOptions& options, std::string_view artifact_model_name) {
    return options.model_id_override.value_or(std::string(artifact_model_name));
}
}
int main() {
    try {
        final_second_pass();
        receipts_and_survivors();
        automatic_receipt();
        service_proof_pin_lifetime();
        final_second_pass_exception_counts_once();
        write_failure_keeps_reclaim_receipt();
        capacity_site_and_staging_receipts();
        std::cout << "PASS service export lifecycle\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
