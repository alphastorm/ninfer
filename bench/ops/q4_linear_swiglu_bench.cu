// Cold-cache public Op benchmark for the registered Q4 LinearSwiGLU profile.

#include "ninfer/ops/linear.h"
#include "ninfer/ops/linear_swiglu.h"
#include "ninfer/ops/silu_mul.h"

#include "core/device.h"
#include "ninfer_bench_common.h"
#include "ops/linear_swiglu/q4/q4_linear_swiglu_plan.h"
#include "quantized_weight.cuh"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace ninfer;

namespace {

constexpr std::int32_t kGateUpRows       = 34816;
constexpr std::int32_t kOutputRows       = 17408;
constexpr std::int32_t kHidden           = 5120;
constexpr std::int32_t kGroupSize        = 64;
constexpr std::size_t kFlushBytes        = 256ULL << 20;
constexpr std::uint64_t kPackedCodeBytes = static_cast<std::uint64_t>(kGateUpRows) * kHidden / 2;
constexpr std::uint64_t kScaleBytes =
    static_cast<std::uint64_t>(kGateUpRows) * (kHidden / kGroupSize) * sizeof(std::uint16_t);

enum class Route {
    Public,
    Materialized,
};

struct Options {
    std::vector<std::int32_t> tokens{1, 2, 4, 8, 16, 24, 32, 48};
    int warmup    = 5;
    int repeat    = 30;
    bool profile  = false;
    bool describe = false;
    Route route   = Route::Public;
};

const char* route_name(Route route) noexcept {
    return route == Route::Public ? "public" : "materialized";
}

const char* schedule_name(Route route, std::int32_t tokens) {
    if (route == Route::Materialized) { return "linear_swiglu.q4.materialized_control"; }
    const auto plan = ops::detail::q4_linear_swiglu_resolve_plan(
        {kGateUpRows, kOutputRows, kHidden, kHidden, tokens});
    return ops::detail::q4_linear_swiglu_schedule_name(plan.schedule);
}

void describe(Route route, std::int32_t tokens) {
    std::size_t workspace_bytes = 0;
    bool fused_epilogue         = false;
    const char* weight_reuse    = "null";
    if (route == Route::Public) {
        const auto plan = ops::detail::q4_linear_swiglu_resolve_plan(
            {kGateUpRows, kOutputRows, kHidden, kHidden, tokens});
        workspace_bytes = plan.workspace_bytes;
        fused_epilogue  = plan.schedule != ops::detail::Q4LinearSwiGluScheduleId::Materialized;
        if (plan.schedule == ops::detail::Q4LinearSwiGluScheduleId::SmallTExact) {
            weight_reuse = "true";
        }
    }
    const auto intermediate_bytes =
        route == Route::Materialized
            ? static_cast<std::uint64_t>(kGateUpRows) * tokens * sizeof(std::uint16_t)
            : 0;
    std::printf("{\"schema_version\":1,\"kernel_family\":\"linear_swiglu_q4\","
                "\"m\":%d,\"gate_up_n\":%d,\"output_n\":%d,\"k\":%d,"
                "\"group_size\":%d,\"route\":\"%s\",\"schedule\":\"%s\","
                "\"packed_code_bytes\":%llu,\"scale_bytes\":%llu,"
                "\"workspace_bytes\":%zu,\"materialized_intermediate_bytes\":%llu,"
                "\"gate_up_paired\":true,\"silu_epilogue_fused\":%s,"
                "\"weights_reused_across_m_within_cta\":%s}\n",
                tokens, kGateUpRows, kOutputRows, kHidden, kGroupSize, route_name(route),
                schedule_name(route, tokens), static_cast<unsigned long long>(kPackedCodeBytes),
                static_cast<unsigned long long>(kScaleBytes), workspace_bytes,
                static_cast<unsigned long long>(intermediate_bytes),
                fused_epilogue ? "true" : "false", weight_reuse);
}

std::vector<std::int32_t> parse_tokens(std::string_view raw) {
    std::vector<std::int32_t> result;
    std::size_t begin = 0;
    while (begin < raw.size()) {
        const std::size_t end = raw.find(',', begin);
        const std::string token(
            raw.substr(begin, end == std::string_view::npos ? raw.size() - begin : end - begin));
        const long value = std::stol(token);
        if (value <= 0 || value > std::numeric_limits<std::int32_t>::max()) {
            throw std::invalid_argument("--t-sweep values must be positive int32");
        }
        result.push_back(static_cast<std::int32_t>(value));
        if (end == std::string_view::npos) { break; }
        begin = end + 1;
    }
    if (result.empty()) { throw std::invalid_argument("--t-sweep must not be empty"); }
    return result;
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto next = [&](const char* label) -> std::string_view {
            if (++index >= argc) { throw std::invalid_argument(std::string("missing ") + label); }
            return argv[index];
        };
        if (argument == "--t-sweep") {
            options.tokens = parse_tokens(next("--t-sweep value"));
        } else if (argument == "--warmup") {
            options.warmup = std::stoi(std::string(next("--warmup value")));
        } else if (argument == "--repeat") {
            options.repeat = std::stoi(std::string(next("--repeat value")));
        } else if (argument == "--profile") {
            options.profile = true;
        } else if (argument == "--describe") {
            options.describe = true;
        } else if (argument == "--route") {
            const std::string_view route = next("--route value");
            if (route == "public") {
                options.route = Route::Public;
            } else if (route == "materialized") {
                options.route = Route::Materialized;
            } else {
                throw std::invalid_argument("--route must be public or materialized");
            }
        } else if (argument == "--help" || argument == "-h") {
            std::printf("Usage: %s [--t-sweep 1,2,...] [--warmup N] [--repeat N] "
                        "[--route public|materialized] [--profile|--describe]\n",
                        argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + std::string(argument));
        }
    }
    if (options.warmup < 0 || options.repeat <= 0) {
        throw std::invalid_argument("--warmup must be nonnegative and --repeat positive");
    }
    if (options.profile && options.tokens.size() != 1) {
        throw std::invalid_argument("--profile requires exactly one T");
    }
    if (options.profile && options.describe) {
        throw std::invalid_argument("--profile and --describe are mutually exclusive");
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.describe) {
            for (const std::int32_t tokens : options.tokens) { describe(options.route, tokens); }
            return 0;
        }
        const auto [min_it, max_it] =
            std::minmax_element(options.tokens.begin(), options.tokens.end());
        const std::int32_t min_t = *min_it;
        const std::int32_t max_t = *max_it;

