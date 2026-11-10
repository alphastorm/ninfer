#include "serve/operational_log.h"

#include "product/logging/pretty_format.h"

#include <spdlog/logger.h>

namespace ninfer::serve {
namespace {

const char* kv_cache_name(ninfer::KvCacheStorage storage) noexcept {
    switch (storage) {
    case ninfer::KvCacheStorage::BFloat16:
        return "bf16";
    case ninfer::KvCacheStorage::Int8Group64:
        return "int8";
    case ninfer::KvCacheStorage::Fp8E4M3Row256:
        return "fp8";
    case ninfer::KvCacheStorage::Nvfp4Group16:
        return "nvfp4";
    case ninfer::KvCacheStorage::Fp8KeyNvfp4Value:
        return "k8v4";
    case ninfer::KvCacheStorage::Rk4V4:
        return "rk4v4";
    }
    return "unknown";
}

const char* kv_capacity_mode_name(ninfer::KvCapacityMode mode) noexcept {
    return mode == ninfer::KvCapacityMode::Automatic ? "auto" : "explicit";
}

} // namespace

void OperationalLog::engine_capacity(const GenerationService& service) const {
    const ninfer::MemorySummary memory            = service.memory_summary();
    const ninfer::EngineOptions& engine           = service.engine_options();
    const ninfer::ContextCacheOptions& cache      = engine.context_cache;
    const ninfer::ContextCostSummary context_cost = service.load_summary().context_cost;

    logger_->info("capacity | KV {} tokens, {}, {} | pages {}/{} | runtime {} | free {}",
                  product::format_pretty_count(memory.kv_capacity), kv_cache_name(memory.kv_cache),
                  kv_capacity_mode_name(memory.kv_capacity_mode),
                  product::format_pretty_count(memory.kv_capacity_page_groups),
                  product::format_pretty_count(memory.kv_capacity_max_page_groups),
                  product::format_pretty_bytes(memory.runtime_reservation_bytes),
                  product::format_pretty_bytes(memory.available_after_startup_bytes));

    logger_->info("context | history {} | {} active + {} extra device states | host {}",
                  cache.enabled ? "on" : "off", engine.max_concurrency, *cache.device_state_slots,
                  product::format_pretty_bytes(*cache.host_capacity_bytes));

    if (service.options().enable_vision) {
        const ninfer::MediaCacheSummary media = service.media_cache_summary();
        logger_->info("media | {} preprocess workers | cache {} | live {}",
                      media.preprocess_threads, product::format_pretty_bytes(media.capacity_bytes),
                      product::format_pretty_bytes(media.live_capacity_bytes));
    }

    const auto graph = memory.cuda_graph.value_or(ninfer::GraphMemorySummary{});
    logger_->debug(
        "memory ledger | after weights {} | after startup {} | headroom {} | slack {} | "
        "graph rebuild estimate {} | graph preparation increment {} B | "
        "graph calibration increment {} B | graph calibration retained {} B | startup retained {} "
        "B",
        product::format_pretty_bytes(memory.available_after_weights_bytes),
        product::format_pretty_bytes(memory.available_after_startup_bytes),
        product::format_pretty_bytes(memory.kv_capacity_headroom_bytes),
        product::format_pretty_bytes(memory.planned_slack_bytes),
        product::format_pretty_bytes(graph.rebuild_estimate_bytes),
        graph.preparation_increment_bytes, graph.calibration_increment_bytes,
        graph.calibration_retained_bytes, memory.startup_retained_bytes);
    logger_->debug("context cost | transfer {} | prefill {} | hardware {} | prefill signature {}",
                   ninfer::context_cost_preset_source_name(context_cost.transfer_source),
                   ninfer::context_cost_preset_source_name(context_cost.prefill_source),
                   product::format_pretty_text(context_cost.hardware_class),
                   product::format_pretty_text(context_cost.prefill_signature));
}

} // namespace ninfer::serve
