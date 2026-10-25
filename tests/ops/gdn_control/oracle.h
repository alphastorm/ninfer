#pragma once
#include "fixtures.h"

namespace ninfer::test::gdn_control {
struct Reference {
    std::vector<double> g, beta, normalized;
};

Reference projection_reference(const Fixture& fixture);
int verify_projection(std::string_view name, const Fixture& fixture, const Reference& reference);
double softplus(double value);
double sigmoid(double value);
} // namespace ninfer::test::gdn_control
