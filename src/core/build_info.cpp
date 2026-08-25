#include "ninfer/build_info.h"

#include "ninfer_build_info_config.h"

#include <sstream>

namespace ninfer {

BuildInfo build_info() noexcept {
    return BuildInfo{
        .upstream_base_sha = NINFER_BUILD_UPSTREAM_BASE_SHA,
        .patch_stack_sha   = NINFER_BUILD_PATCH_STACK_SHA,
        .build_profile     = NINFER_BUILD_PROFILE,
        .build_type        = NINFER_BUILD_TYPE,
        .cxx_compiler      = NINFER_BUILD_CXX_COMPILER,
        .cuda_compiler     = NINFER_BUILD_CUDA_COMPILER,
        .cuda_toolkit      = NINFER_BUILD_CUDA_TOOLKIT,
        .source_dirty      = NINFER_BUILD_SOURCE_DIRTY != 0,
    };
}

std::string format_build_info(std::string_view program_name) {
    const BuildInfo info = build_info();
    std::ostringstream out;
    out << program_name << " upstream_base_sha=" << info.upstream_base_sha
        << " patch_stack_sha=" << info.patch_stack_sha << " build_profile=" << info.build_profile
        << " build_type=" << info.build_type << " cxx_compiler=" << info.cxx_compiler
        << " cuda_compiler=" << info.cuda_compiler << " cuda_toolkit=" << info.cuda_toolkit
        << " source_dirty=" << (info.source_dirty ? "true" : "false");
    return out.str();
}

} // namespace ninfer
