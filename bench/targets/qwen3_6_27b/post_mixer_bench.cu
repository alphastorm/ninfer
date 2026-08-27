// Exact Qwen3.6 27B groupwise-int Q4 -> Q5 post-mixer state experiment.

#include "ninfer/ops/linear_add.h"
#include "ninfer/ops/linear_swiglu.h"

#include "core/device.h"
#include "ninfer_bench_common.h"
#include "ops/linear_add/q5/q5_linear_add_kernels.h"
#include "ops/linear_add/q5/q5_linear_add_plan.h"
#include "ops/linear_swiglu/q4/q4_linear_swiglu_plan.h"
#include "quantized_weight.cuh"

#include <cuda_profiler_api.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace ninfer;

namespace {

constexpr std::int32_t kHidden         = 5120;
constexpr std::int32_t kGateUpRows     = 34816;
constexpr std::int32_t kActivationRows = 17408;
constexpr std::int32_t kOutputRows     = 5120;
constexpr std::int32_t kTokens         = 4;
constexpr std::size_t kActivationBytes =
    static_cast<std::size_t>(kActivationRows) * kTokens * sizeof(std::uint16_t);
constexpr std::size_t kFlushBytes = 256ULL << 20;

enum class ActivationState : std::uint8_t {
    Cold,
    Q4Produced,
    Hot,
};

enum class Candidate : std::uint8_t {
    Public,
    MmaR64C16,
};

enum class Measure : std::uint8_t {
    Q5,
    Chain,
};

struct Options {
    ActivationState activation_state = ActivationState::Q4Produced;
    Candidate candidate              = Candidate::Public;
    Measure measure                  = Measure::Q5;
    int warmup                       = 5;
    int repeat                       = 30;
    bool profile_q5                  = false;
    bool describe                    = false;
};

const char* activation_state_name(ActivationState state) noexcept {
    switch (state) {
    case ActivationState::Cold:
        return "cold";
    case ActivationState::Q4Produced:
        return "q4-produced";
    case ActivationState::Hot:
        return "hot";
    }
    return "unknown";
}

const char* candidate_name(Candidate candidate) noexcept {
    return candidate == Candidate::Public ? "public" : "mma-r64-c16";
}

const char* measure_name(Measure measure) noexcept {
    return measure == Measure::Q5 ? "q5" : "chain";
}

const char* q5_schedule_name(Candidate candidate) {
    if (candidate == Candidate::MmaR64C16) {
        return "linear_add.q5.mma.r64.c16.cta_collective_residual";
    }
    const auto plan = ops::detail::q5_linear_add_resolve_plan(
        {kOutputRows, kActivationRows, kActivationRows, kTokens});
    return ops::detail::q5_linear_add_schedule_name(plan.schedule);
}

const char* q4_schedule_name() {
    const auto plan = ops::detail::q4_linear_swiglu_resolve_plan(
        {kGateUpRows, kActivationRows, kHidden, kHidden, kTokens});
    return ops::detail::q4_linear_swiglu_schedule_name(plan.schedule);
}

void print_usage(const char* executable) {
    std::printf("Usage: %s [--activation-state cold|q4-produced|hot] "
                "[--candidate public|mma-r64-c16] [--measure q5|chain] "
                "[--warmup N] [--repeat N] [--profile-q5|--describe]\n",
                executable);
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto next = [&](const char* label) -> std::string_view {
            if (++index >= argc) { throw std::invalid_argument(std::string("missing ") + label); }
            return argv[index];
        };
        if (argument == "--activation-state") {
            const std::string_view state = next("--activation-state value");
            if (state == "cold") {
                options.activation_state = ActivationState::Cold;
            } else if (state == "q4-produced") {
                options.activation_state = ActivationState::Q4Produced;
            } else if (state == "hot") {
                options.activation_state = ActivationState::Hot;
            } else {
                throw std::invalid_argument("--activation-state must be cold, q4-produced, or hot");
            }
        } else if (argument == "--candidate") {
            const std::string_view candidate = next("--candidate value");
            if (candidate == "public") {
                options.candidate = Candidate::Public;
            } else if (candidate == "mma-r64-c16") {
                options.candidate = Candidate::MmaR64C16;
            } else {
                throw std::invalid_argument("--candidate must be public or mma-r64-c16");
            }
        } else if (argument == "--measure") {
            const std::string_view measure = next("--measure value");
            if (measure == "q5") {
                options.measure = Measure::Q5;
            } else if (measure == "chain") {
                options.measure = Measure::Chain;
            } else {
                throw std::invalid_argument("--measure must be q5 or chain");
            }
        } else if (argument == "--warmup") {
            options.warmup = std::stoi(std::string(next("--warmup value")));
        } else if (argument == "--repeat") {
            options.repeat = std::stoi(std::string(next("--repeat value")));
        } else if (argument == "--profile-q5") {
            options.profile_q5 = true;
        } else if (argument == "--describe") {
            options.describe = true;
        } else if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + std::string(argument));
        }
    }
    if (options.warmup < 0 || options.repeat <= 0) {
        throw std::invalid_argument("--warmup must be nonnegative and --repeat positive");
    }
    if (options.measure == Measure::Chain &&
        options.activation_state != ActivationState::Q4Produced) {
        throw std::invalid_argument("--measure chain requires --activation-state q4-produced");
    }
    if (options.profile_q5 && options.measure != Measure::Q5) {
        throw std::invalid_argument("--profile-q5 requires --measure q5");
    }
    if (options.profile_q5 && options.describe) {
        throw std::invalid_argument("--profile-q5 and --describe are mutually exclusive");
    }
    return options;
}

