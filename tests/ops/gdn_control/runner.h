#pragma once

#include "fixtures.h"

namespace ninfer::test::gdn_control {
int run(Profile profile, Case test_case, bool normalize_input, CudaExecutionView execution);
} // namespace ninfer::test::gdn_control
