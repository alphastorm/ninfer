#include "runtime/engine/prefill_pacing.h"
#include "runtime/engine/scheduler.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Request { using SequenceHandle = std::uint64_t; };
using Scheduler = ninfer::runtime::Scheduler<Request>;
using ninfer::runtime::ContextMachineCostModel;
using ninfer::runtime::PrefillPacing;

ContextMachineCostModel costs() {
    ContextMachineCostModel model;
    constexpr std::uint64_t q32 = std::uint64_t{1} << 32;
    model.prefill.chunk_ns = 1'000'000;
    model.prefill.token_ns_q32 = 100'000 * q32;
    model.prefill.attention_pair_ns_q32 = 2 * q32;
    return model;
}

void budget_policy() {
    const auto model = costs();
    PrefillPacing disabled(0, model);
    require(disabled.cap(0, 4096) == 0, "disabled budget changed the planned chunk");
    require(PrefillPacing(1000, model).cap(0, 2048) == 0,
            "a fitting chunk was split");
    require(PrefillPacing(60, model).cap(0, 2048) == 512,
            "cap is not the largest affordable aligned chunk");
    require(PrefillPacing(120, model).cap(0, 4096) == 1152,
            "shallow attention estimate is wrong");
    require(PrefillPacing(120, model).cap(100000, 4096) == 384,
            "deep attention did not reduce the cap");
    require(PrefillPacing(1, model).cap(0, 4096) == 128,
            "an unachievable budget did not preserve minimum progress");
    require(PrefillPacing(1, model).cap(0, 100) == 0,
            "a sub-alignment tail was cut or enlarged");
    require(PrefillPacing(1, model).cap(0, 0) == 0, "empty prefill acquired work");
    require(PrefillPacing(250, model).cap(std::numeric_limits<std::uint64_t>::max(), 4096) == 128,
            "overflowing attention estimate wrapped into a fitting chunk");
    PrefillPacing corrected(120, model);
    const auto expected = model.prefill_ns(ninfer::runtime::make_prefill_work(0, 1024, 0, 0, 1024));
    for (int step = 0; step != 32; ++step) corrected.observe(0, 1024, 2 * expected);
    require(corrected.cap(0, 4096) == 512, "calibration did not adapt to a slower card");
    const auto cap = corrected.cap(0, 4096);
    corrected.observe(0, 0, 100);
    corrected.observe(0, 1024, 0);
    require(corrected.cap(0, 4096) == cap, "empty observations changed calibration");
    require(corrected.over_budget(120'000'001) && !corrected.over_budget(120'000'000),
            "budget overrun boundary is not exact");
    require(!disabled.over_budget(999'000'000), "disabled pacing reported an overrun");
}

void cadence_policy() {
    Scheduler scheduler;
    require(scheduler.prefill_due(true, 4096, 4096, 0), "disabled cadence delayed prefill");
    require(scheduler.prefill_due(false, 4096, 4096, 8), "idle decode stalled prefill");
    for (unsigned round = 1; round <= 8; ++round) {
        scheduler.decode_executed();
        require(scheduler.prefill_due(true, 4096, 4096, 8) == (round == 8),
                "cadence did not count executed rounds");
    }
    scheduler.prefill_executed(0, 2);
    require(!scheduler.prefill_due(true, 4096, 4096, 8), "prefill did not reset decode credit");
    scheduler.decode_executed();
    require(scheduler.prefill_due(true, 128, 4096, 8), "a short arrival waited a full chunk cadence");
    require(scheduler.prefill_due(true, 512, 4096, 8), "a capped step did not scale cadence");
    require(!scheduler.prefill_due(true, 513, 4096, 8), "scaled cadence rounded down");
}
} // namespace

int main() {
    try {
        budget_policy();
        cadence_policy();
        std::cout << "prefill pacing policy passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
