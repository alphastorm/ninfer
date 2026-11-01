#include "models/qwen3_5/program/speculative/extent_cap.h"
#include "models/qwen3_5/program/speculative/dflash_ingress.h"
#include "product/dflash_trace.h"

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace ninfer;
namespace spec = ninfer::models::qwen3_5::speculative;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
spec::Observation observation(std::uint32_t a, std::uint32_t p = 7) {
    return {.extent = p, .accepted = a, .licensed = a + 1, .committed = a + 1};
}
void governor() {
    DFlashOptions options{.mode = DFlashMode::Adaptive, .rounds = 8, .threshold = 20, .cap = 3};
    spec::ExtentCap state(options);
    for (int i = 0; i < 7; ++i) {
        state.commit(observation(2));
        require(!state.armed() && state.limit(7) == 7, "arming must wait for R commits");
    }
    auto ignored = observation(0);
    ignored.clipped = true;
    state.commit(ignored);
    ignored = observation(0);
    ignored.committed = 0;
    ignored.discarded = true;
    state.commit(ignored);
    ignored = observation(2);
    ignored.committed = 1;
    ignored.terminal = true;
    state.commit(ignored);
    require(!state.armed(), "clipped/discarded/partial rounds must not train");
    state.commit(observation(2));
    require(state.armed() && state.limit(7) == 3, "exact integer arming and first decision");
    state.commit(observation(3, 3));
    require(state.limit(7) == 7, "capped saturation must probe Long");
    state.commit(observation(4));
    require(state.limit(7) == 7, "high Long acceptance must retain Long");
    state.commit(observation(1));
    require(state.limit(7) == 3, "low Long acceptance must select cap");
    state.commit(observation(1, 3));
    require(state.limit(7) == 3, "unsaturated cap must remain capped");
    auto resumed = std::move(state);
    require(resumed.armed() && resumed.limit(7) == 3, "resume must preserve request state");
    state = spec::ExtentCap(options);
    require(!state.armed() && state.limit(7) == 7, "same-length rebinding must reset");
    spec::ExtentCap rolling(options);
    for (int i = 0; i < 8; ++i) rolling.commit(observation(7));
    for (int i = 0; i < 7; ++i) rolling.commit(observation(0));
    require(rolling.armed(), "ring must replace old observations");
    spec::ExtentCap cal({.mode = DFlashMode::Calibrate});
    for (int i = 0; i < 16; ++i) cal.commit(observation(7));
    require(cal.limit(7) == 3, "CAL starts after exactly 16 informative Long rounds");
    cal.commit(observation(0, 3));
    require(cal.limit(7) == 7, "CAL alternates cap and Long");
    cal.commit(observation(7));
    require(cal.limit(7) == 3, "CAL does not fit adaptive outcomes");
    spec::ExtentCap off;
    for (int i = 0; i < 64; ++i) off.commit(observation(0));
    require(!off.armed() && off.limit(7) == 7, "default must never arm");
}
void ingress() {
    using ninfer::models::qwen3_5::DFlashDecodeIngress;
    for (auto batch : {1U, 8U}) for (auto frontier : {0U, 1023U, 8191U, 32767U, 131070U}) {
        for (auto budget : {0U, 1U, 2U, 8U, 100U}) {
            DFlashDecodeIngress old{}, current{};
            std::memset(&old, 0x35, sizeof(old));
            std::memcpy(&current, &old, sizeof(old));
            for (std::uint32_t row = 0; row < batch; ++row) {
                spec::ExtentCap off;
                const auto legacy = std::min({7U, budget > 1 ? budget - 1 : 0U,
                                              131072U - frontier - 1});
                const auto plan = spec::plan_extent(off, 7, budget, 131072, frontier);
                require(plan.extent == legacy, "unarmed extent must equal N16 at every edge");
                old.proposal_valid_columns[row] = 8;
                old.proposal_extents[row] = legacy;
                old.target_valid_columns[row] = legacy + 1;
                for (std::uint32_t j = 0; j < 8; ++j)
                    old.target_rope_positions[row * 8 + j] = frontier + std::min(j, legacy) - 3;
                spec::write_extent_ingress(current, row, 7, frontier, -3, plan.extent);
            }
            require(std::memcmp(&old, &current, sizeof(old)) == 0,
                    "all ingress fields, offsets, unused bytes and lane pitch must be identical");
            spec::ExtentCap forced({.mode = DFlashMode::ForcedCap, .cap = 1});
            const auto cap = spec::plan_extent(forced, 7, budget, 131072, frontier);
            require(cap.extent == std::min(1U, static_cast<std::uint32_t>(old.proposal_extents[0])),
                    "forced cap must preserve budget/capacity clipping");
            spec::write_extent_ingress(current, 0, 7, frontier, -3, cap.extent);
            require(current.proposal_valid_columns[0] == 8, "physical proposal width stays eight");
            if (batch == 8)
                require(current.proposal_extents[7] == old.proposal_extents[7],
                        "armed neighbor must not affect unarmed lane");
        }
    }
}
DFlashTrace trace_fixture() {
    DFlashTrace trace;
    trace.options = {.mode = DFlashMode::Adaptive, .rounds = 8, .threshold = 20, .cap = 3};
    trace.request_sha256 = std::string(64, 'a');
    trace.prompt_token_ids = {11, 12};
    spec::ExtentCap state(trace.options);
    for (std::uint32_t i = 0; i < 16; ++i) {
        const auto p = state.limit(7);
        auto obs = observation(i == 9 ? p : 1, p);
        if (i == 14) { obs.terminal = true; obs.committed = 1; }
        if (i == 15) { obs.terminal = true; obs.discarded = true; obs.committed = 0; }
        DFlashRoundRecord record{.ordinal = i, .frontier = 2 + i * 2,
                                 .physical_k = 7, .extent = p, .previous_append = 2,
                                 .batch = 1, .row = 0, .lane = 3, .graph_class = 2,
                                 .graph_envelope = 8192, .accepted = obs.accepted,
                                 .licensed = obs.licensed, .committed = obs.committed,
                                 .terminal = obs.terminal, .discarded = obs.discarded,
                                 .armed = state.armed(), .capped = p < 7};
        const TokenId tokens[8] = {21, 22, 23, 24, 25, 26, 27, 28};
        spec::append_trace(trace, record, {tokens, obs.committed});
        state.commit(obs);
    }
    return trace;
}
void trace() {
    auto value = trace_fixture();
    auto json = product::dflash_trace_json(value);
    require(json.at("schema_version") == 1 && json.at("rounds").size() == 16,
            "trace must serialize ordered schema and records");
    require(json.at("request_sha256") == std::string(64, 'a') &&
                json.at("prompt_token_ids") == nlohmann::json({11, 12}) &&
                json.at("rounds").back().at("discarded") == true,
            "trace must bind request, exact prompt IDs and terminal discard");
    value.rounds.resize(spec::kTraceRoundLimit);
    bool refused = false;
    try { spec::append_trace(value, {}, {}); } catch (const std::length_error&) { refused = true; }
    require(refused, "trace round overflow must refuse incomplete evidence");
    value = {};
    value.output_token_ids.resize(spec::kTraceTokenLimit);
    const TokenId token = 1;
    refused = false;
    try { spec::append_trace(value, {}, {&token, 1}); }
    catch (const std::length_error&) { refused = true; }
    require(refused, "trace token overflow must refuse incomplete evidence");
}
}
int main(int argc, char**) {
    try {
        if (argc > 1) { std::cout << product::dflash_trace_json(trace_fixture()) << '\n'; return 0; }
        governor(); ingress(); trace();
        std::cout << "OK request-owned ExtentCap, commit binding, ingress identity and trace\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
