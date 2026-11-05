#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace ninfer::models::context {

// Pure checkpoint scoring. Callers supply validity, frontier and exact compatible-prefix facts;
// the scorer acquires no resources or reader leases.
template <class Handle, class Valid, class Frontier, class Prefix>
[[nodiscard]] std::uint64_t branch_recovery_loss(std::span<const Handle> removed,
                                                std::span<const Handle> surviving,
                                                Valid&& valid, Frontier&& frontier,
                                                Prefix&& prefix) {
    std::vector<Handle> ordered;
    for (const auto handle : removed) {
        if (valid(handle) && std::find(ordered.begin(), ordered.end(), handle) == ordered.end())
            ordered.push_back(handle);
    }
    std::sort(ordered.begin(), ordered.end(), [&](auto left, auto right) {
        const auto l = frontier(left), r = frontier(right);
        return l != r ? l > r : left.index < right.index;
    });
    struct LineageLoss {
        Handle deepest;
        std::uint32_t tokens;
    };
    std::vector<LineageLoss> lineages;
    for (const auto handle : ordered) {
        std::uint32_t fallback = 0;
        for (const auto retained : surviving) {
            if (!valid(retained) ||
                std::find(ordered.begin(), ordered.end(), retained) != ordered.end())
                continue;
            if (frontier(retained) > fallback && prefix(retained, handle))
                fallback = frontier(retained);
        }
        const auto lost = frontier(handle) - fallback;
        const auto lineage = std::find_if(lineages.begin(), lineages.end(), [&](const auto& item) {
            return prefix(handle, item.deepest);
        });
        if (lineage == lineages.end())
            lineages.push_back({handle, lost});
        else
            lineage->tokens = std::max(lineage->tokens, lost);
    }
    std::uint64_t result = 0;
    for (const auto& lineage : lineages) result += lineage.tokens;
    return result;
}

} // namespace ninfer::models::context
