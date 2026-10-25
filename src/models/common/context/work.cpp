#include "models/common/context/work.h"
#include "core/paged_kv_cache.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>

namespace ninfer::models::context {

std::uint64_t elapsed_ns(Clock::time_point started) noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

std::int32_t checked_i32(std::uint32_t value, const char* label) {
    if (value > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::overflow_error(label);
    }
    return static_cast<std::int32_t>(value);
}

std::uint32_t kv_pages_for_frontier(std::uint32_t frontier) noexcept {
    return frontier == 0 ? 0U : 1U + (frontier - 1U) / static_cast<std::uint32_t>(kPagedKVPageSize);
}

runtime::ContextTransferRequirement
state_transfer_requirement(TransferWork work, runtime::ContextTransferDirection direction) {
    return runtime::ContextTransferRequirement{
        .resource    = {},
        .direction   = direction,
        .image_count = 1,
        .page_groups = 0,
        .work        = work,
    };
}

runtime::ContextTransferRequirement
kv_transfer_requirement(runtime::ContextTransferResource resource,
                        runtime::ContextTransferDirection direction, std::uint32_t pages,
                        TransferWork work) {
    return runtime::ContextTransferRequirement{
        .resource    = resource,
        .direction   = direction,
        .page_groups = pages,
        .work        = work,
    };
}

} // namespace ninfer::models::context
