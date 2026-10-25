#pragma once

#include "runtime/engine/model_instance.h"
#include "runtime/engine/context_cache/context_cost.h"
#include "models/qwen3_5/program/runtime_types.h"
#include "models/qwen4_exp/program/runtime_types.h"

#include <variant>

namespace ninfer::runtime {

[[nodiscard]] EngineOptions normalize_engine_options(EngineOptions options);

// Finite architecture dispatch ends at Engine startup; inner execution remains statically typed.
using ActiveModel = std::variant<std::unique_ptr<ModelInstance<models::qwen3_5::RuntimeTypes>>,
                                 std::unique_ptr<ModelInstance<models::qwen4_exp::RuntimeTypes>>>;

struct ConstructedModel {
    ActiveModel instance;
    LoadSummary load;
    ContextMachineCostModel context_cost;
};

[[nodiscard]] ConstructedModel construct_model(EngineOptions&, DeviceContext&);

} // namespace ninfer::runtime
