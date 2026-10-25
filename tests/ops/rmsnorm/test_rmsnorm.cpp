#include "ninfer/ops/rmsnorm.h"
#include "cuda_fixture.h"
#include "core/buffer.h"
#include "ops/norm_test_common.h"
#include "core/device.h"
#include "core/decode_graph.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace ninfer;
using namespace ninfer::test;
using namespace ninfer::test::norm;

namespace {

// BF16 RNE alone can incur almost 2^-8 relative error; the gross bound must
// contain that storage error as well as FP32 reduction/rsqrt error.
constexpr ReductionCriterion rmsnorm_bf16_criterion() {
    return {/*relative_l2*/ 1.85e-3, /*gross_absolute*/ 1.0e-5,
            /*gross_relative_to_max_reference*/ 3.95e-3};
}

std::vector<double> rmsnorm_oracle(const std::vector<float>& input,
                                   const std::vector<float>& weight, const Shape& shape,
                                   bool unit_offset) {
    std::vector<double> output(input.size());
    const auto row_count = static_cast<std::int64_t>(shape.rows) * shape.tokens;
    for (std::int64_t row = 0; row < row_count; ++row) {
        const std::size_t base = static_cast<std::size_t>(row) * shape.d;
        double sum_squares     = 0.0;
        for (std::int32_t column = 0; column < shape.d; ++column) {
            const double value = input[base + column];
            sum_squares += value * value;
        }
        const double inverse = 1.0 / std::sqrt(sum_squares / static_cast<double>(shape.d) + kEps);
        for (std::int32_t column = 0; column < shape.d; ++column) {
            const double gain     = static_cast<double>(weight[column]) + (unit_offset ? 1.0 : 0.0);
            output[base + column] = static_cast<double>(input[base + column]) * inverse * gain;
        }
    }
    return output;
}

int run_case(const char* label, const Shape& shape, bool unit_offset, std::uint32_t seed,
             float input_scale = 4.0F, bool bf16x2_unaligned = false, bool replay = false,
             int width = 0) {
    const std::size_t count = shape.elements();
    std::vector<float> input(count), weight(shape.d);
    fill_uniform(input, seed, -input_scale, input_scale);
    fill_uniform(weight, seed + 1U, -1.5F, 1.5F);
    round_to_bf16(input);
    round_to_bf16(weight);
    weight[0] =
        unit_offset ? -1.0F : 0.0F; // Exact zero gain, without an intermediate BF16 gain cast.

    DeviceInput device_input  = make_input(input, bf16x2_unaligned);
    DeviceInput device_weight = make_input(weight, bf16x2_unaligned);
    const std::size_t leading = bf16x2_unaligned ? sizeof(std::uint16_t) : 0;
    GuardedDeviceBuffer output(leading + count * sizeof(std::uint16_t));
    output.fill(0xff);
    void* output_data = static_cast<std::uint8_t*>(output.data()) + leading;

    Tensor input_tensor = tensor_for(device_input.memory, shape);
    Tensor weight_tensor(device_weight.memory, DType::BF16, {shape.d});
    Tensor output_tensor = tensor_for(output.span().subspan(leading), shape);
    if (width) {
        input_tensor  = Tensor(device_input.memory, DType::BF16,
                               {shape.d, shape.rows, width, shape.tokens / width});
        output_tensor = Tensor(output.span().subspan(leading), DType::BF16,
                               {shape.d, shape.rows, width, shape.tokens / width});
    }
    DeviceContext device;
    DecodeGraphDefinition definition;
    DecodeGraphExecutable graph;
    const auto launch = [&] {
        ops::rmsnorm(input_tensor, weight_tensor, kEps, unit_offset, output_tensor,
                     device.execution_view());
    };
    int failures = 0;
    for (int phase = 0; phase < (replay ? 2 : 1); ++phase) {
        if (phase) {
            for (auto& value : input) value = -value;
            for (auto& value : weight) value = -value;
            const std::size_t offset = bf16x2_unaligned ? 1 : 0;
            for (std::size_t i = 0; i < input.size(); ++i)
                device_input.expected[i + offset] = f32_to_bf16(input[i]);
            for (std::size_t i = 0; i < weight.size(); ++i)
                device_weight.expected[i + offset] = f32_to_bf16(weight[i]);
            CUDA_CHECK(cudaMemcpy(device_input.storage.data(), device_input.expected.data(),
                                  device_input.expected.size() * 2, cudaMemcpyHostToDevice));
            CUDA_CHECK(cudaMemcpy(device_weight.storage.data(), device_weight.expected.data(),
                                  device_weight.expected.size() * 2, cudaMemcpyHostToDevice));
        }
        output.fill(0xff);
        cuda_synchronize();
        if (replay && phase == 0) {
            definition.capture(device.stream(), launch);
            graph.instantiate(definition);
        }
        if (replay)
            graph.launch(device.stream());
        else
            launch();
        cuda_synchronize(device.stream());
        const auto reference = rmsnorm_oracle(input, weight, shape, unit_offset);
        failures += verify_reduction(label, from_device_bf16(output_data, count), reference,
                                     rmsnorm_bf16_criterion());
        failures += verify_output_storage(std::string(label) + " output", output, bf16x2_unaligned);
        failures += verify_preserved(std::string(label) + " preserves input", device_input);
        failures += verify_preserved(std::string(label) + " preserves weight", device_weight);
    }
    return failures;
}

} // namespace

