#include "models/common/context/recovery_loss.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
struct Handle {
    std::uint32_t index = UINT32_MAX, generation = 1;
    friend bool operator==(Handle, Handle) = default;
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
} // namespace

int main() {
    try {
        agreement_cases();
        std::cout << "ok\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
