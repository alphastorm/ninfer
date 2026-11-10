#include "models/common/context/recovery_loss.h"

#include <array>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Handle {
    std::uint32_t index = UINT32_MAX;
    std::uint64_t generation = 1;
    static inline std::uint64_t comparisons = 0;
    friend bool operator==(Handle left, Handle right) {
        ++comparisons;
        return left.index == right.index && left.generation == right.generation;
    }
};
struct View {
    struct Record {
        std::vector<std::uint32_t> tokens;
        std::uint32_t domain = 0;
    };
    std::vector<Record> records;
    Handle add(std::vector<std::uint32_t> tokens, std::uint32_t domain = 0) {
        records.push_back({std::move(tokens), domain});
        return {static_cast<std::uint32_t>(records.size() - 1)};
    }
    bool valid(Handle h) const { return h.index < records.size() && h.generation == 1; }
    std::uint32_t frontier(Handle h) const { return records.at(h.index).tokens.size(); }
    bool prefix(Handle a, Handle b) const {
        const auto& x = records.at(a.index);
        const auto& y = records.at(b.index);
        return x.domain == y.domain && x.tokens.size() <= y.tokens.size() &&
               std::equal(x.tokens.begin(), x.tokens.end(), y.tokens.begin());
    }
    std::uint64_t loss(std::span<const Handle> removed, std::span<const Handle> kept) const {
        return ninfer::models::context::branch_recovery_loss(
            removed, kept, [&](auto h) { return valid(h); },
            [&](auto h) { return frontier(h); }, [&](auto a, auto b) { return prefix(a, b); });
    }
};
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void agreement_cases() {
    View view;
    const auto root = view.add(std::vector<std::uint32_t>(100, 1));
    auto tokens = view.records[root.index].tokens;
    tokens.resize(200, 2);
    const auto deep = view.add(tokens);
    tokens.resize(150);
    const auto middle = view.add(tokens);
    const std::array chain{root, deep};
    const std::array kept{middle};
    require(view.loss(chain, kept) == 100, "intermediate fallback hid a shallow recovery point");
    const auto alias = view.add(view.records[deep.index].tokens);
    const std::array one{deep}, alias_kept{alias};
    require(view.loss(one, alias_kept) == 0, "surviving alias was not a complete fallback");
    const std::array repeated{deep, deep, Handle{}, Handle{deep.index, 2}};
    require(view.loss(repeated, {}) == 200, "duplicate or stale handles changed loss");
    const auto independent = view.add(std::vector<std::uint32_t>(40, 8));
    const std::array branches{deep, independent};
    require(view.loss(branches, {}) == 240, "independent branches did not add");
}

// Independent exhaustive definition from upstream 452c3f5f: each maximal removed branch
// pays its worst lost recovery point, including shared ancestors.
std::uint64_t oracle(const View& view, std::span<const Handle> input,
                     std::span<const Handle> advertised) {
    std::vector<Handle> removed;
    for (const auto h : input)
        if (view.valid(h) && std::find(removed.begin(), removed.end(), h) == removed.end())
            removed.push_back(h);
    std::uint64_t result = 0;
    for (const auto branch : removed) {
        if (std::any_of(removed.begin(), removed.end(), [&](auto other) {
                return other != branch && view.prefix(branch, other) &&
                       (view.frontier(branch) < view.frontier(other) || other.index < branch.index);
            }))
            continue;
        std::uint32_t worst = 0;
        for (const auto point : removed) {
            if (!view.prefix(point, branch)) continue;
            std::uint32_t restored = 0;
            for (const auto kept : advertised)
                if (view.valid(kept) &&
                    std::find(removed.begin(), removed.end(), kept) == removed.end() &&
                    view.prefix(kept, point))
                    restored = std::max(restored, view.frontier(kept));
            worst = std::max(worst, view.frontier(point) - restored);
        }
        result += worst;
    }
    return result;
}