int validate_contract() {
    DeviceBuffer input(ninfer::test::cuda_environment().context.device(), 128 * 4);
    DeviceBuffer weight(ninfer::test::cuda_environment().context.device(), 128 * 2);
    DeviceBuffer output(ninfer::test::cuda_environment().context.device(), 128 * 4);
    Tensor x(input.span(), DType::BF16, {128, 2}), w(weight.span(), DType::BF16, {128}),
        y(output.span(), DType::BF16, {128, 2});
    int failed        = 0;
    const auto reject = [&](const Tensor& a, const Tensor& b, Tensor out) {
        try {
            ops::rmsnorm(a, b, 1.0e-6F, false, out,
                         ninfer::test::cuda_environment().default_execution);
        } catch (const std::invalid_argument&) { return 0; }
        std::cerr << "rmsnorm overlap was accepted\n";
        return 1;
    };
    auto alias = y;
    alias.data = x.data;
    failed += reject(x, w, alias);
    alias.data = static_cast<std::byte*>(x.data) + 2;
    failed += reject(x, w, alias);
    alias.data = w.data;
    failed += reject(x, w, alias);
    auto tied = w;
    tied.data = x.data;
    failed += reject(x, tied, y);
    auto empty        = Tensor::metadata(DType::BF16, {128, 1});
    auto empty_weight = Tensor::metadata(DType::BF16, {128});
    empty.ne[1]       = 0;
    ops::rmsnorm(empty, empty_weight, 1.0e-6F, false, empty,
                 ninfer::test::cuda_environment().default_execution);
    return failed;
}

