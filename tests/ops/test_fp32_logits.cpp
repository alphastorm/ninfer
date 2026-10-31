#include "cuda_fixture.h"
#include "ops/op_tester.h"
#include "ops/linear/linear_test_common.h"
#include "ninfer/ops/argmax.h"
#include "ninfer/ops/sampling.h"
#include "ninfer/ops/speculative_accept.h"
#include "ninfer/ops/target_logprobs.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

using namespace ninfer;
using namespace ninfer::test;

namespace {
int check(bool condition, const char* message) {
    if (condition) return 0;
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

int projections() {
    using namespace ninfer::test::linear;
    const WeightGenerator formats[]{make_bf16_weight, make_q4_g64_fp16_weight,
        make_q5_g64_fp16_weight, make_q6_g64_fp16_weight, make_q8_g32_fp16_weight,
        make_fp8_weight, make_nvfp4_weight};
    int failures = 0;
    for (const auto make : formats) {
        constexpr int n = 256, k = 5120;
        const auto packed = make(n, k, 713);
        auto device_weight = to_device(packed.payload);
        const Weight weight = packed.device_weight(device_weight.span());
        bool retained_fine_sum = false;
        for (int t : {1, 2, 7, 8, 9, 16, 17, 24, 32, 33, 65}) {
            std::vector<std::uint16_t> input(static_cast<std::size_t>(k) * t);
            for (std::size_t i = 0; i < input.size(); ++i)
                input[i] = f32_to_bf16(static_cast<float>(int((i * 7919 + 17) % 257) - 128) / 256.0f);
            auto device_input = to_device(input);
            GuardedDeviceBuffer output(static_cast<std::size_t>(n) * t * sizeof(float));
            output.fill(0xff);
            const Tensor x(device_input.span(), DType::BF16, {k, t});
            Tensor y(output.span(), DType::FP32, {n, t});
            ops::linear(x, weight, y, cuda_environment().default_execution);
            cuda_synchronize();
            const auto values = from_device<float>(output.data(), static_cast<std::size_t>(n) * t);
            std::vector<double> actual, expected;
            for (int col : {0, t / 2, t - 1}) {
                for (int row : {0, 1, 7, 17, 127, 255}) {
                    double sum = 0;
                    for (int i = 0; i < k; ++i)
                        sum += quantized_weight::logical_weight_fp64(packed, row, i) *
                               static_cast<double>(bf16_to_f32(input[static_cast<std::size_t>(col) * k + i]));
                    const float value = values[static_cast<std::size_t>(col) * n + row];
                    retained_fine_sum |= value != bf16_to_f32(f32_to_bf16(value));
                    actual.push_back(value);
                    expected.push_back(sum);
                }
            }
            // Existing A16 criterion, without relaxing it for the FP32 epilogue.
            failures += verify_reduction("FP32 linear FP64 oracle", actual, expected,
                                          {1.0 / 256, 1.0 / 256, 2.0 / 256});
            failures += output.verify_guards("FP32 linear output");
            failures += verify_exact("FP32 linear preserves activation",
                                      from_device<std::uint16_t>(device_input, input.size()), input);
        }
        failures += check(retained_fine_sum, "head must store sums finer than BF16");
    }
    return failures;
}

std::vector<float> near_ties(int rows, int domain, int columns) {
    std::vector<float> logits(static_cast<std::size_t>(rows) * columns, -8.0f);
    for (int column = 0; column < columns; ++column) {
        const auto base = static_cast<std::size_t>(column) * rows;
        logits[base + 17] = 8.0f;
        logits[base + 18] = 8.001f; // Both would round to 8 in BF16; lower id must not win.
        for (int row = domain; row < rows; ++row) logits[base + row] = 1000.0f;
    }
    return logits;
}

int readers() {
    int failures = 0;
    for (int domain : {128, 248077}) {
        const int rows = domain == 128 ? 128 : 248320;
        for (int batch : {1, 2, 17}) {
            const auto logits = near_ties(rows, domain, batch);
            auto device_logits = to_device(logits);
            auto device_output = to_device(std::vector<int>(batch, -1));
            Tensor input(device_logits.span(), DType::FP32, {rows, batch});
            Tensor output(device_output.span(), DType::I32, {batch});
            ops::argmax(input, output, domain, cuda_environment().default_execution);
            cuda_synchronize();
            failures += verify_exact("FP32 argmax near ties", from_device<int>(device_output, batch),
                                      std::vector<int>(batch, 18));
            auto device_positions = to_device(std::vector<int>(batch, 100));
            Tensor positions(device_positions.span(), DType::I32, {batch});
            const auto bytes = ops::sampling_workspace_capacity_bytes(domain, batch, batch);
            DeviceBuffer backing(cuda_environment().context.device(), std::max<std::size_t>(bytes, 256));
            MemoryArena workspace(backing.span());
            for (bool stochastic : {false, true}) {
                ops::SamplingConfig config;
                config.temperature = stochastic ? 1.0f : 0.0f;
                config.top_k = 1;
                config.presence_penalty = 0.0001f; // Exercise the adjusted-logit route too.
                auto configs = to_device(std::vector<ops::SamplingConfig>(batch, config));
                ops::sample(input, output, domain, static_cast<const ops::SamplingConfig*>(configs.data()),
                            positions, ops::kSamplePurposeDecode, workspace, cuda_environment().default_execution);
                cuda_synchronize();
                failures += verify_exact("FP32 sampler near ties", from_device<int>(device_output, batch),
                                          std::vector<int>(batch, 18));
            }
            auto targets = to_device(std::vector<int>(batch, 18));
            auto logprobs = to_device(std::vector<float>(batch, 0));
            Tensor target(targets.span(), DType::I32, {batch});
            Tensor logprob(logprobs.span(), DType::FP32, {batch});
            ops::target_logprobs(input, target, domain, logprob, cuda_environment().default_execution);
            cuda_synchronize();
            const double sum = std::exp(double(logits[17]) - logits[18]) + 1.0 +
                               (domain - 2) * std::exp(-8.0 - logits[18]);
            const auto values = from_device<float>(logprobs, batch);
            failures += verify_reduction("FP32 logprobs FP64 oracle",
                std::vector<double>(values.begin(), values.end()), std::vector<double>(batch, -std::log(sum)),
                {2.0e-5, 2.0e-4, 0});
            failures += verify_exact("FP32 consumers preserve logits",
                                      from_device<float>(device_logits, logits.size()), logits);
        }
    }
    return failures;
}

int acceptance(bool sparse, bool stochastic) {
    constexpr int k = 7, batch = 2, width = k + 1;
    const int rows = sparse ? 248320 : 128, domain = sparse ? 248077 : 128;
    auto logits = to_device(near_ties(rows, domain, width * batch));
    auto targets = to_device(std::vector<int>(width * batch, 18));
    auto drafts = to_device(std::vector<int>(k * batch, 17));
    auto extents = to_device(std::vector<int>(batch, k));
    auto lengths = to_device(std::vector<int>(batch, 100));
    auto anchors = to_device(std::vector<int>(batch, 17));
    auto licensed = to_device(std::vector<int>(width * batch, -1));
    auto counts = to_device(std::vector<int>(batch, -1));
    auto accepted = to_device(std::vector<int>(batch, -1));
    ops::SamplingConfig config;
    config.temperature = stochastic ? 1.0f : 0.0f;
    config.top_k = 1;
    config.presence_penalty = 0.0001f;
    auto configs = to_device(std::vector<ops::SamplingConfig>(batch, config));
    Tensor l(logits.span(), DType::FP32, {rows, width, batch});
    Tensor t(targets.span(), DType::I32, {width, batch}), d(drafts.span(), DType::I32, {k, batch});
    Tensor e(extents.span(), DType::I32, {batch}), len(lengths.span(), DType::I32, {batch});
    Tensor a(anchors.span(), DType::I32, {batch}), out(licensed.span(), DType::I32, {width, batch});
    Tensor c(counts.span(), DType::I32, {batch}), acc(accepted.span(), DType::I32, {batch});
    const std::size_t bytes = sparse
        ? ops::speculative_accept_sparse_drafts_workspace_capacity_bytes(domain, {false}, k, k, batch, batch)
        : ops::speculative_accept_greedy_drafts_workspace_capacity_bytes(domain, k, k, batch, batch);
    DeviceBuffer backing(cuda_environment().context.device(), std::max<std::size_t>(bytes, 256));
    MemoryArena workspace(backing.span());
    if (sparse) {
        std::vector<int> ids(16 * k * batch);
        std::vector<float> q(ids.size(), 0);
        for (int i = 0; i < k * batch; ++i) {
            ids[i * 16] = 17;
            ids[i * 16 + 1] = 18;
            q[i * 16] = 1;
            for (int j = 2; j < 16; ++j) ids[i * 16 + j] = j - 2;
        }
        auto candidates = to_device(ids);
        auto probabilities = to_device(q);
        Tensor ci(candidates.span(), DType::I32, {16, k, batch});
        Tensor pq(probabilities.span(), DType::FP32, {16, k, batch});
        ops::speculative_accept_sparse_drafts(t, l, d, ci, pq, e, len, a, out, c, acc, domain,
            static_cast<const ops::SamplingConfig*>(configs.data()), {false}, workspace,
            cuda_environment().default_execution);
        cuda_synchronize();
    } else {
        ops::speculative_accept_greedy_drafts(t, l, d, e, len, a, out, c, acc, domain,
            static_cast<const ops::SamplingConfig*>(configs.data()), workspace,
            cuda_environment().default_execution);
        cuda_synchronize();
    }
    int failures = 0;
    failures += verify_exact("FP32 acceptance rejects BF16-tied draft", from_device<int>(accepted, batch),
                              std::vector<int>(batch, 0));
    failures += verify_exact("FP32 acceptance correction", from_device<int>(anchors, batch), std::vector<int>(batch, 18));
    failures += verify_exact("FP32 acceptance counts", from_device<int>(counts, batch), std::vector<int>(batch, 1));
    failures += verify_exact("FP32 acceptance frontier", from_device<int>(lengths, batch), std::vector<int>(batch, 101));
    std::vector<int> expected(width * batch, 0);
    for (int b = 0; b < batch; ++b) expected[b * width] = 18;
    failures += verify_exact("FP32 licensed tokens", from_device<int>(licensed, expected.size()), expected);
    return failures;
}
} // namespace

int main() {
    if (cuda_unavailable()) return 77;
    int failures = projections() + readers();
    for (bool sparse : {false, true})
        for (bool stochastic : {false, true}) failures += acceptance(sparse, stochastic);
    std::cout << (failures ? "FAIL" : "OK") << " FP32 logits numerical qualification\n";
    return failures ? 1 : 0;
}
