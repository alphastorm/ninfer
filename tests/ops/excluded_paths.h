#pragma once

#include <array>
#include <iostream>
#include <string_view>

namespace ninfer::test {

// Exact refusal messages, not substring matches: unrelated runtime errors remain failures.
// Only unsupported activation/attention implementations are excluded; A16 dequantization
// of FP8/NVFP4 weights must still pass its independent mathematical oracle.
inline constexpr std::array kBlackwellOnlyPaths{
    std::string_view{"NVFP4 A4 execution requires an sm_120a GPU"},
    std::string_view{"NVFP4 KV attention requires an sm_120a GPU"},
    std::string_view{"K8V4 KV attention requires an sm_120a GPU"},
    std::string_view{"Block-scaled MoE execution requires an sm_120a GPU"},
    std::string_view{"Striped TMA weight access requires an sm_120a GPU"},
};
inline constexpr std::array kAdaOrNewerPaths{
    std::string_view{"FP8 A8 execution requires an sm_89 or newer GPU"},
    std::string_view{"FP8 KV attention requires an sm_89 or newer GPU"},
};

inline bool report_excluded_path(std::string_view label, std::string_view message) {
#if defined(NINFER_SM86) || defined(NINFER_SM89)
    for (auto path : kBlackwellOnlyPaths) {
        if (message == path) {
            std::cout << label << ": EXCLUDED (" << path << ")\n";
            return true;
        }
    }
#endif
#if defined(NINFER_SM86)
    for (auto path : kAdaOrNewerPaths) {
        if (message == path) {
            std::cout << label << ": EXCLUDED (" << path << ")\n";
            return true;
        }
    }
#endif
    return false;
}

} // namespace ninfer::test
