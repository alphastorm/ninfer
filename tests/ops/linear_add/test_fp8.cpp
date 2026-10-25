#include "ops/linear_add/linear_add_test_common.h"
#include "ops/linear_common.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace ninfer;
using namespace ninfer::test::linear_add;

struct Profile {
    std::int32_t n;
    std::int32_t k;
    std::int32_t first_a8;
    std::uint32_t seed;
};

int run_profile(const Profile& profile) {
    constexpr std::array a16_graph_tokens{4, 128};
    constexpr std::array a8_graph_tokens{128, 193, 257, 512, 513, 1025};
    std::vector<Invocation> invocations;
    const auto add = [&](std::int32_t tokens, ops::LinearPolicy policy) {
        const auto graphs = policy == ops::LinearPolicy::A16Only
                                ? std::span<const int>(a16_graph_tokens)
                                : std::span<const int>(a8_graph_tokens);
        const bool replay = std::find(graphs.begin(), graphs.end(), tokens) != graphs.end();
        invocations.push_back(Invocation{
            .tokens     = tokens,
            .policy     = policy,
            .arithmetic = policy == ops::LinearPolicy::AllowA8 && tokens >= profile.first_a8
                              ? Arithmetic::A8
                              : Arithmetic::A16,
            .graph      = replay ? GraphReplay::OriginalNegative : GraphReplay::None,
        });
    };
    for (const int tokens : {1, 2, 26}) { add(tokens, ops::LinearPolicy::A16Only); }
    for (const int tokens :
         {profile.first_a8 - 1, profile.first_a8, 48, 65, 1024, 8, 16, 32, 64, 96, 128, 129}) {
        add(tokens, ops::LinearPolicy::AllowA8);
    }
    for (const int tokens : {17, 18, 191, 192, 193, 255, 256, 257, 383, 384, 385, 511, 512, 513,
                             767, 768, 769, 1023, 1025}) {
        add(tokens, ops::LinearPolicy::AllowA8);
    }
    for (int tokens = 2; tokens <= 24; ++tokens) { add(tokens, ops::LinearPolicy::A16Only); }
    for (const int tokens : {31, 32, 33, 63, 64, 65, 127, 128, 129, 1024}) {
        add(tokens, ops::LinearPolicy::A16Only);
    }
    int failures = run_cases("FP8 linear_add", WeightFormat::FP8,
                             InvocationCase{
                                 .n               = profile.n,
                                 .k               = profile.k,
                                 .seed            = profile.seed,
                                 .invocations     = invocations,
                                 .fixture         = FixturePattern::Hashed,
                                 .rows            = Samples::Uniform32,
                                 .columns         = Samples::Uniform32,
                                 .scan_all_finite = false,
                             });

    const auto capacity = [&](ops::LinearPolicy policy, std::int32_t begin, std::int32_t end) {
        return ops::linear_add_workspace_capacity_bytes(QType::FP8_E4M3FN_ROW_BF16, profile.n,
                                                        profile.k, policy, begin, end);
    };
    const auto a16_interval = capacity(ops::LinearPolicy::A16Only, 1, 2048);
    const auto pre_boundary = capacity(ops::LinearPolicy::AllowA8, 1, profile.first_a8 - 1);
    const auto hot_interval = capacity(ops::LinearPolicy::AllowA8, 1, 48);
    const auto exact_48     = capacity(ops::LinearPolicy::AllowA8, 48, 48);
    const auto through_1024 = capacity(ops::LinearPolicy::AllowA8, 1, 1024);
    const auto exact_1024   = capacity(ops::LinearPolicy::AllowA8, 1024, 1024);
    if (a16_interval != 0 || pre_boundary != 0 || hot_interval != exact_48 ||
        through_1024 != exact_1024 || exact_1024 <= exact_48) {
        std::cerr << "FP8 linear_add [" << profile.n << ',' << profile.k
                  << "]: workspace interval contract mismatch\n";
        ++failures;
    }
    return failures;
}

} // namespace

int main() {
    return ninfer::test::linear_common::run_test_main("FP8 linear_add", [] {
        return run_profile(Profile{.n = 5120, .k = 6144, .first_a8 = 17, .seed = 861U}) +
               run_profile(Profile{.n = 5120, .k = 17408, .first_a8 = 20, .seed = 863U});
    });
}
