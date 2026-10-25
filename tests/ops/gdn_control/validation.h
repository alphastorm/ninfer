#pragma once

#include "fixtures.h"

namespace ninfer::test::gdn_control {
int validate(Profile profile, bool normalize_input, CudaExecutionView execution);
int validate_capacity(Profile profile, bool normalize_input);
} // namespace ninfer::test::gdn_control
