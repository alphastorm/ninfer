#pragma once

#include "ninfer/types.h"

namespace ninfer::runtime {

[[nodiscard]] EngineOptions normalize_engine_options(EngineOptions options);

} // namespace ninfer::runtime
