#pragma once

#include "ninfer/types.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace ninfer::models::qwen3_5::detail {

// Design reference: ashalliants/ninfer-3090 fdcc3494c and e5acb18fe (Apache-2.0),
// derived there from Infernix/remesis. This implementation indexes only the request's
// committed ledger, without a second token corpus or allocation during lookup.
struct NgramProposal {
    std::array<TokenId, 15> tokens{};
    std::uint32_t count = 0;
    std::uint32_t matched = 0;
};

class NgramProposer {
public:
    explicit NgramProposer(std::uint32_t capacity)
        : buckets_(std::bit_ceil(std::max<std::size_t>(capacity, 64))) {}

    // A rewind invalidates every offset. No token from another request, an abandoned
    // suffix, or a provisional round remains eligible after the rebuild.
    [[nodiscard]] NgramProposal propose(std::span<const TokenId> ledger,
                                         std::uint32_t maximum,
                                         std::uint32_t neural_drafts,
                                         std::uint32_t minimum_match = 12) {
        if (maximum > 15 || minimum_match < 4 || minimum_match > 64) {
            throw std::invalid_argument("invalid n-gram proposal geometry");
        }
        if (ledger.size() < indexed_) {
            std::fill(buckets_.begin(), buckets_.end(), Bucket{});
            indexed_ = 0;
        }
        for (; indexed_ < ledger.size(); ++indexed_) {
            const auto end = indexed_ + 1;
            for (const auto width : widths_) {
                if (end < width) continue;
                const auto window = ledger.subspan(end - width, width);
                if (std::any_of(window.begin(), window.end(), [](TokenId t) { return t < 0; }))
                    continue;
                auto& bucket = buckets_[hash(window) & (buckets_.size() - 1)];
                for (std::size_t slot = bucket.size() - 1; slot > 0; --slot)
                    bucket[slot] = bucket[slot - 1];
                bucket[0] = {static_cast<std::uint32_t>(end), width};
            }
        }
        NgramProposal best;
        if (maximum == 0) return best;
        const auto history = ledger.last(std::min<std::size_t>(64, ledger.size()));
        for (const auto width : widths_) {
            if (history.size() < width) continue;
            const auto tail = history.last(width);
            const auto& bucket = buckets_[hash(tail) & (buckets_.size() - 1)];
            for (const auto entry : bucket) {
                if (entry.width != width || entry.end < width || entry.end >= ledger.size())
                    continue;
                const auto source = ledger.subspan(entry.end - width, width);
                if (!std::equal(source.begin(), source.end(), tail.begin())) continue;
                std::uint32_t matched = width;
                while (matched < history.size() && entry.end > matched) {
                    const auto token = ledger[entry.end - matched - 1];
                    if (token < 0 || token != history[history.size() - matched - 1]) break;
                    ++matched;
                }
                if (matched < minimum_match || matched < best.matched) continue;
                std::uint32_t available = 0;
                while (available < maximum + 2 && entry.end + available < ledger.size() &&
                       ledger[entry.end + available] >= 0)
                    ++available;
                // Leave a bonus token and one boundary token in the known source.
                const auto count = std::min(maximum, available > 2 ? available - 2 : 0);
                if (count == 0 || (count < maximum && count <= neural_drafts)) continue;
                if (matched == best.matched && count <= best.count) continue;
                best.count = count;
                best.matched = matched;
                std::copy_n(ledger.begin() + entry.end, count, best.tokens.begin());
            }
        }
        return best;
    }

private:
    struct Entry {
        std::uint32_t end = 0;
        std::uint32_t width = 0;
    };
    using Bucket = std::array<Entry, 4>;
    static constexpr std::array<std::uint32_t, 3> widths_{16, 8, 4};
    std::vector<Bucket> buckets_;
    std::size_t indexed_ = 0;

    [[nodiscard]] static std::uint64_t hash(std::span<const TokenId> window) noexcept {
        std::uint64_t value = 1469598103934665603ULL ^ window.size();
        for (const auto token : window)
            value = (value ^ static_cast<std::uint32_t>(token)) * 1099511628211ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }
};

// Copy graphs exist only at one or two active lanes. At two lanes the widened
// target call needs at least twelve copied tokens to amortize its extra columns.
[[nodiscard]] inline bool ngram_copy_admitted(std::uint32_t lanes,
                                              const NgramProposal& proposal) noexcept {
    return lanes == 1 ? proposal.count != 0 : lanes == 2 && proposal.count >= 12;
}

} // namespace ninfer::models::qwen3_5::detail
