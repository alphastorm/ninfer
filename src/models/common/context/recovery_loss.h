#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

namespace ninfer::models::context {

// Pure checkpoint scoring. Callers supply validity, frontier and exact compatible-prefix facts;
// the scorer acquires no resources or reader leases.
template <class Handle, class Valid, class Frontier, class Prefix>
[[nodiscard]] std::uint64_t branch_recovery_loss(std::span<const Handle> removed,
                                                std::span<const Handle> surviving,
                                                Valid&& valid, Frontier&& frontier,
                                                Prefix&& prefix) {
    struct HandleHash {
        std::size_t operator()(const Handle& handle) const noexcept { return handle.index; }
    };
    std::unordered_set<Handle, HandleHash> removed_handles;
    removed_handles.reserve(removed.size());
    struct Point {
        Handle handle;
        std::uint32_t frontier;
    };
    std::vector<Point> ordered;
    ordered.reserve(removed.size());
    for (const auto handle : removed) {
        if (valid(handle) && removed_handles.insert(handle).second)
            ordered.push_back({handle, frontier(handle)});
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.frontier != right.frontier ? left.frontier > right.frontier
                                               : left.handle.index < right.handle.index;
    });
    if (ordered.empty()) return 0;
    std::vector<Point> survivors;
    survivors.reserve(surviving.size());
    for (const auto retained : surviving)
        if (valid(retained) && !removed_handles.contains(retained))
            survivors.push_back({retained, frontier(retained)});
    struct LineageLoss {
        Handle deepest;
        std::uint32_t tokens;
    };
    std::vector<LineageLoss> lineages;
    lineages.reserve(ordered.size());
    for (const auto& point : ordered) {
        const auto handle = point.handle;
        std::uint32_t fallback = 0;
        for (const auto& retained : survivors) {
            if (retained.frontier > fallback && prefix(retained.handle, handle))
                fallback = retained.frontier;
        }
        const auto lost = point.frontier - fallback;
        // Deeper nodes precede their ancestors. A shared removed ancestor contributes to
        // every maximal branch it prefixes, not merely the first matching branch.
        bool ancestor = false;
        for (auto& lineage : lineages) {
            if (!prefix(handle, lineage.deepest)) continue;
            lineage.tokens = std::max(lineage.tokens, lost);
            ancestor = true;
        }
        if (!ancestor)
            lineages.push_back({handle, lost});
    }
    std::uint64_t result = 0;
    for (const auto& lineage : lineages) result += lineage.tokens;
    return result;
}

} // namespace ninfer::models::context
