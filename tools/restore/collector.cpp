// Never-merge CUDA 13.1 diagnostic collector. Load before the first CUDA initialization.
#include "core/restore_trace.h"
#include <cupti.h>
#include <cuda.h>
#include <dlfcn.h>
#include <array>
#include <atomic>
#include <cstring>

namespace {
using ninfer::restore_trace::Recorder;
Recorder& output() { static Recorder recorder("RESTORE_CUDA_FILE"); return recorder; }
CUpti_SubscriberHandle subscriber{};
constexpr std::size_t buffer_size = 1U << 20;
alignas(8) std::array<std::array<std::uint8_t, buffer_size>, 8> buffers{};
std::array<std::atomic<bool>, 8> used{};
void check(CUptiResult code, const char* operation) {
    output().emit("cupti.result", operation, code);
    if (code != CUPTI_SUCCESS) {
        // Refuse the diagnostic process; never produce an apparently complete partial trace.
        std::fprintf(stderr, "restore collector initialization failed: %s (%u)\n", operation, code);
        std::abort();
    }
}
void symbol(const char* name, std::uint64_t id, std::uint64_t kind) {
    if (!name) return;
    const std::string_view text(name);
    for (std::size_t i = 0; i < text.size(); i += 112)
        output().emit("cuda.symbol", text.substr(i, 112), id, kind, i, text.size());
}
void CUPTIAPI callback(void*, CUpti_CallbackDomain domain, CUpti_CallbackId id, const void* data) {
    if (domain == CUPTI_CB_DOMAIN_DRIVER_API || domain == CUPTI_CB_DOMAIN_RUNTIME_API) {
        const auto& event = *static_cast<const CUpti_CallbackData*>(data);
        output().emit(event.callbackSite == CUPTI_API_ENTER ? "cuda.api.begin" : "cuda.api.end",
                      event.functionName ? event.functionName : "", domain, id,
                      event.correlationId, event.contextUid,
                      reinterpret_cast<std::uintptr_t>(event.context));
        if (event.callbackSite == CUPTI_API_ENTER) symbol(event.symbolName, event.correlationId, domain);
        const bool exit = event.callbackSite == CUPTI_API_EXIT;
        const int result = exit && event.functionReturnValue
            ? *static_cast<const int*>(event.functionReturnValue) : -1;
        if (exit) output().emit("cuda.api.result", {}, domain, id, event.correlationId, result);
        if (domain == CUPTI_CB_DOMAIN_DRIVER_API &&
            std::strcmp(event.functionName, "cuModuleGetFunction") == 0) {
            const auto& p = *static_cast<const cuModuleGetFunction_params*>(event.functionParams);
            output().emit("cuda.lookup.module", {}, event.correlationId, exit,
                          reinterpret_cast<std::uintptr_t>(p.hmod),
                          exit && result == 0 ? reinterpret_cast<std::uintptr_t>(*p.hfunc) : 0);
            if (!exit) symbol(p.name, event.correlationId, domain);
        }
        if (domain == CUPTI_CB_DOMAIN_RUNTIME_API &&
            std::strcmp(event.functionName, "cudaLibraryGetKernel") == 0) {
            const auto& p = *static_cast<const cudaLibraryGetKernel_v12060_params*>(event.functionParams);
            output().emit("cuda.lookup.library", {}, event.correlationId, exit,
                          reinterpret_cast<std::uintptr_t>(p.library),
                          exit && result == 0 ? reinterpret_cast<std::uintptr_t>(*p.pKernel) : 0);
            if (!exit) symbol(p.name, event.correlationId, domain);
        }
    } else if (domain == CUPTI_CB_DOMAIN_RESOURCE) {
        const auto& event = *static_cast<const CUpti_ResourceData*>(data);
        std::uint32_t context = 0;
        const auto result = cuptiGetContextId(event.context, &context);
        output().emit("cuda.resource", {}, id, context,
                      reinterpret_cast<std::uintptr_t>(event.context), result);
        if (id == CUPTI_CBID_RESOURCE_MODULE_LOADED || id == CUPTI_CBID_RESOURCE_MODULE_UNLOAD_STARTING) {
            const auto& module = *static_cast<const CUpti_ModuleResourceData*>(event.resourceDescriptor);
            // Deliberately do not inspect, hash, copy or force resolution of module bytes.
            output().emit("cuda.module", {}, id, context, module.moduleId, module.cubinSize);
        }
    }
}
void CUPTIAPI request_buffer(std::uint8_t** data, std::size_t* size, std::size_t* max) {
    *max = 0;
    for (std::size_t i = 0; i != used.size(); ++i) {
        if (!used[i].exchange(true)) { *data = buffers[i].data(); *size = buffer_size; return; }
    }
    *data = nullptr; *size = 0;
    output().emit("cupti.pool.exhausted");
}
void CUPTIAPI complete_buffer(CUcontext context, std::uint32_t stream, std::uint8_t* data,
                              std::size_t, std::size_t valid) {
    CUpti_Activity* record = nullptr;
    CUptiResult result;
    while ((result = cuptiActivityGetNextRecord(data, valid, &record)) == CUPTI_SUCCESS) {
        switch (record->kind) {
        case CUPTI_ACTIVITY_KIND_MEMCPY: {
            const auto& r = *reinterpret_cast<CUpti_ActivityMemcpy*>(record);
            output().emit("cuda.copy", {}, r.correlationId, r.contextId, r.streamId, r.start, r.end, r.bytes);
            output().emit("cuda.copy.kind", {}, r.correlationId, r.copyKind, r.srcKind, r.dstKind);
            break;
        }
        case CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL: {
            const auto& r = *reinterpret_cast<CUpti_ActivityKernel4*>(record);
            output().emit("cuda.kernel", {}, r.correlationId, r.contextId, r.streamId, r.start, r.end);
            symbol(r.name, r.correlationId, CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL);
            break;
        }
        case CUPTI_ACTIVITY_KIND_FUNCTION: {
            const auto& r = *reinterpret_cast<CUpti_ActivityFunction*>(record);
            output().emit("cuda.function", {}, r.id, r.contextId, r.moduleId, r.functionIndex);
            symbol(r.name, r.id, CUPTI_ACTIVITY_KIND_FUNCTION);
            break;
        }
        case CUPTI_ACTIVITY_KIND_CUDA_EVENT: {
            const auto& r = *reinterpret_cast<CUpti_ActivityCudaEvent2*>(record);
            output().emit("cuda.event", {}, r.correlationId, r.contextId, r.streamId,
                          r.eventId, r.deviceTimestamp, r.cudaEventSyncId);
            break;
        }
        case CUPTI_ACTIVITY_KIND_SYNCHRONIZATION: {
            const auto& r = *reinterpret_cast<CUpti_ActivitySynchronization2*>(record);
            output().emit("cuda.sync", {}, r.correlationId, r.contextId, r.streamId, r.start, r.end, r.cudaEventSyncId);
            break;
        }
        case CUPTI_ACTIVITY_KIND_DRIVER:
        case CUPTI_ACTIVITY_KIND_RUNTIME: {
            const auto& r = *reinterpret_cast<CUpti_ActivityAPI*>(record);
            output().emit("cuda.api.interval", {}, r.correlationId, r.cbid, r.threadId, r.start, r.end, r.kind);
            break;
        }
        default: output().emit("cuda.unparsed", {}, record->kind); break;
        }
    }
    if (result != CUPTI_ERROR_MAX_LIMIT_REACHED) output().emit("cupti.decode.error", {}, result);
    std::size_t dropped = 0;
    const auto drop_result = cuptiActivityGetNumDroppedRecords(context, stream, &dropped);
    output().emit("cupti.dropped", {}, dropped, drop_result, stream);
    for (std::size_t i = 0; i != used.size(); ++i) if (buffers[i].data() == data) used[i] = false;
}
void finish() {
    cuptiActivityFlushAll(CUPTI_ACTIVITY_FLAG_FLUSH_FORCED);
    cuptiUnsubscribe(subscriber);
    output().emit("cupti.end");
}
__attribute__((constructor)) void initialize() {
    if (!std::getenv("RESTORE_CUDA_FILE")) return;
    auto& recorder = output();
    const char* loading = std::getenv("CUDA_MODULE_LOADING");
    recorder.emit("cuda.loading.environment", !loading ? "UNSET" :
        std::strcmp(loading, "LAZY") == 0 ? "LAZY" :
        std::strcmp(loading, "EAGER") == 0 ? "EAGER" : "OTHER");
    check(cuptiActivityRegisterTimestampCallback(ninfer::restore_trace::monotonic_ns), "timestamp.clock_monotonic_ns");
    // Bound CUPTI device-side buffers as well as our two fixed 8-MiB producer pools.
    std::size_t size = sizeof(std::size_t), bytes = 1U << 20, count = 4;
    check(cuptiActivitySetAttribute(CUPTI_ACTIVITY_ATTR_DEVICE_BUFFER_SIZE, &size, &bytes), "device.buffer.bytes");
    check(cuptiActivitySetAttribute(CUPTI_ACTIVITY_ATTR_DEVICE_BUFFER_POOL_LIMIT, &size, &count), "device.buffer.limit");
    check(cuptiSubscribe(&subscriber, callback, nullptr), "subscribe");
    check(cuptiEnableDomain(1, subscriber, CUPTI_CB_DOMAIN_DRIVER_API), "driver.callbacks");
    check(cuptiEnableDomain(1, subscriber, CUPTI_CB_DOMAIN_RUNTIME_API), "runtime.callbacks");
    check(cuptiEnableDomain(1, subscriber, CUPTI_CB_DOMAIN_RESOURCE), "resource.callbacks");
    check(cuptiActivityRegisterCallbacks(request_buffer, complete_buffer), "buffers");
    check(cuptiActivityEnableCudaEventDeviceTimestamps(1), "event.timestamps");
    for (auto kind : {CUPTI_ACTIVITY_KIND_DRIVER, CUPTI_ACTIVITY_KIND_RUNTIME,
                     CUPTI_ACTIVITY_KIND_MEMCPY, CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL,
                     CUPTI_ACTIVITY_KIND_FUNCTION, CUPTI_ACTIVITY_KIND_CUDA_EVENT,
                     CUPTI_ACTIVITY_KIND_SYNCHRONIZATION}) check(cuptiActivityEnable(kind), "activity.enable");
    recorder.emit("cupti.begin", {}, CUPTI_API_VERSION);
    std::atexit(finish);
}
} // namespace
extern "C" __attribute__((visibility("default"))) void restore_cuda_loading_mode() {
    CUmoduleLoadingMode mode{};
    int driver = 0;
    const auto result = cuModuleGetLoadingMode(&mode);
    const auto version = cuDriverGetVersion(&driver);
    output().emit("cuda.loading.effective", {}, result, mode, version, driver);
}