        cudaStream_t stream = nullptr;
        CUDA_CHECK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        DeviceBuffer flush(kFlushBytes);
        DeviceBuffer input = bench::make_bf16(static_cast<std::size_t>(kHidden) * max_t);
        DeviceBuffer output(static_cast<std::size_t>(kOutputRows) * max_t * sizeof(std::uint16_t));
        std::unique_ptr<DeviceBuffer> materialized;
        if (options.route == Route::Materialized) {
            materialized = std::make_unique<DeviceBuffer>(static_cast<std::size_t>(kGateUpRows) *
                                                          max_t * sizeof(std::uint16_t));
        }
        bench::PackedQuantizedWeight packed = bench::make_row_split_weight(
            QType::Q4G64_F16S, kGateUpRows, kHidden, kHidden, {0x31, 0xa5, 0x3c00});
        const std::size_t workspace_capacity = ops::linear_swiglu_workspace_capacity_bytes(
            QType::Q4G64_F16S, kGateUpRows, kHidden, min_t, max_t);
        WorkspaceArena workspace(std::max<std::size_t>(workspace_capacity, 256));

        const auto launch = [&](std::int32_t tokens, cudaStream_t launch_stream) {
            Tensor x(input.p, DType::BF16, {kHidden, tokens});
            Tensor out(output.p, DType::BF16, {kOutputRows, tokens});
            if (options.route == Route::Public) {
                ops::linear_swiglu(x, packed.weight, out, workspace, launch_stream);
                return;
            }
            Tensor gate_up(materialized->p, DType::BF16, {kGateUpRows, tokens});
            ops::linear(x, packed.weight, gate_up, launch_stream);
            ops::silu_mul(gate_up.slice(0, 0, kOutputRows),
                          gate_up.slice(0, kOutputRows, kOutputRows), out, launch_stream);
        };

        if (options.profile) {
            launch(options.tokens.front(), stream);
            CUDA_CHECK(cudaStreamSynchronize(stream));
            const auto tokens = options.tokens.front();
            std::printf("PROFILE linear_swiglu Q4 route=%s schedule=%s T=%d workspace=%zu\n",
                        route_name(options.route), schedule_name(options.route, tokens), tokens,
                        workspace_capacity);
            CUDA_CHECK(cudaStreamDestroy(stream));
            return 0;
        }

        for (const std::int32_t tokens : options.tokens) {
            const auto timing = bench::measure_cold_launch(
                [&](cudaStream_t launch_stream) { launch(tokens, launch_stream); }, flush, stream,
                options.warmup, options.repeat);
            const double seconds = timing.median_us * 1.0e-6;
            const double flops   = 2.0 * static_cast<double>(kGateUpRows) * kHidden * tokens;
            const double bytes   = static_cast<double>(packed.model_weight_bytes()) +
                                   2.0 * static_cast<double>(kHidden + kOutputRows) * tokens +
                                   (options.route == Route::Materialized
                                        ? 2.0 * sizeof(std::uint16_t) * kGateUpRows * tokens
                                        : 0.0);
            std::printf("route=%-12s schedule=%-44s T=%-3d median=%8.3f us p95=%8.3f us "
                        "%7.1f GB/s %7.2f TFLOP/s workspace=%zu\n",
                        route_name(options.route), schedule_name(options.route, tokens), tokens,
                        timing.median_us, timing.p95_us, bytes / seconds / 1.0e9,
                        flops / seconds / 1.0e12, workspace_capacity);
        }

        CUDA_CHECK(cudaStreamDestroy(stream));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ninfer_q4_linear_swiglu_bench: %s\n", error.what());
        return 1;
    }
}