void shared_ancestor_losses() {
    for (bool reverse : {false, true}) {
        View view;
        const auto root = view.add(std::vector<std::uint32_t>(100, 1));
        auto left = view.records[root.index].tokens, right = left;
        left.resize(200, 2);
        right.resize(200, 3);
        const auto a = view.add(reverse ? right : left), b = view.add(reverse ? left : right);
        left.resize(150);
        right.resize(150);
        const auto x = view.add(left), y = view.add(right);
        std::array removed{root, a, b};
        const std::array both{x, y};
        const std::array one{x};
        const auto actual = view.loss(removed, both);
        if (actual != 200)
            throw std::runtime_error("shared-ancestor loss: expected 200, got " + std::to_string(actual));
        require(view.loss(removed, one) == 300, "one surviving branch hid shared recovery loss");
        std::reverse(removed.begin(), removed.end());
        require(view.loss(removed, both) == 200, "input order changed shared-ancestor loss");
        const std::array chain{root, reverse ? b : a};
        require(view.loss(chain, one) == 100, "chain charged more than its worst lost ancestor");
    }
}

void random_oracle() {
    std::mt19937 random(20261019);
    for (unsigned fixture = 0; fixture < 80; ++fixture) {
        View view;
        std::vector<Handle> handles;
        for (unsigned i = 0; i < 32; ++i) {
            std::vector<std::uint32_t> tokens;
            if (i && random() % 3) tokens = view.records[handles[random() % i].index].tokens;
            for (auto add = random() % 5; add; --add) tokens.push_back(random() % 8);
            handles.push_back(view.add(std::move(tokens), random() % 3));
        }
        for (unsigned query = 0; query < 50; ++query) {
            std::vector<Handle> removed, kept;
            for (const auto h : handles) {
                if (random() % 2) {
                    removed.push_back(h);
                    if (random() % 3 == 0) removed.push_back(h);
                }
                if (random() % 3) kept.push_back(h);
            }
            removed.push_back({});
            kept.push_back({});
            const auto expected = oracle(view, removed, kept);
            require(view.loss(removed, kept) == expected, "score differs from exhaustive oracle");
            std::shuffle(removed.begin(), removed.end(), random);
            require(view.loss(removed, kept) == expected, "permutation changed recovery loss");
        }
    }
}

void hash_preserves_generation() {
    const std::array removed{Handle{7, 1}};
    const std::array surviving{Handle{7, (1ULL << 40U) + 1}};
    const auto frontier = [](Handle h) { return h.generation == 1 ? 10U : 4U; };
    const auto score = ninfer::models::context::branch_recovery_loss<Handle>(
        removed, surviving, [](auto) { return true; }, frontier,
        [&](auto left, auto right) { return frontier(left) <= frontier(right); });
    require(score == 6, "membership conflated distinct checkpoint generations");
}

void survivor_membership_scale() {
    for (const std::uint32_t count : {32U, 128U, 512U}) {
        std::vector<Handle> removed, surviving;
        for (std::uint32_t i = 0; i < count; ++i) removed.push_back({i});
        for (std::uint32_t i = 0; i < 8 * count; ++i) surviving.push_back({i});
        Handle::comparisons = 0;
        std::uint64_t valid_calls = 0, frontier_calls = 0;
        const auto start = std::chrono::steady_clock::now();
        const auto score = ninfer::models::context::branch_recovery_loss<Handle>(
            removed, surviving, [&](auto) { ++valid_calls; return true; },
            [&](auto) { ++frontier_calls; return 1U; },
            [](auto, auto) { return false; });
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto limit = 4ULL * count * (removed.size() + surviving.size());
        std::cout << "removed=" << count << " surviving=" << surviving.size()
                  << " score=" << score << " handle_comparisons=" << Handle::comparisons
                  << " limit=" << limit << " valid_calls=" << valid_calls
                  << " frontier_calls=" << frontier_calls << " seconds=" << elapsed << '\n';
        require(score == count, "scale fixture changed recovery loss");
        // Count equality operations, not elapsed time: the remaining prefix/lineage work is
        // quadratic, but membership must not add another scan of every removed checkpoint.
        require(Handle::comparisons <= limit,
                "survivor membership still scans every removed handle");
        require(valid_calls + frontier_calls <= 4ULL * (removed.size() + surviving.size()),
                "survivor validity/frontier facts were not cached once");
    }
}
} // namespace

int main() {
    try {
        agreement_cases();
        shared_ancestor_losses();
        random_oracle();
        hash_preserves_generation();
        survivor_membership_scale();
        std::cout << "ok\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
