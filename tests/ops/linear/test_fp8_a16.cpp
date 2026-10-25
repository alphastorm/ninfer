#include "core/weight.h"
#include "cuda_fixture.h"
#include "core/buffer.h"
#include "ops/linear/linear_test_common.h"

#include <array>
#include <exception>
#include <iostream>
#include <utility>
#include <vector>

namespace {

using namespace ninfer;
using namespace ninfer::test::linear;

std::vector<Invocation> a16_capacity_calls() {
    std::vector<Invocation> calls;
    for (int t = 1; t <= 33; ++t) calls.push_back({t});
    for (int t : {63, 64, 65, 95, 96, 97, 127, 128, 129, 512, 1024}) calls.push_back({t});
    for (int t : {3, 7, 11, 15, 19, 23, 25, 33})
        calls.push_back({t, CallForm::Policy, ops::LinearPolicy::A16Only, true});
    calls.push_back({1, CallForm::A16Convenience});
    for (int t : {64, 97, 129})
        calls.push_back({t, CallForm::Policy, ops::LinearPolicy::A16Only, true});
    return calls;
}

int run_fp8_a16() {
    const auto attn_invocations = a16_capacity_calls();
    int failures                = run_shape("FP8_A16", ActivationCompute::A16, make_fp8_weight,
                                            {14336, 5120, 811U, Comparison::Sampled, true, attn_invocations});
    const auto gdn_invocations  = a16_capacity_calls();
    failures += run_shape("FP8_A16", ActivationCompute::A16, make_fp8_weight,
                          {16384, 5120, 817U, Comparison::Sampled, true, gdn_invocations});
    const auto mlp_invocations = a16_capacity_calls();
    failures += run_shape("FP8_A16", ActivationCompute::A16, make_fp8_weight,
                          {34816, 5120, 821U, Comparison::Sampled, true, mlp_invocations});
    std::vector<Invocation> vocabulary_invocations{
        Invocation{1, CallForm::A16Convenience, ops::LinearPolicy::A16Only},
        Invocation{8, CallForm::Policy, ops::LinearPolicy::AllowA8},
        Invocation{9, CallForm::Policy, ops::LinearPolicy::AllowA4},
        Invocation{24, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{25, CallForm::Policy, ops::LinearPolicy::AllowA8},
        Invocation{41, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{42, CallForm::Policy, ops::LinearPolicy::AllowA8},
        Invocation{48, CallForm::Policy, ops::LinearPolicy::AllowA4},
        Invocation{49, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{64, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{65, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{96, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{97, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{128, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{129, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{160, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{161, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{192, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{193, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{256, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{257, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{288, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{289, CallForm::Policy, ops::LinearPolicy::A16Only},
        Invocation{1024, CallForm::Policy, ops::LinearPolicy::AllowA4},
    };
    for (int t = 1; t <= 41; ++t)
        vocabulary_invocations.push_back({t, CallForm::Policy, ops::LinearPolicy::A16Only});
    for (int t : {7, 25, 41, 65, 128})
        vocabulary_invocations.push_back({t, CallForm::Policy, ops::LinearPolicy::A16Only, true});
    failures += run_shape("FP8_A16", ActivationCompute::A16, make_fp8_weight,
                          {248320, 5120, 823U, Comparison::Sampled, true, vocabulary_invocations});
    constexpr std::array vocabulary_policies{
        ops::LinearPolicy::A16Only,
        ops::LinearPolicy::AllowA8,
        ops::LinearPolicy::AllowA4,
    };
    for (const ops::LinearPolicy policy : vocabulary_policies) {
        try {
            const std::size_t capacity = ops::linear_workspace_capacity_bytes(
                QType::FP8_E4M3FN_ROW_BF16, 248320, 5120, policy, 1, 2048);
            if (capacity != 0) {
                std::cerr << "FP8 vocabulary A16 route reported nonzero workspace\n";
                ++failures;
            }
        } catch (const std::exception& error) {
            std::cerr << "FP8 vocabulary policy was rejected: " << error.what() << '\n';
            ++failures;
        }
    }
    const auto residual6144_invocations = a16_capacity_calls();
    failures += run_shape("FP8_A16", ActivationCompute::A16, make_fp8_weight,
                          {5120, 6144, 827U, Comparison::Sampled, true, residual6144_invocations});
    const auto residual17408_invocations = a16_capacity_calls();
    failures +=
        run_shape("FP8_A16", ActivationCompute::A16, make_fp8_weight,
                  {5120, 17408, 829U, Comparison::Sampled, true, residual17408_invocations});

    auto packed = make_fp8_weight(14336, 5120, 831U);
    DeviceBuffer device_weight(ninfer::test::cuda_environment().context.device(),
                               packed.payload.size());
    device_weight.copy_from_host(packed.payload.data(), device_weight.size());
    const Weight weight = packed.device_weight(device_weight.span());
    const std::vector<std::uint16_t> input_bits(5120, 0);
    auto device_input = test::to_device(input_bits);
    test::GuardedDeviceBuffer device_output(14336 * sizeof(std::uint16_t));
    device_output.fill(0xff);
    Tensor input(device_input.span(), DType::BF16, {5120, 1});
    Tensor output(device_output.span(), DType::BF16, {14336, 1});
    ninfer::DeviceBuffer workspace_storage(ninfer::test::cuda_environment().context.device(), 256);
    MemoryArena workspace(workspace_storage.span());
    const auto invoke = [&](const Weight& candidate) {
        ops::linear(input, candidate, output, ops::LinearPolicy::A16Only, workspace,
                    ninfer::test::cuda_environment().default_execution);
    };
    try {
        invoke(weight);
        test::cuda_synchronize();
    } catch (const std::exception& error) {
        std::cerr << "valid FP8 metadata was rejected: " << error.what() << '\n';
        ++failures;
    }
    const auto expect_invalid = [&](const char* label, Weight invalid) {
        try {
            invoke(invalid);
            test::cuda_synchronize();
            std::cerr << "invalid FP8 " << label << " was accepted\n";
            ++failures;
        } catch (const std::invalid_argument&) {}
    };
    Weight invalid = weight;
    invalid.layout = QuantLayout::Contiguous;
    expect_invalid("layout", invalid);
    invalid             = weight;
    invalid.scale_nb[1] = invalid.scale_nb[1] - 2;
    expect_invalid("scale stride", invalid);
    invalid               = weight;
    invalid.payload_bytes = invalid.payload_bytes - 1;
    expect_invalid("payload bound", invalid);
    failures += device_output.verify_guards("FP8 metadata output");
    failures += test::linear_common::verify_preserved(
        device_input.data(), std::as_bytes(std::span(input_bits)), "FP8 metadata input");
    failures += test::linear_common::verify_preserved(
        device_weight.data(), std::as_bytes(std::span(packed.payload)), "FP8 metadata weight");
    for (auto [n, k] : {std::pair{14336, 5120}, std::pair{16384, 5120}, std::pair{34816, 5120},
                        std::pair{248320, 5120}, std::pair{5120, 6144}, std::pair{5120, 17408}}) {
        failures += verify_workspace_envelopes(QType::FP8_E4M3FN_ROW_BF16, n, k);
    }
    return failures;
}

} // namespace

int main() {
    return ninfer::test::linear_common::run_test_main("FP8 A16 Linear",
                                                      [] { return run_fp8_a16(); });
}
