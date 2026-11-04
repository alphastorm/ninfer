#pragma once

#include "ninfer/types.h"
#include <nlohmann/json.hpp>

namespace ninfer::product {
inline nlohmann::json dflash_trace_json(const DFlashTrace& trace) {
    nlohmann::json records = nlohmann::json::array();
    for (const auto& r : trace.rounds) {
        records.push_back({
            {"ordinal", r.ordinal}, {"frontier", r.frontier}, {"physical_k", r.physical_k},
            {"extent", r.extent}, {"previous_append", r.previous_append}, {"batch", r.batch},
            {"row", r.row}, {"lane", r.lane}, {"graph_class", r.graph_class},
            {"graph_envelope", r.graph_envelope}, {"accepted", r.accepted},
            {"licensed", r.licensed}, {"committed", r.committed},
            {"output_offset", r.output_offset}, {"graph_replay", r.graph_replay},
            {"budget_clipped", r.budget_clipped}, {"capacity_clipped", r.capacity_clipped},
            {"terminal", r.terminal}, {"discarded", r.discarded}, {"armed", r.armed},
            {"capped", r.capped}});
    }
    return {{"schema_version", 1}, {"request_sha256", trace.request_sha256},
            {"policy", {{"mode", static_cast<unsigned>(trace.options.mode)},
                        {"rounds", trace.options.rounds}, {"threshold", trace.options.threshold},
                        {"cap", trace.options.cap}}},
            {"prompt_token_ids", trace.prompt_token_ids}, {"output_token_ids", trace.output_token_ids},
            {"rounds", std::move(records)}};
}
} // namespace ninfer::product
