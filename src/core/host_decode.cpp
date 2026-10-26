#include "core/host_decode.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <mmsystem.h>
#endif

namespace ninfer {

bool host_decode_flag(const char* name) {
    const char* value = std::getenv(name);
    if (!value || std::string_view(value) == "0") return false;
    if (std::string_view(value) == "1") return true;
    throw std::invalid_argument(std::string(name) + " must be 0 or 1");
}

#ifdef _WIN32
namespace {

// Process policy is shared by Engine workers; the last owner restores the exact
// initial state. This mutex is touched only at worker creation/destruction.
std::mutex power_mutex;
unsigned power_owners = 0;
PROCESS_POWER_THROTTLING_STATE saved_power{};

void windows_check(BOOL success, const char* operation) {
    if (!success) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 std::to_string(GetLastError()));
    }
}

void acquire_power() {
    std::lock_guard lock(power_mutex);
    if (power_owners == 0) {
        saved_power = {};
        saved_power.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        windows_check(GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                                             &saved_power, sizeof(saved_power)),
                      "GetProcessInformation(ProcessPowerThrottling)");
        auto state = saved_power;
        state.ControlMask |= PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask &= ~PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        windows_check(SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                                             &state, sizeof(state)),
                      "SetProcessInformation(ProcessPowerThrottling)");
    }
    ++power_owners;
}

void release_power() noexcept {
    std::lock_guard lock(power_mutex);
    if (--power_owners == 0 &&
        !SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                               &saved_power, sizeof(saved_power))) {
        std::fprintf(stderr, "host decode: cannot restore process power throttling (%lu)\n",
                     GetLastError());
    }
}

// Respect the current thread group and allowed mask. Do not guess a hybrid CPU's
// P-core numbering: Windows reports the relative efficiency class per core.
KAFFINITY performance_cores(const GROUP_AFFINITY& allowed) {
    DWORD size = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        throw std::runtime_error("cannot size Windows processor topology");
    }
    std::vector<unsigned char> bytes(size);
    windows_check(GetLogicalProcessorInformationEx(RelationProcessorCore,
                    reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(bytes.data()),
                    &size), "GetLogicalProcessorInformationEx");
    BYTE lowest = 255, highest = 0;
    KAFFINITY mask = 0;
    for (DWORD offset = 0; offset < size;) {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            bytes.data() + offset);
        if (entry->Size == 0 || entry->Size > size - offset)
            throw std::runtime_error("invalid Windows processor topology record");
        if (entry->Relationship == RelationProcessorCore) {
            const auto& core = entry->Processor;
            lowest = core.EfficiencyClass < lowest ? core.EfficiencyClass : lowest;
            if (core.EfficiencyClass > highest) {
                highest = core.EfficiencyClass;
                mask = 0;
            }
            if (core.EfficiencyClass == highest) {
                for (WORD group = 0; group < core.GroupCount; ++group)
                    if (core.GroupMask[group].Group == allowed.Group)
                        mask |= core.GroupMask[group].Mask & allowed.Mask;
            }
        }
        offset += entry->Size;
    }
    if (lowest == highest) return 0; // Homogeneous CPUs have no P-core lever.
    if (!mask) throw std::runtime_error("no allowed performance core in worker CPU group");
    return mask;
}

} // namespace
#endif

DecodeHostPolicy::DecodeHostPolicy() {
    const bool timer = host_decode_flag("NINFER_HOST_TIMER_1MS");
    const bool power = host_decode_flag("NINFER_HOST_POWER_OPTOUT");
    const bool priority = host_decode_flag("NINFER_HOST_THREAD_PRIORITY");
    const bool affinity = host_decode_flag("NINFER_HOST_PCORE_AFFINITY");
#ifdef _WIN32
    try {
        if (timer) {
            if (timeBeginPeriod(1) != TIMERR_NOERROR)
                throw std::runtime_error("timeBeginPeriod(1) failed");
            timer_ = true;
        }
        if (power) {
            acquire_power();
            power_ = true;
        }
        if (priority) {
            previous_priority_ = GetThreadPriority(GetCurrentThread());
            windows_check(previous_priority_ != THREAD_PRIORITY_ERROR_RETURN, "GetThreadPriority");
            windows_check(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST),
                          "SetThreadPriority");
            priority_ = true;
        }
        if (affinity) {
            windows_check(GetThreadGroupAffinity(GetCurrentThread(), &previous_affinity_),
                          "GetThreadGroupAffinity");
            const auto mask = performance_cores(previous_affinity_);
            if (mask) {
                auto selected = previous_affinity_;
                selected.Mask = mask;
                windows_check(SetThreadGroupAffinity(GetCurrentThread(), &selected, nullptr),
                              "SetThreadGroupAffinity");
                affinity_ = true;
            } else {
                std::fprintf(stderr, "host decode: P-core affinity not applicable (homogeneous CPU)\n");
            }
        }
    } catch (...) {
        restore();
        throw;
    }
#else
    if (timer || power || priority || affinity)
        throw std::invalid_argument("Windows host decode controls require a native Windows build");
#endif
}

DecodeHostPolicy::~DecodeHostPolicy() noexcept { restore(); }

void DecodeHostPolicy::restore() noexcept {
#ifdef _WIN32
    if (affinity_ && !SetThreadGroupAffinity(GetCurrentThread(), &previous_affinity_, nullptr))
        std::fprintf(stderr, "host decode: cannot restore thread affinity (%lu)\n", GetLastError());
    affinity_ = false;
    if (priority_ && !SetThreadPriority(GetCurrentThread(), previous_priority_))
        std::fprintf(stderr, "host decode: cannot restore thread priority (%lu)\n", GetLastError());
    priority_ = false;
    if (power_) release_power();
    power_ = false;
    if (timer_ && timeEndPeriod(1) != TIMERR_NOERROR)
        std::fprintf(stderr, "host decode: cannot restore timer period\n");
    timer_ = false;
#endif
}

DecodeRoundWait::DecodeRoundWait(const DeviceContext& device)
    : flush_(host_decode_flag("NINFER_HOST_ROUND_FLUSH")) {
    if (host_decode_flag("NINFER_HOST_ROUND_POLL")) event_.emplace(device);
}

void DecodeRoundWait::submitted(cudaStream_t stream) {
    if (event_) event_->record(stream);
    if (flush_) {
        const auto result = cudaStreamQuery(stream);
        if (result == cudaErrorNotReady) {
            // A not-ready query is expected and must not poison later error checks.
            const auto pending = cudaGetLastError();
            if (pending != cudaErrorNotReady) CUDA_CHECK(pending);
        } else {
            CUDA_CHECK(result);
        }
    }
}

void DecodeRoundWait::wait(const DeviceContext& device) const {
    if (event_) {
        while (!event_->ready()) {}
        const auto pending = cudaGetLastError();
        if (pending != cudaErrorNotReady) CUDA_CHECK(pending);
    } else {
        device.synchronize();
    }
}

} // namespace ninfer
