#include "core/host_decode.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void setting(const char* name, const char* value) {
#ifdef _WIN32
    if (_putenv_s(name, value ? value : "") != 0) throw std::runtime_error("_putenv_s failed");
#else
    if ((value ? setenv(name, value, 1) : unsetenv(name)) != 0)
        throw std::runtime_error("setenv failed");
#endif
}
}

int main() {
    constexpr const char* name = "NINFER_HOST_DECODE_TEST_FLAG";
    setting(name, nullptr);
    if (ninfer::host_decode_flag(name)) return 1;
    setting(name, "0");
    if (ninfer::host_decode_flag(name)) return 1;
    setting(name, "1");
    if (!ninfer::host_decode_flag(name)) return 1;
    for (const auto* invalid : {"true", "2", "-1", " 1"}) {
        setting(name, invalid);
        try {
            (void)ninfer::host_decode_flag(name);
            return 1;
        } catch (const std::invalid_argument& error) {
            if (std::string_view(error.what()).find(name) == std::string_view::npos) return 1;
        }
    }
    setting(name, nullptr);
    for (const auto* flag : {"NINFER_HOST_TIMER_1MS", "NINFER_HOST_POWER_OPTOUT",
                             "NINFER_HOST_THREAD_PRIORITY", "NINFER_HOST_PCORE_AFFINITY"})
        setting(flag, "0");
    { ninfer::DecodeHostPolicy unchanged; }
#ifdef _WIN32
    const int priority = GetThreadPriority(GetCurrentThread());
    setting("NINFER_HOST_THREAD_PRIORITY", "1");
    {
        ninfer::DecodeHostPolicy raised;
        if (GetThreadPriority(GetCurrentThread()) != THREAD_PRIORITY_HIGHEST) return 1;
    }
    if (GetThreadPriority(GetCurrentThread()) != priority) return 1;
    setting("NINFER_HOST_THREAD_PRIORITY", "0");
    // A homogeneous host is valid; a hybrid host must restore its initial mask.
    GROUP_AFFINITY before{}, after{};
    if (!GetThreadGroupAffinity(GetCurrentThread(), &before)) return 1;
    setting("NINFER_HOST_PCORE_AFFINITY", "1");
    { ninfer::DecodeHostPolicy affinity; }
    if (!GetThreadGroupAffinity(GetCurrentThread(), &after) || before.Group != after.Group ||
        before.Mask != after.Mask) return 1;
#else
    for (const auto* flag : {"NINFER_HOST_TIMER_1MS", "NINFER_HOST_POWER_OPTOUT",
                             "NINFER_HOST_THREAD_PRIORITY", "NINFER_HOST_PCORE_AFFINITY"}) {
        setting(flag, "1");
        try {
            ninfer::DecodeHostPolicy unsupported;
            return 1;
        } catch (const std::invalid_argument&) {}
        setting(flag, "0");
    }
#endif
    std::cout << "host decode environment and scoped thread policy passed\n";
}
