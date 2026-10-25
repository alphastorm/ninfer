#pragma once

#include "models/common/moe/plan.h"
#include "runtime/engine/kv_capacity.h"

namespace ninfer::models {

struct WeightPlacement {
    std::optional<std::size_t> resident_bytes;
    std::size_t fixed_bytes = 0;
    std::optional<moe::Plan> offload;
    std::vector<ExpertBankCacheSpec> banks;
};

template <class SequencePlan>
struct ExecutionPlan {
    SequencePlan sequence;
    runtime::KvCapacityResolution capacity;
    bool host_experts   = false;
    std::uint32_t chunk = 0;
    std::shared_ptr<const moe::Plan> moe;
};

} // namespace ninfer::models
