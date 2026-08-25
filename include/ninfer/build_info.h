#pragma once

#include <string>
#include <string_view>

namespace ninfer {

struct BuildInfo {
    std::string_view upstream_base_sha;
    std::string_view patch_stack_sha;
    std::string_view build_profile;
    std::string_view build_type;
    std::string_view cxx_compiler;
    std::string_view cuda_compiler;
    std::string_view cuda_toolkit;
    bool source_dirty = false;
};

[[nodiscard]] BuildInfo build_info() noexcept;
[[nodiscard]] std::string format_build_info(std::string_view program_name);

} // namespace ninfer
