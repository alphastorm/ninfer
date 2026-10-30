#include "ninfer/types.h"

#include <cstdint>
#include <iostream>
#include <numeric>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

void check(bool condition, std::string_view message) {
    if (condition) { return; }
    std::cerr << message << '\n';
    ++failures;
}

void check_sums(const ninfer::SpeculativeStats& stats) {
    check(std::accumulate(stats.attempted_per_position.begin(),
                          stats.attempted_per_position.end(), std::uint64_t{0}) ==
              stats.drafted_tokens,
          "position attempts must sum to native drafted tokens");
    check(std::accumulate(stats.accepted_per_position.begin(),
                          stats.accepted_per_position.end(), std::uint64_t{0}) ==
              stats.accepted_tokens,
          "position accepts must sum to native accepted tokens");
    for (std::size_t i = 0; i < stats.attempted_per_position.size(); ++i) {
        check(stats.accepted_per_position[i] <= stats.attempted_per_position[i],
              "a position cannot accept more often than it was attempted");
    }
}

void check_rounds(ninfer::SpeculativeBackend backend, std::uint32_t k) {
    ninfer::SpeculativeStats stats{
        .backend = backend,
        .enabled = true,
        .draft_window = k,
        .attempted_per_position = std::vector<std::uint64_t>(k),
        .accepted_per_position = std::vector<std::uint64_t>(k),
    };
    const auto* attempts = stats.attempted_per_position.data();
    const auto* accepts  = stats.accepted_per_position.data();
    // A fallback has no draft positions, including the first MTP decode after a restore.
    stats.observe_round(0, 0);
    check(stats.rounds == 0 && stats.fallback_steps == 1,
          "fallback must not fabricate a speculative verification round");
    check_sums(stats);
    // Every proposed position counts as attempted even when position zero rejects.
    stats.observe_round(k, 0);
    check(stats.attempted_per_position == std::vector<std::uint64_t>(k, 1) &&
              stats.accepted_per_position == std::vector<std::uint64_t>(k, 0),
          "position-zero rejection must retain the complete attempted window");
    check_sums(stats);
    stats.observe_round(k, k);
    check_sums(stats);
    // The output/context budget can shorten the last round independently of configured K.
    stats.observe_round(2, 1);
    stats.observe_round(1, 0);
    stats.observe_round(0, 0);
    check(stats.rounds == 4 && stats.fallback_steps == 2 &&
              stats.drafted_tokens == 2 * k + 3 && stats.accepted_tokens == k + 1,
          "partial and fallback rounds must preserve native aggregate semantics");
    for (std::uint32_t i = 0; i < k; ++i) {
        check(stats.attempted_per_position[i] == 2U + (i < 2) + (i < 1) &&
                  stats.accepted_per_position[i] == 1U + (i == 0),
              "only the attempted and accepted prefixes may increment");
    }
    check(stats.attempted_per_position.data() == attempts &&
              stats.accepted_per_position.data() == accepts,
          "commit observation must not allocate or resize position storage");
    check_sums(stats);
}
} // namespace

int main() {
    check_rounds(ninfer::SpeculativeBackend::Mtp, 3);
    check_rounds(ninfer::SpeculativeBackend::DFlash2, 7);
    return failures ? 1 : 0;
}
