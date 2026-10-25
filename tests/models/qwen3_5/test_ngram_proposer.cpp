#include "models/qwen3_5/program/speculative/ngram_proposer.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <span>
#include <string_view>
#include <vector>

namespace {
using ninfer::TokenId;
using ninfer::models::qwen3_5::detail::NgramProposer;
using ninfer::models::qwen3_5::detail::NgramProposal;
using ninfer::models::qwen3_5::detail::ngram_copy_admitted;

int failures = 0;
void expect(bool condition, std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::vector<TokenId> fixture(std::uint32_t source_tokens = 80) {
    std::vector<TokenId> ledger(source_tokens);
    std::iota(ledger.begin(), ledger.end(), 100);
    ledger.push_back(-1);
    for (TokenId token = 100; token < 116; ++token) ledger.push_back(token);
    return ledger;
}

// Independent exhaustive safety oracle: a returned proposal must follow the exact
// committed suffix somewhere earlier in this same ledger, with two source tokens
// beyond the copy. Hash buckets, insertion order and implementation widths are unused.
bool backed_by_prefix(std::span<const TokenId> ledger, const NgramProposal& proposal) {
    if (!proposal.count) return true;
    if (proposal.matched > ledger.size() || proposal.count > proposal.tokens.size()) return false;
    const auto tail = ledger.last(proposal.matched);
    for (std::size_t end = proposal.matched; end + proposal.count + 2 <= ledger.size(); ++end) {
        const auto match = ledger.subspan(end - proposal.matched, proposal.matched);
        if (!std::equal(match.begin(), match.end(), tail.begin())) continue;
        const auto copy = ledger.subspan(end, proposal.count + 2);
        if (std::any_of(copy.begin(), copy.end(), [](TokenId t) { return t < 0; })) continue;
        if (std::equal(proposal.tokens.begin(), proposal.tokens.begin() + proposal.count,
                       copy.begin())) return true;
    }
    return false;
}
} // namespace

int main() {
    {
        NgramProposer proposer(128);
        const auto ledger = fixture();
        const auto copy = proposer.propose(ledger, 15, 7);
        expect(copy.count == 15 && copy.matched == 16, "full copy after a sixteen-token suffix");
        for (std::uint32_t i = 0; i < copy.count; ++i)
            expect(copy.tokens[i] == 116 + static_cast<TokenId>(i), "copy follows earlier match");
        expect(backed_by_prefix(ledger, copy), "full copy independently prefix-backed");
        expect(proposer.propose(ledger, 0, 7).count == 0, "disabled proposer emits nothing");
        expect(ngram_copy_admitted(1, copy) && ngram_copy_admitted(2, copy), "one/two lane admission");
        for (std::uint32_t lanes = 3; lanes <= 8; ++lanes)
            expect(!ngram_copy_admitted(lanes, copy), "three or more lanes never copy");
    }
    {
        NgramProposer proposer(128);
        auto ledger = fixture(26);
        auto copy = proposer.propose(ledger, 15, 7);
        expect(copy.count == 8, "short source leaves bonus and boundary tokens");
        expect(!ngram_copy_admitted(2, copy), "short copy cannot widen partner lane");
        ledger = fixture(25);
        expect(proposer.propose(ledger, 15, 7).count == 0, "neural-sized short copy is declined");
    }
    {
        NgramProposer proposer(256);
        auto ledger = fixture();
        auto copy = proposer.propose(ledger, 15, 7);
        ledger.insert(ledger.end(), copy.tokens.begin(), copy.tokens.begin() + copy.count);
        copy = proposer.propose(ledger, 15, 7);
        expect(copy.count == 15 && copy.tokens[0] == 131, "committed append advances the copy");
        expect(backed_by_prefix(ledger, copy), "incremental proposal uses committed source");
        ledger.resize(4);
        ledger.insert(ledger.end(), {700, 701, 702, 703});
        expect(proposer.propose(ledger, 15, 7).count == 0, "rewind discards abandoned offsets");
        NgramProposer other_request(256);
        expect(other_request.propose(std::span<const TokenId>(ledger).last(4), 15, 7).count == 0,
               "different request has no inherited source");
    }
    {
        NgramProposer proposer(128);
        auto ledger = fixture();
        ledger[20] = -1;
        expect(proposer.propose(ledger, 15, 7).count == 0, "copy stops at a nontext boundary");
        expect(proposer.propose({}, 15, 7).count == 0, "empty history is safe after rewind");
        bool invalid = false;
        try { (void)proposer.propose({}, 16, 7); }
        catch (const std::invalid_argument&) { invalid = true; }
        expect(invalid, "unsupported width is rejected");
    }
    {
        // A small table deliberately overwrites and collides. Every answer still
        // needs exact prefix equality, never merely a hash or a stale position.
        NgramProposer proposer(64);
        std::vector<TokenId> ledger;
        std::uint32_t random = 17;
        std::uint32_t proposals = 0;
        for (int round = 0; round < 100; ++round) {
            for (int i = 0; i < 50; ++i) {
                random = random * 1664525U + 1013904223U;
                ledger.push_back(static_cast<TokenId>(random % 100000U));
            }
            const auto source = ledger.size() - 50;
            ledger.push_back(-1);
            for (std::size_t i = 0; i < 16; ++i) ledger.push_back(ledger[source + i]);
            const auto copy = proposer.propose(ledger, 15, 7);
            proposals += copy.count != 0;
            expect(backed_by_prefix(ledger, copy), "colliding buckets cannot invent a source");
        }
        expect(proposals != 0, "collision fixture exercises successful proposals");
    }
    if (!failures) std::cout << "ok\n";
    return failures ? 1 : 0;
}
