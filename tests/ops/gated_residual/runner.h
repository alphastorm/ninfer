#pragma once
#include "fixtures.h"

namespace ninfer::test::hc {
int verify(const Fixture& fixture, const std::string& label, bool exact_residual);
int run_suite(Form form);
int validate(Form form, CudaExecutionView execution);
} // namespace ninfer::test::hc