void describe(const Options& options) {
    std::printf("{\"schema_version\":1,\"workload\":\"qwen27-post-mixer-t4\","
                "\"producer\":{\"op\":\"linear_swiglu\",\"format\":\"q4g64\","
                "\"schedule\":\"%s\",\"shape\":[34816,5120,4]},"
                "\"consumer\":{\"op\":\"linear_add\",\"format\":\"q5g64\","
                "\"candidate\":\"%s\",\"schedule\":\"%s\","
                "\"shape\":[5120,17408,4]},\"activation_bytes\":%zu,"
                "\"activation_state\":\"%s\",\"timed_scope\":\"%s\","
                "\"same_stream_dependency\":true,\"cuda_graph\":false}\n",
                q4_schedule_name(), candidate_name(options.candidate),
                q5_schedule_name(options.candidate), kActivationBytes,
                activation_state_name(options.activation_state), measure_name(options.measure));
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.describe) {
            describe(options);
            return 0;
        }

        cudaStream_t stream = nullptr;
        CUDA_CHECK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        DeviceBuffer flush(kFlushBytes);
        DeviceBuffer hidden = bench::make_bf16(static_cast<std::size_t>(kHidden) * kTokens);
        DeviceBuffer activation =
            bench::make_bf16(static_cast<std::size_t>(kActivationRows) * kTokens);
        DeviceBuffer residual = bench::make_bf16(static_cast<std::size_t>(kOutputRows) * kTokens);
        bench::PackedQuantizedWeight q4_gate_up = bench::make_row_split_weight(
            QType::Q4G64_F16S, kGateUpRows, kHidden, kHidden, {0x31, 0xa5, 0x3c00});
        bench::PackedQuantizedWeight q5_down = bench::make_row_split_weight(
            QType::Q5G64_F16S, kOutputRows, kActivationRows, kActivationRows, {0x47, 0x93, 0x3c00});
        const std::size_t q4_workspace_bytes = ops::linear_swiglu_workspace_capacity_bytes(
            QType::Q4G64_F16S, kGateUpRows, kHidden, kTokens, kTokens);
        const std::size_t q5_workspace_bytes = ops::linear_add_workspace_capacity_bytes(
            QType::Q5G64_F16S, kOutputRows, kActivationRows, kTokens, kTokens);
        WorkspaceArena q4_workspace(std::max<std::size_t>(q4_workspace_bytes, 256));
        WorkspaceArena q5_workspace(std::max<std::size_t>(q5_workspace_bytes, 256));

        Tensor hidden_tensor(hidden.p, DType::BF16, {kHidden, kTokens});
        Tensor activation_tensor(activation.p, DType::BF16, {kActivationRows, kTokens});
        Tensor residual_tensor(residual.p, DType::BF16, {kOutputRows, kTokens});

        const auto launch_q4 = [&](cudaStream_t launch_stream) {
            ops::linear_swiglu(hidden_tensor, q4_gate_up.weight, activation_tensor, q4_workspace,
                               launch_stream);
        };
        const auto launch_q5 = [&](cudaStream_t launch_stream) {
            if (options.candidate == Candidate::Public) {
                ops::linear_add(activation_tensor, q5_down.weight, residual_tensor, q5_workspace,
                                launch_stream);
            } else {
                ops::detail::q5_linear_add_mma_r64_c16_launch(activation_tensor, q5_down.weight,
                                                              residual_tensor, launch_stream);
            }
        };
        const auto prepare_q5 = [&](cudaStream_t launch_stream) {
            switch (options.activation_state) {
            case ActivationState::Cold:
                bench::flush_l2(flush, launch_stream);
                return;
            case ActivationState::Q4Produced:
                bench::flush_l2(flush, launch_stream);
                launch_q4(launch_stream);
                return;
            case ActivationState::Hot:
                launch_q5(launch_stream);
                return;
            }
        };
        const auto launch_sample = [&](cudaStream_t launch_stream) {
            if (options.measure == Measure::Chain) {
                bench::flush_l2(flush, launch_stream);
                launch_q4(launch_stream);
                launch_q5(launch_stream);
                return;
            }
            prepare_q5(launch_stream);
            launch_q5(launch_stream);
        };

        for (int iteration = 0; iteration < options.warmup; ++iteration) { launch_sample(stream); }
        CUDA_CHECK(cudaStreamSynchronize(stream));

        if (options.profile_q5) {
            prepare_q5(stream);
            CUDA_CHECK(cudaStreamSynchronize(stream));
            CUDA_CHECK(cudaProfilerStart());
            launch_q5(stream);
            CUDA_CHECK(cudaStreamSynchronize(stream));
            CUDA_CHECK(cudaProfilerStop());
            std::printf("PROFILE workload=qwen27-post-mixer-t4 activation_state=%s "
                        "candidate=%s schedule=%s timed_scope=q5\n",
                        activation_state_name(options.activation_state),
                        candidate_name(options.candidate), q5_schedule_name(options.candidate));
            CUDA_CHECK(cudaStreamDestroy(stream));
            return 0;
        }

        cudaEvent_t start = nullptr;
        cudaEvent_t stop  = nullptr;
        CUDA_CHECK(cudaEventCreate(&start));
        CUDA_CHECK(cudaEventCreate(&stop));
        std::vector<double> samples;
        samples.reserve(static_cast<std::size_t>(options.repeat));
        for (int iteration = 0; iteration < options.repeat; ++iteration) {
            if (options.measure == Measure::Chain) {
                bench::flush_l2(flush, stream);
                CUDA_CHECK(cudaEventRecord(start, stream));
                launch_q4(stream);
                launch_q5(stream);
            } else {
                prepare_q5(stream);
                CUDA_CHECK(cudaEventRecord(start, stream));
                launch_q5(stream);
            }
            CUDA_CHECK(cudaEventRecord(stop, stream));
            CUDA_CHECK(cudaEventSynchronize(stop));
            float milliseconds = 0.0F;
            CUDA_CHECK(cudaEventElapsedTime(&milliseconds, start, stop));
            samples.push_back(static_cast<double>(milliseconds) * 1000.0);
        }
        CUDA_CHECK(cudaEventDestroy(start));
        CUDA_CHECK(cudaEventDestroy(stop));
        const bench::ColdTiming timing = bench::summarize_timings(std::move(samples));
        std::printf("{\"schema_version\":1,\"workload\":\"qwen27-post-mixer-t4\","
                    "\"activation_state\":\"%s\",\"candidate\":\"%s\","
                    "\"schedule\":\"%s\",\"timed_scope\":\"%s\","
                    "\"warmup\":%d,\"repeat\":%d,\"median_us\":%.6f,"
                    "\"min_us\":%.6f,\"p95_us\":%.6f}\n",
                    activation_state_name(options.activation_state),
                    candidate_name(options.candidate), q5_schedule_name(options.candidate),
                    measure_name(options.measure), options.warmup, options.repeat, timing.median_us,
                    timing.min_us, timing.p95_us);

        CUDA_CHECK(cudaStreamDestroy(stream));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ninfer_qwen3_6_27b_post_mixer_bench: %s\n", error.what());
        return 1;
    }
}
