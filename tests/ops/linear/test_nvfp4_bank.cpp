#include "core/weight_view.h"
#include "ninfer/ops/linear.h"
#include "ninfer/ops/linear_add.h"
#include "ninfer/ops/linear_split.h"
#include "ninfer/ops/linear_swiglu.h"
#include "ops/input_projection_test_common.h"
#include "ops/linear_common.h"

#include <array>
#include <cstring>

namespace {

using namespace ninfer;
using namespace ninfer::test;
using namespace ninfer::test::input_projection;

enum class Operation { Linear, Add, Split, SwiGLU };

int check_bank(Operation operation, int n, int k) {
    const auto packed = quantized_weight::make_patterned_weight(
        QType::NVFP4, n, k, 701U, {.weight_scale_divisor = 128.0F, .input_scale_divisor = 1.0F});
    const std::array<std::uint64_t, 3> shape{2, static_cast<std::uint64_t>(n + 256),
                                             static_cast<std::uint64_t>(k)};
    const auto geometry =
        weight_geometry(QType::NVFP4, QuantLayout::BatchedBlockScaleK16M128x4, shape);
    const auto first_row    = shape[1] + 128;
    const auto code_offset  = first_row * k / 2;
    const auto scale_offset = geometry.scale_offset + first_row * k / 16;
    std::vector<std::byte> bytes(geometry.bytes);
    std::memcpy(bytes.data() + code_offset, packed.payload.data(), packed.code_plane_bytes);
    std::memcpy(bytes.data() + scale_offset, packed.payload.data() + packed.scale_plane_offset,
                packed.scale_plane_bytes);
    const std::array divisors{64.0F, 128.0F};
    std::memcpy(bytes.data() + geometry.divisor_offset, divisors.data(), sizeof(divisors));
    GuardedDeviceBuffer storage(bytes.size());
    storage.copy_from_host(bytes.data(), bytes.size());
    const WeightParent parent{geometry, storage.span().device,
                              static_cast<const std::byte*>(storage.data()), divisors};
    const auto weight =
        native_weight({{static_cast<std::uint64_t>(n), static_cast<std::uint64_t>(k)},
                       {{&parent, first_row * k, (first_row + n) * k}}},
                      1.0F);

    const bool swiglu      = operation == Operation::SwiGLU;
    const int output_rows  = swiglu ? n / 2 : n;
    const auto sample_rows = sampled_rows(output_rows, 17);
    std::vector<double> sums, up_sums;
    for (const int row : sample_rows) {
        double sum = 0, up = 0;
        for (int column = 0; column < k; ++column) {
            sum += quantized_weight::logical_weight_fp64(packed, row, column);
            if (swiglu)
                up += quantized_weight::logical_weight_fp64(packed, row + output_rows, column);
        }
        sums.push_back(sum);
        up_sums.push_back(up);
    }
    int failures = 0;
    for (const auto policy : {ops::LinearPolicy::A16Only, ops::LinearPolicy::AllowA4}) {
        for (const int tokens :
             {1, policy == ops::LinearPolicy::A16Only ? (swiglu ? 16 : 129) : 257}) {
            // +/-6 is exact in the A4 activation representation with divisor 1.
            std::vector<std::uint16_t> input_bits(static_cast<std::size_t>(k) * tokens);
            for (int token = 0; token < tokens; ++token) {
                std::fill_n(input_bits.data() + static_cast<std::size_t>(token) * k, k,
                            f32_to_bf16(token % 2 ? -6.0F : 6.0F));
            }
            DeviceBuffer input_storage(cuda_environment().context.device(), input_bits.size() * 2);
            input_storage.copy_from_host(input_bits.data(), input_storage.size());
            const Tensor input(input_storage.span(), DType::BF16, {k, tokens});
            const std::vector<int> segments = operation == Operation::Split
                                                  ? std::vector<int>{6144, 1024, 6144, 1024}
                                                  : std::vector<int>{output_rows};
            std::vector<GuardedBf16Tensor> outputs;
            for (const int rows : segments) outputs.emplace_back(rows, tokens);
            std::vector<Tensor> tensors;
            for (auto& output : outputs) tensors.push_back(output.tensor());
            if (operation == Operation::Add) {
                outputs[0].copy_from_bits(std::vector<std::uint16_t>(
                    static_cast<std::size_t>(output_rows) * tokens, f32_to_bf16(0.5F)));
            }
            const ops::ProjectionWeights projection = ops::SingleProjectionWeight{weight, policy};
            std::size_t capacity                    = 0;
            switch (operation) {
            case Operation::Linear:
                capacity = ops::linear_workspace_capacity_bytes(QType::NVFP4, n, k, policy, tokens,
                                                                tokens);
                break;
            case Operation::Add:
                capacity = ops::linear_add_workspace_capacity_bytes(QType::NVFP4, n, k, policy,
                                                                    tokens, tokens);
                break;
            case Operation::Split:
                capacity = ops::linear_split_workspace_capacity_bytes(projection, segments, tokens,
                                                                      tokens);
                break;
            case Operation::SwiGLU:
                capacity = ops::linear_swiglu_workspace_capacity_bytes(QType::NVFP4, n, k, policy,
                                                                       tokens, tokens);
                break;
            }
            GuardedDeviceBuffer scratch(std::max<std::size_t>(capacity, 256));
            MemoryArena workspace(scratch.span());
            const auto execution = cuda_environment().default_execution;
            switch (operation) {
            case Operation::Linear:
                ops::linear(input, weight, tensors[0], policy, workspace, execution);
                break;
            case Operation::Add:
                ops::linear_add(input, weight, tensors[0], policy, workspace, execution);
                break;
            case Operation::Split:
                ops::linear_split(input, projection, tensors, workspace, execution);
                break;
            case Operation::SwiGLU:
                ops::linear_swiglu(input, weight, tensors[0], policy, workspace, execution);
                break;
            }
            cuda_synchronize();
            const std::string label =
                "NVFP4 bank op=" + std::to_string(static_cast<int>(operation)) +
                " T=" + std::to_string(tokens) +
                (policy == ops::LinearPolicy::AllowA4 ? " A4" : " A16");
            std::vector<std::vector<double>> downloaded;
            for (auto& output : outputs) {
                failures += output.verify_guards(label);
                failures += output.verify_fully_written(label);
                downloaded.push_back(output.values());
            }
            std::vector<double> actual, expected;
            for (std::size_t i = 0; i < sample_rows.size(); ++i) {
                int local_row       = sample_rows[i];
                std::size_t segment = 0;
                while (local_row >= segments[segment]) local_row -= segments[segment++];
                for (const int token : sampled_rows(tokens, 7)) {
                    const double x                = token % 2 ? -6.0 : 6.0;
                    const double projection_value = sums[i] * x;
                    const double value =
                        swiglu
                            ? projection_value / (1 + std::exp(-projection_value)) * up_sums[i] * x
                            : projection_value + (operation == Operation::Add ? 0.5 : 0.0);
                    actual.push_back(
                        downloaded[segment][static_cast<std::size_t>(token) * segments[segment] +
                                            local_row]);
                    expected.push_back(value);
                }
            }
            failures += compare(label, actual, expected, ReductionCriterion{0.02, 1.0 / 256, 0.02});
            failures += scratch.verify_guards(label);
        }
    }
    failures += storage.verify_guards("NVFP4 bank");
    failures += linear_common::verify_preserved(storage.data(), std::span(bytes), "NVFP4 bank");
    return failures;
}

} // namespace

int main() {
    return ninfer::test::linear_common::run_test_main("NVFP4 bank operands", [] {
        return check_bank(Operation::Linear, 14336, 5120) +
               check_bank(Operation::Split, 14336, 5120) + check_bank(Operation::Add, 5120, 6144) +
               check_bank(Operation::SwiGLU, 34816, 5120);
    });
}