int main() {
    if (cuda_unavailable()) {
        std::cout << "SKIP: no usable CUDA device\n";
        return 77;
    }

    int failures = validate_contract();
    failures += run_case("rmsnorm offset [5120,1]", {5120, 1}, true, 1101U);
    failures += run_case("rmsnorm offset [5120,128]", {5120, 128}, true, 1102U);
    failures += run_case("rmsnorm offset [5120,1024]", {5120, 1024}, true, 1107U);
    failures += run_case("rmsnorm offset [2048,7]", {2048, 7}, true, 1103U);
    failures += run_case("rmsnorm offset [256,24,7]", {256, 24, 7}, true, 1104U);
    failures += run_case("rmsnorm offset [256,2,1]", {256, 2}, true, 1105U);
    failures += run_case("rmsnorm offset [256,4,48]", {256, 4, 48}, true, 1106U);
    failures += run_case("rmsnorm plain [2048,1]", {2048, 1}, false, 1201U);
    failures += run_case("rmsnorm plain [2048,128]", {2048, 128}, false, 1202U);
    failures += run_case("rmsnorm plain [128,32,7]", {128, 32, 7}, false, 1203U);
    failures += run_case("rmsnorm plain [128,8,128]", {128, 8, 128}, false, 1204U);
    failures += run_case("rmsnorm offset unaligned [128,32]", {128, 32}, true, 1301U, 4.0F, true);
    failures += run_case("rmsnorm plain unaligned [128,8]", {128, 8}, false, 1302U, 4.0F, true);
    failures += run_case("rmsnorm plain near-zero [128,32]", {128, 32}, false, 1303U, 1.0e-5F);
    for (int t = 1; t <= 128; ++t) {
        const bool graph = t == 1 || t == 2 || t == 7 || t == 8 || t == 15 || t == 16 || t == 31 ||
                           t == 32 || t == 63 || t == 64 || t == 65 || t == 96 || t == 127 ||
                           t == 128;
        for (bool offset : {false, true}) {
            const auto label =
                "rmsnorm hidden5120 T=" + std::to_string(t) + (offset ? " offset" : " plain");
            failures += run_case(label.c_str(), {5120, 1, t}, offset, 1400U + t, 4.f, false, graph);
        }
        for (int heads : {4, 24}) {
            const auto label =
                "rmsnorm QK256 H=" + std::to_string(heads) + " T=" + std::to_string(t);
            failures +=
                run_case(label.c_str(), {256, heads, t}, true, 1600U + t, 4.f, false, graph);
        }
    }
    for (int t : {129, 256, 1024, 2048})
        for (bool offset : {false, true})
            failures += run_case("rmsnorm hidden prefill", {5120, 1, t}, offset, 1700U + t, 4.f,
                                 false, true);
    for (int t : {129, 256, 1024, 2048})
        for (int heads : {4, 24})
            failures +=
                run_case("rmsnorm QK prefill", {256, heads, t}, true, 1750U + t, 4.f, false, true);
    for (int width : {2, 3, 8, 9, 15, 16})
        for (int batch : {1, 8}) {
            for (bool offset : {false, true})
                failures += run_case("rmsnorm hidden W/B", {5120, 1, width * batch}, offset,
                                     1800U + width, 4.f, false, true, width);
            for (int heads : {4, 24})
                failures += run_case("rmsnorm QK W/B", {256, heads, width * batch}, true,
                                     1900U + width, 4.f, false, true, width);
        }
    for (float scale : {0.0f, 1.e-5f, 4096.f})
        for (bool offset : {false, true}) {
            failures += run_case("rmsnorm hidden scale/unaligned", {5120, 1, 17}, offset, 2001U,
                                 scale, true, true);
            failures += run_case("rmsnorm QK scale/unaligned", {256, 4, 17}, offset, 2002U, scale,
                                 true, true);
        }
    for (int rows : {1, 8, 128, 1024, 4096}) {
        const auto label  = "rmsnorm offset10240 rows=" + std::to_string(rows);
        const bool replay = rows == 8 || rows == 128 || rows == 1024;
        failures += run_case(label.c_str(), {10240, 1, rows}, true, 2100U + rows, 4.f, false,
                             replay, rows == 128 ? 16 : 0);
    }
    for (float scale : {0.0f, 1.e-5f, 4096.f})
        failures += run_case("rmsnorm offset10240 scale", {10240, 1, 3}, true, 2201U, scale);
    failures +=
        run_case("rmsnorm offset10240 unaligned", {10240, 1, 7}, true, 2202U, 4.f, true, true);
    failures += run_case("rmsnorm plain10240", {10240, 1, 3}, false, 2203U);
    std::cout << (failures ? "FAIL" : "OK") << " rmsnorm\n";
    return failures ? 1 : 0;
}
