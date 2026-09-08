#pragma once

// Activation paths a build excludes. Ada (sm_89) and Ampere (sm_86) builds replace the NVFP4
// W4A4 launchers, and Ampere the FP8 A8 and FP8-KV Attention launchers, with stubs that refuse
// the plan (src/ops/nvfp4_a16_only_stubs.cpp, src/ops/fp8_sm86_stubs.cpp). A test arm that
// reaches one of those launchers on such a build ends as "excluded": the refusal is the
// contract there, never a silent fallback to the A16 path, and every case the arm ran before
// the refusal keeps its verdict. On an sm_120a build no refusal is ever legitimate.

#include <exception>
#include <iostream>
#include <string_view>
#include <utility>

namespace ninfer::test {

#if defined(NINFER_SM86)
inline constexpr bool kBuildRunsNvfp4A4 = false;
inline constexpr bool kBuildRunsFp8     = false; // FP8 A8 contractions and FP8-KV Attention
#elif defined(NINFER_SM89)
inline constexpr bool kBuildRunsNvfp4A4 = false;
inline constexpr bool kBuildRunsFp8     = true;
#else
inline constexpr bool kBuildRunsNvfp4A4 = true;
inline constexpr bool kBuildRunsFp8     = true;
#endif

// Whether `what` is a refusal from a stub standing in for a path this build excludes.
inline bool build_excludes(std::string_view what) {
    if (what.find("requires an sm_120a") != std::string_view::npos) { return !kBuildRunsNvfp4A4; }
    if (what.find("requires an sm_89") != std::string_view::npos) { return !kBuildRunsFp8; }
    return false;
}

inline int& excluded_arm_count() {
    static int count = 0;
    return count;
}

// Runs one test arm and returns its failure count. A refusal from an excluded-path stub ends
// the arm with zero further failures and counts it; any other exception propagates.
template <typename Arm> int arm(std::string_view label, Arm&& run) {
    try {
        return std::forward<Arm>(run)();
    } catch (const std::exception& error) {
        if (!build_excludes(error.what())) { throw; }
        std::cout << label << ": excluded on this build (" << error.what() << ")\n";
        ++excluded_arm_count();
        return 0;
    }
}

// For a suite whose subject is an excluded path: the build must have refused it at least once,
// or the stubs are letting the plan through to something else.
inline int require_excluded(std::string_view label, bool build_runs_path) {
    if (build_runs_path || excluded_arm_count() > 0) { return 0; }
    std::cerr << label << ": this build excludes the path but nothing refused it\n";
    return 1;
}

} // namespace ninfer::test
