#include "serve/session_checkpoint_service.h"

#include "core/io/durable_file.h"
#include "core/sha256.h"
#include "ninfer/build_info.h"
#include "ninfer/engine.h"
#include "product/speculative_options.h"
#include "serve/client_identity.h"
#include "serve/opaque_id.h"
#include "serve/server_identity.h"

#include <cstdio>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <syncstream>
#include <utility>

namespace ninfer::serve {
namespace {
using Access = runtime::CheckpointEngineAccess;
using Reason = runtime::DurableReason;
using ImportState = runtime::DurableImportState;
using Json = nlohmann::json;

const char* kv_name(KvCacheStorage storage) {
    switch (storage) {
    case KvCacheStorage::BFloat16: return "bf16";
    case KvCacheStorage::Int8Group64: return "int8";
    case KvCacheStorage::Fp8E4M3Row256: return "fp8";
    case KvCacheStorage::Fp8KeyNvfp4Value: return "k8v4";
    case KvCacheStorage::Nvfp4Group16: break;
    }
    throw std::invalid_argument("unsupported durable KV storage");
}

runtime::RuntimeFingerprintV2 startup_fingerprint(Engine& engine, const ServeOptions& options) {
    const auto build = build_info();
    const auto load = engine.load_summary();
    const auto memory = engine.memory_summary();
    const auto& effective = engine.options();
    runtime::RuntimeFingerprintV2 value;
#if defined(_WIN32)
    constexpr const char* platform = "windows";
#elif defined(__linux__)
    constexpr const char* platform = "linux";
#else
    throw std::invalid_argument("unsupported durable platform");
    constexpr const char* platform = "unsupported";
#endif
    value.build = {.upstream_base_sha = std::string(build.upstream_base_sha),
        .patch_stack_sha = std::string(build.patch_stack_sha), .source_dirty = build.source_dirty,
        .build_profile = std::string(build.build_profile), .build_type = std::string(build.build_type),
        .binary_sha256 = options.binary_sha256, .platform = platform,
        .cxx_compiler = std::string(build.cxx_compiler), .cuda_compiler = std::string(build.cuda_compiler),
        .cuda_toolkit = std::string(build.cuda_toolkit), .cuda_architecture = std::string(build.cuda_architecture)};
    value.model = {.artifact_sha256 = options.artifact_sha256, .architecture = load.architecture,
        .model_id = resolve_public_model_id(options, load.model_name), .weights_id = options.artifact_sha256};
    value.frontend = Access::frontend_identity(engine);
    value.native_layout = Access::checkpoint_layout(engine);
    value.engine = {.max_context = effective.max_context, .kv_capacity = memory.kv_capacity,
        .kv_dtype = kv_name(effective.kv_cache), .max_concurrency = effective.max_concurrency,
        .prefill_chunk = memory.prefill_chunk,
        .speculative_backend = product::speculative_backend_name(effective.speculative.backend),
        .draft_window = effective.speculative.draft_tokens,
        .proposal_head = effective.speculative.proposal_head == ProposalHead::Optimized ? "optimized" : "full",
        .enable_vision = effective.enable_vision, .use_cuda_graph = effective.use_cuda_graph};
    value.responses.codec = kResponseStoreSnapshotCodec;
    value.deployment.profile = options.deployment_profile;
    value.deployment.effective_config_sha256 = session_checkpoint_effective_config_sha256(value);
    // This validates every authority before the store can create/discover a generation.
    (void)session_checkpoint_runtime_fingerprint(value);
    return value;
}

SessionCheckpointStoreOptions store_options(Engine& engine, const ServeOptions& options) {
    if (options.api_key.empty()) { throw std::invalid_argument("durable serving requires authentication"); }
    if (options.session_checkpoint_require_origin_auth && options.api_key.size() < 32) {
        throw std::invalid_argument("strict checkpoint origin authentication requires a 32-byte API key");
    }
    constexpr std::string_view domain = "ninfer-checkpoint-manifest-origin-v1";
    const auto key = crypto::hmac_sha256(std::as_bytes(std::span(options.api_key.data(), options.api_key.size())),
                                        std::as_bytes(std::span(domain.data(), domain.size())));
    return {.root = options.session_checkpoint_root,
        .disk_quota_bytes = options.session_checkpoint_quota_bytes,
        .short_session_tokens = options.session_checkpoint_min_tokens,
        .staging_bytes = options.session_checkpoint_staging_bytes,
        .write_buffer_bytes = options.session_checkpoint_write_buffer_bytes,
        .origin_mac_key = std::string(reinterpret_cast<const char*>(key.data()), key.size()),
        .require_origin_auth = options.session_checkpoint_require_origin_auth,
        .read_queue = Access::make_read_queue(engine, options.session_checkpoint_root)};
}

const char* import_name(ImportState state) noexcept {
    switch (state) {
    case ImportState::Imported: return "imported";
    case ImportState::CapacityRefused: return "capacity_refused";
    case ImportState::Corrupt: return "corrupt";
    case ImportState::IdentityMismatch: return "identity_mismatch";
    case ImportState::PolicyDisabled: return "policy_disabled";
    case ImportState::Unavailable: return "unavailable";
    }
    return "policy_disabled";
}

Json operation(std::string_view kind, std::string_view state, Reason reason, bool committed,
               const runtime::DurableCheckpointStats& stats = {}, std::string_view origin = "not_checked") {
    return {{"schema", "ninfer-durable-operation/1"}, {"operation", kind}, {"state", state},
        {"reason", runtime::durable_reason_name(reason)}, {"committed", committed}, {"origin", origin},
        {"frontier_tokens", stats.frontier_tokens}, {"restored_tokens", stats.restored_tokens},
        {"payload_bytes", stats.payload_bytes}, {"points", stats.points},
        {"states", stats.states}, {"pages", stats.pages}};
}

runtime::DurableImportResult prepared_refusal(const runtime::DurablePrepareResult& prepared) {
    switch (prepared.state) {
    case runtime::DurablePrepareState::CapacityRefused:
        return {.state = ImportState::CapacityRefused, .reason = prepared.reason};
    case runtime::DurablePrepareState::Corrupt:
        return {.state = ImportState::Corrupt, .reason = prepared.reason};
    case runtime::DurablePrepareState::IdentityMismatch:
        return {.state = ImportState::IdentityMismatch, .reason = prepared.reason};
    case runtime::DurablePrepareState::PolicyDisabled:
        return {.state = ImportState::PolicyDisabled, .reason = prepared.reason};
    case runtime::DurablePrepareState::Unavailable:
        return {.state = ImportState::Unavailable, .reason = prepared.reason};
    case runtime::DurablePrepareState::Prepared:
        // A missing provider transaction is not evidence against authenticated disk bytes.
        return {.state = ImportState::Unavailable, .reason = Reason::IoUnavailable};
    }
    return {.state = ImportState::Unavailable, .reason = Reason::IoUnavailable};
}
} // namespace

SessionCheckpointService::SessionCheckpointService(Engine& engine, const ServeOptions& options,
                                                   OpenAIResponsesStore& responses,
                                                   std::optional<std::string> principal_scope)
    : engine_(engine), responses_(responses), options_(options),
      principal_scope_(std::move(principal_scope)),
      fingerprint_(startup_fingerprint(engine, options)),
      fingerprint_json_(session_checkpoint_runtime_fingerprint(fingerprint_)),
      limits_{.staging_bytes = options.session_checkpoint_staging_bytes,
               .write_buffer_bytes = options.session_checkpoint_write_buffer_bytes},
      store_(store_options(engine, options)),
      automatic_([this](std::string_view session) { automatic_save(session); }) {}

SessionCheckpointService::~SessionCheckpointService() { automatic_.drain(); }

void SessionCheckpointService::receipt(const Json& value) const noexcept {
    try { std::osyncstream(std::clog) << value.dump() << '\n'; } catch (...) {}
}

runtime::DurableImportResult SessionCheckpointService::restore(
    std::string_view session, std::optional<std::string_view> required_response) {
    const auto finish = [&](runtime::DurableImportResult result, std::string_view origin = "not_checked") {
        receipt(operation("restore", import_name(result.state), result.reason,
                          result.state == ImportState::Imported && result.reason == Reason::None,
                          result.stats, origin));
        return result;
    };
    if (!options_.allow_prefix_reuse || !engine_.options().context_cache.enabled ||
        stopping_.load(std::memory_order_acquire)) {
        return finish({.state = ImportState::PolicyDisabled, .reason = Reason::CacheDisabled});
    }
    // Both guards precede any disk I/O. Disk data supplies neither scope nor epoch authority.
    auto guard = responses_.begin_restore(std::string(session));
    runtime::DurableImportTarget target{.session_sha256 = std::string(session),
        .expected_epoch = Access::session_epoch(engine_, session),
        .shared_reuse_scope = principal_scope_,
        .allow_prefix_reuse = options_.allow_prefix_reuse, .cache_participation = true};
    auto loaded = store_.load(session, fingerprint_json_, required_response);
    if (loaded.state != SessionCheckpointLoadState::Available || !loaded.checkpoint) {
        const auto state = loaded.state == SessionCheckpointLoadState::Unavailable ? ImportState::Unavailable
            : loaded.state == SessionCheckpointLoadState::Corrupt ? ImportState::Corrupt
            : loaded.reason == Reason::OriginRequired ? ImportState::PolicyDisabled
            : ImportState::IdentityMismatch;
        return finish({.state = state, .reason = loaded.reason});
    }
    auto& verified = *loaded.checkpoint;
    target.expected_tag = verified.responses.latest_response_id;
    const std::string_view origin = verified.origin_authenticated ? "authenticated" : "unauthenticated";
    auto prepared = [&]() -> runtime::DurablePrepareResult {
        try {
            return Access::prepare_import(engine_, target, *verified.engine, fingerprint_,
                                           verified.expected_engine, limits_);
        } catch (const std::bad_alloc&) {
            return {.state = runtime::DurablePrepareState::CapacityRefused, .reason = Reason::Capacity};
        } catch (...) {
            return {.state = runtime::DurablePrepareState::Unavailable, .reason = Reason::IoUnavailable};
        }
    }();
    runtime::DurableImportResult result;
    if (prepared.state != runtime::DurablePrepareState::Prepared || !prepared.prepared) {
        result = prepared_refusal(prepared);
    } else {
        result = commit_openai_response_restore(responses_, std::move(verified.responses), guard,
                                                [&] { return prepared.prepared->commit(); });
        if (result.state != ImportState::Imported) { prepared.prepared->abort(); }
    }
    if (result.state == ImportState::Corrupt) {
        (void)store_.quarantine(session, verified.generation, result.reason);
    }
    return finish(result, origin);
}

SessionCheckpointService::SaveOutcome SessionCheckpointService::save_session(
    std::string_view session, std::optional<std::string_view> expected_tag, bool allow_covered) {
    // A Response DELETE owns the same session operation through disk invalidation and RAM
    // deletion. No pre-delete snapshot may enter save() after a successful deletion.
    auto operation_mutex = session_operation(session);
    std::lock_guard operation_lock(*operation_mutex);
    SaveOutcome outcome;
    auto snapshot = responses_.snapshot_session(session);
    if (!snapshot) { outcome.skip.durable_reason = Reason::NoSessionRecords; return outcome; }
    if (expected_tag && snapshot->latest_response_id != *expected_tag) {
        outcome.skip.durable_reason = Reason::TagMismatch;
        outcome.skip.export_state = runtime::DurableExportState::Stale;
        return outcome;
    }
    const auto& tag = snapshot->latest_response_id;
    if (allow_covered && store_.covers(session, fingerprint_json_, tag)) {
        outcome.covered = true;
        outcome.tag = std::move(snapshot->latest_response_id);
        return outcome;
    }
    try {
        outcome.saved = store_.save(*snapshot, fingerprint_json_,
            [&](runtime::ContinuationCheckpointWriter& writer) {
                return Access::export_session(engine_, session, tag, writer, limits_);
            },
            [&](runtime::DurableSessionEpoch epoch, const std::function<bool()>& commit) {
                return Access::publish_export(engine_, session, tag, epoch, commit);
            }, &outcome.skip);
    } catch (...) {
        outcome.skip.export_state = runtime::DurableExportState::Refused;
        if (outcome.skip.durable_reason == Reason::None) { outcome.skip.durable_reason = Reason::IoUnavailable; }
    }
    outcome.tag = std::move(snapshot->latest_response_id);
    return outcome;
}

Json SessionCheckpointService::save(std::string_view session) {
    const auto outcome = save_session(session, {}, false);
    Json result;
    if (outcome.saved) {
        result = operation("save", "exported", Reason::None, true, outcome.saved->engine,
                           outcome.saved->origin_authenticated ? "authenticated" : "unauthenticated");
        result["dir_sync"] = outcome.saved->dir_sync;
    } else {
        result = operation("save", outcome.skip.export_state == runtime::DurableExportState::Stale
                                      ? "stale" : "refused", outcome.skip.durable_reason, false);
    }
    receipt(result);
    if (outcome.saved) {
        progress_.fetch_add(1, std::memory_order_release);
        Access::notify_checkpoint_progress(engine_);
    }
    return result;
}

Json SessionCheckpointService::status(std::string_view session) const {
    return store_.status(session, fingerprint_json_);
}

std::shared_ptr<std::mutex> SessionCheckpointService::session_operation(std::string_view session) {
    std::lock_guard lock(operations_mutex_);
    // Entries exist only for active/waiting operations; an idle batch is retired on the next
    // admission. Session churn cannot leave an unbounded tombstone table.
    for (auto at = operations_.begin(); at != operations_.end();) {
        if (at->second.expired()) { at = operations_.erase(at); } else { ++at; }
    }
    const auto key = std::string(session);
    auto& weak = operations_[key];
    if (auto active = weak.lock()) { return active; }
    auto active = std::make_shared<std::mutex>();
    weak = active;
    return active;
}

SessionCheckpointEraseResult SessionCheckpointService::erase(std::string_view session) {
    auto operation_mutex = session_operation(session);
    std::lock_guard operation_lock(*operation_mutex);
    try { return store_.erase(session); }
    catch (...) {
        throw ApiException({.status = 503, .type = "server_error",
            .message = "checkpoint storage is unavailable", .code = "io_unavailable"});
    }
}

bool SessionCheckpointService::erase_response(const std::string& id, const std::string& session) {
    auto operation_mutex = session_operation(session);
    std::lock_guard operation_lock(*operation_mutex);
    if (!responses_.get_for_session(id, session)) { return false; }
    SessionCheckpointEraseResult erased;
    try { erased = store_.erase(session); }
    catch (...) {
        throw ApiException({.status = 503, .type = "server_error",
            .message = "checkpoint storage is unavailable", .code = "io_unavailable"});
    }
    if (erased == SessionCheckpointEraseResult::Conflict) {
        throw ApiException({.status = 409, .type = "server_error",
            .message = "session checkpoint is in use", .code = "transaction_busy"});
    }
    return responses_.erase_for_session(id, session);
}

std::shared_ptr<const runtime::ContinuationCheckpointReader>
SessionCheckpointService::pin_recoverable(std::string_view session, std::string_view tag) const noexcept {
    try {
        auto loaded = store_.load(session, fingerprint_json_, tag);
        if (loaded.state != SessionCheckpointLoadState::Available || !loaded.checkpoint ||
            loaded.checkpoint->responses.latest_response_id != tag) { return {}; }
        return std::move(loaded.checkpoint->engine);
    } catch (...) { return {}; }
}

void SessionCheckpointService::response_stored(std::string_view session, std::uint64_t frontier_tokens) noexcept {
    progress_.fetch_add(1, std::memory_order_release);
    Access::notify_checkpoint_progress(engine_);
    if (stopping_.load(std::memory_order_acquire)) { return; }
    try {
        { std::lock_guard lock(pressure_mutex_); pressure_saves_.erase(std::string(session)); }
        if (checkpoint_worth_saving(frontier_tokens, options_.session_checkpoint_min_tokens,
                                    store_.may_hold(session))) {
            (void)automatic_.enqueue(std::string(session));
        }
    } catch (...) {}
}

void SessionCheckpointService::publication_changed() noexcept {
    progress_.fetch_add(1, std::memory_order_release);
    Access::notify_checkpoint_progress(engine_);
}

void SessionCheckpointService::automatic_save(std::string_view session) noexcept {
    try {
        const auto outcome = save_session(session);
        const bool settled = outcome.saved || outcome.covered ||
            (!checkpoint_refusal_is_transient(outcome.skip) && outcome.skip.durable_reason != Reason::IoUnavailable);
        if (outcome.saved) {
            auto value = operation("save", "exported", Reason::None, true, outcome.saved->engine,
                                   outcome.saved->origin_authenticated ? "authenticated" : "unauthenticated");
            value["dir_sync"] = outcome.saved->dir_sync;
            receipt(value);
        } else if (!outcome.covered) {
            receipt(operation("save", outcome.skip.export_state == runtime::DurableExportState::Stale
                                          ? "stale" : "refused", outcome.skip.durable_reason, false));
        }
        { std::lock_guard lock(pressure_mutex_);
          const auto found = pressure_saves_.find(std::string(session));
          if (found != pressure_saves_.end() && found->second.tag == outcome.tag) {
              found->second.settled = settled;
          } }
        if (settled) {
            progress_.fetch_add(1, std::memory_order_release);
            Access::notify_checkpoint_progress(engine_);
        }
    } catch (...) {}
}

void SessionCheckpointService::save_before_eviction(
    std::span<runtime::PressureCheckpointVictim> victims) noexcept {
    using Outcome = runtime::PressureCheckpointOutcome;
    for (auto& victim : victims) {
        victim.outcome = Outcome::Pending;
        try {
            const bool covered = store_.covers(
                victim.session_sha256, fingerprint_json_, victim.checkpoint_tag);
            const auto publication = covered ? ResponsePublicationState::Stored
                : responses_.await_publication(victim.session_sha256, victim.checkpoint_tag,
                                                std::chrono::milliseconds(0));
            if (covered || publication == ResponsePublicationState::Absent) {
                victim.outcome = Outcome::Settled;
                std::lock_guard lock(pressure_mutex_);
                pressure_saves_.erase(victim.session_sha256);
                continue;
            }
            // Native completion may precede its HTTP handler. Do not export an older
            // snapshot or manufacture retry progress while this exact reply is publishing.
            // Its publication guard notifies again on either put or abandonment.
            if (publication == ResponsePublicationState::Publishing) { continue; }
            std::lock_guard lock(pressure_mutex_);
            auto found = pressure_saves_.find(victim.session_sha256);
            const auto progress = progress_.load(std::memory_order_acquire);
            if (found != pressure_saves_.end() && found->second.tag == victim.checkpoint_tag &&
                found->second.settled) {
                victim.outcome = Outcome::Settled;
                pressure_saves_.erase(found);
                continue;
            }
            if (found != pressure_saves_.end() && found->second.tag == victim.checkpoint_tag &&
                found->second.attempted_progress == progress) { continue; }
            if (found == pressure_saves_.end() && pressure_saves_.size() >= 64) { continue; }
            pressure_saves_[victim.session_sha256] = {
                .tag = victim.checkpoint_tag, .attempted_progress = progress};
            if (automatic_.enqueue(victim.session_sha256) == AutomaticCheckpointEnqueueResult::Dropped) {
                pressure_saves_.erase(victim.session_sha256);
            }
        } catch (...) {}
    }
}

bool SessionCheckpointService::write_shutdown_report(const ShutdownCheckpointSummary& summary) const noexcept {
    if (options_.shutdown_report.empty()) { return true; }
    try {
        const auto separator = options_.stop_event.rfind('-');
        const auto launch_id = separator == std::string::npos ? std::string() : options_.stop_event.substr(separator + 1);
        const Json report{{"artifact_type", "ninfer_serve_shutdown_report"}, {"schema_version", 1},
            {"launch_id", launch_id}, {"saved", summary.saved}, {"skipped", summary.skipped},
            {"refused", summary.refused}, {"complete", summary.complete()}};
        const auto path = std::filesystem::absolute(options_.shutdown_report).lexically_normal();
        io::create_directories(path.parent_path());
        const auto temporary = path.string() + "." + new_opaque_id("shutdown_") + ".tmp";
        const auto text = report.dump() + "\n";
        const auto close = [](std::FILE* file) { std::fclose(file); };
        std::unique_ptr<std::FILE, decltype(close)> file(io::open_binary_write(temporary), close);
        io::write_exact(file.get(), std::as_bytes(std::span(text.data(), text.size())));
        io::flush_file(file.get());
        file.reset();
        io::replace_path(temporary, path);
        io::sync_directory(path.parent_path());
        return true;
    } catch (...) { return false; }
}

bool SessionCheckpointService::final_save() noexcept {
    std::lock_guard lock(final_mutex_);
    if (final_result_) { return *final_result_; }
    stopping_.store(true, std::memory_order_release);
    automatic_.drain();
    ShutdownCheckpointSummary summary;
    try {
        for (const auto& session : responses_.session_digests()) {
            const auto outcome = save_session(session);
            if (outcome.saved) {
                ++summary.saved;
                auto value = operation("save", "exported", Reason::None, true, outcome.saved->engine,
                                       outcome.saved->origin_authenticated ? "authenticated" : "unauthenticated");
                value["dir_sync"] = outcome.saved->dir_sync;
                receipt(value);
            } else if (outcome.covered || outcome.skip.durable_reason == Reason::NoSessionRecords) {
                ++summary.skipped;
            } else {
                ++summary.refused;
                receipt(operation("save", "refused", outcome.skip.durable_reason, false));
            }
        }
    } catch (...) { ++summary.refused; }
    const bool reported = write_shutdown_report(summary);
    final_result_ = summary.complete() && reported;
    return *final_result_;
}

} // namespace ninfer::serve
