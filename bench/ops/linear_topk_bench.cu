// Public cold-cache benchmark for the three registered linear_topk profiles.
#include "ninfer/ops/linear_topk.h"

#include "ninfer_bench_common.h"
#include "quantized_weight.cuh"

#include <cuda_runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <string>
#include <vector>

using namespace ninfer;
using namespace ninfer::bench;

namespace {

enum class Profile {
    W8,
    Fp8,
    Q4,
};

const char* profile_name(Profile profile) {
    if (profile == Profile::W8) { return "w8-full"; }
    if (profile == Profile::Fp8) { return "fp8-full"; }
    return "q4-optimized";
}

void run(Profile profile, std::int32_t columns, int warmup, int repeat) {
    constexpr std::int32_t kHidden = 5120;
    constexpr std::int32_t kTopK   = 16;
    const std::int32_t rows        = profile == Profile::Q4 ? 131072 : 248320;
    const QType qtype              = profile == Profile::W8    ? QType::W8G32_F16S
                                     : profile == Profile::Fp8 ? QType::FP8_E4M3FN_ROW_BF16S
                                                               : QType::Q4G64_F16S;

    PackedQuantizedWeight packed =
        profile == Profile::Fp8
            ? make_fp8_weight(rows, kHidden)
            : make_row_split_weight(qtype, rows, kHidden, kHidden,
                                    QuantizedWeightFill{profile == Profile::Q4 ? std::uint8_t{0x11}
                                                                               : std::uint8_t{0x01},
                                                        0, 0x3c00});
    DeviceBuffer hidden = make_bf16(static_cast<std::size_t>(kHidden) * columns);
    DeviceBuffer ids(static_cast<std::size_t>(kTopK) * columns * sizeof(std::int32_t));
    DeviceBuffer scores(static_cast<std::size_t>(kTopK) * columns * sizeof(float));
    DeviceBuffer id_map(
        profile == Profile::Q4 ? static_cast<std::size_t>(rows) * sizeof(std::int32_t) : 1);
    if (profile == Profile::Q4) {
        std::vector<std::int32_t> host_ids(static_cast<std::size_t>(rows));
        for (std::int32_t row = 0; row < rows; ++row) { host_ids[row] = row; }
        id_map.copy_from_host(host_ids.data(), id_map.bytes);
    }

    const std::size_t workspace_bytes =
        ops::linear_topk_workspace_capacity_bytes(qtype, rows, kHidden, columns, columns);
    WorkspaceArena workspace(workspace_bytes);
    Tensor hidden_tensor(hidden.p, DType::BF16, {kHidden, columns});
    Tensor ids_tensor(ids.p, DType::I32, {kTopK, columns});
    Tensor scores_tensor(scores.p, DType::FP32, {kTopK, columns});
    Tensor map_tensor(id_map.p, DType::I32, {rows});
    DeviceBuffer flush(256ULL << 20);
    cudaStream_t stream = nullptr;
    CUDA_CHECK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));

    const auto launch = [&](cudaStream_t launch_stream) {
        if (profile == Profile::Q4) {
            ops::linear_topk(hidden_tensor, packed.weight, map_tensor, ids_tensor, scores_tensor,
                             workspace, launch_stream);
        } else {
            ops::linear_topk(hidden_tensor, packed.weight, 248077, ids_tensor, scores_tensor,
                             workspace, launch_stream);
        }
    };
    TimedGraph graph;
    graph.capture(stream, launch);
    const ColdTiming timing    = measure_cold_graph(graph, flush, stream, warmup, repeat);
    const double weight_bytes  = static_cast<double>(packed.model_weight_bytes());
    const double effective_gbs = weight_bytes / timing.median_us / 1000.0;
    const double useful_tflops = 2.0 * static_cast<double>(profile == Profile::Q4 ? rows : 248077) *
                                 kHidden * columns / timing.median_us / 1.0e6;
    std::printf("%s,%d,%.3f,%.3f,%.3f,%.1f,%.2f,%zu,%zu\n", profile_name(profile), columns,
                timing.median_us, timing.min_us, timing.p95_us, effective_gbs, useful_tflops,
                workspace.peak_used(), graph.nodes());
    CUDA_CHECK(cudaStreamDestroy(stream));
}

Profile parse_profile(std::string_view value) {
    if (value == "w8-full") { return Profile::W8; }
    if (value == "fp8-full") { return Profile::Fp8; }
    if (value == "q4-optimized") { return Profile::Q4; }
    throw std::invalid_argument("profile must be w8-full, fp8-full, or q4-optimized");
}

} // namespace

int main(int argc, char** argv) {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        std::printf("SKIP: no usable CUDA device\n");
        return 0;
    }
    try {
        std::string profile = "all";
        int columns = 0, warmup = 8, repeat = 60;
        for (int i = 1; i < argc; ++i) {
            const std::string flag = argv[i];
            if (flag == "--help") {
                std::printf("usage: %s [--profile all|w8-full|fp8-full|q4-optimized] [--columns U] "
                            "[--warmup N] [--repeat N]\n",
                            argv[0]);
                return 0;
            }
            if (i + 1 == argc) throw std::invalid_argument("missing option value");
            const std::string value = argv[++i];
            if (flag == "--profile")
                profile = value;
            else if (flag == "--columns") {
                columns = std::stoi(value);
                if (columns < 1) throw std::invalid_argument("columns must be positive");
            } else if (flag == "--warmup")
                warmup = std::stoi(value);
            else if (flag == "--repeat")
                repeat = std::stoi(value);
            else
                throw std::invalid_argument("unknown option: " + flag);
        }
        if (warmup < 0 || repeat < 1) throw std::invalid_argument("invalid timing counts");
        std::vector<Profile> profiles;
        if (profile == "all")
            profiles = {Profile::W8, Profile::Fp8, Profile::Q4};
        else
            profiles = {parse_profile(profile)};
        std::puts("profile,U,median_us,min_us,p95_us,weight_GBs,useful_TFLOPs,workspace_bytes,"
                  "graph_nodes");
        for (auto selected : profiles) {
            if (columns)
                run(selected, columns, warmup, repeat);
            else
                for (int n = 1; n <= 120; ++n) run(selected, n, warmup, repeat);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "linear_topk benchmark: %s\n", error.what());
        return 1;
    }
}
