#include "ninfer/ops/indexing.h"
#include "core/buffer.h"
#include "op_measurement.cuh"

#include <cuda_runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <vector>

using namespace ninfer;
using namespace ninfer::bench;

namespace {

void run(std::int32_t d, std::int32_t vision_tokens, bool profile_once) {
    const std::int32_t prompt_tokens = vision_tokens + 2;
    const std::size_t n              = static_cast<std::size_t>(d) * vision_tokens;
    DeviceBuffer source              = make_bf16(n, 101U);
    DeviceBuffer destination         = make_bf16(static_cast<std::size_t>(d) * prompt_tokens, 103U);
    std::vector<std::int32_t> indices(static_cast<std::size_t>(vision_tokens));
    std::iota(indices.begin(), indices.end(), 1);
    DeviceBuffer device_indices(ninfer::bench::device_context().device(),
                                indices.size() * sizeof(std::int32_t));
    cudaMemcpy(device_indices.data(), indices.data(), device_indices.size(),
               cudaMemcpyHostToDevice);
    Tensor source_tensor(source.span(), DType::BF16, {d, vision_tokens});
    Tensor destination_tensor(destination.span(), DType::BF16, {d, prompt_tokens});
    Tensor indices_tensor(device_indices.span(), DType::I32, {vision_tokens});

    const auto launch = [&](CudaExecutionView cuda) {
        ops::scatter(source_tensor, indices_tensor, destination_tensor, cuda);
    };

    if (profile_once) {
        launch(default_execution());
        cudaDeviceSynchronize();
        std::printf("scatter profile D=%d V=%d\n", d, vision_tokens);
        return;
    }

    const double bytes  = static_cast<double>(vision_tokens) * (d * 4.0 + sizeof(std::int32_t));
    const Result result = bench_loop(launch, bytes);
    char tag[96];
    std::snprintf(tag, sizeof(tag), "scatter [%d,%d]", d, vision_tokens);
    print_result(tag, result);
}

} // namespace

int main(int argc, char** argv) try {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        std::printf("SKIP: no usable CUDA device\n");
        return 0;
    }

    std::int32_t d    = 2048;
    bool profile_once = false;
    std::vector<std::int32_t> vision_tokens;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--d") && i + 1 < argc) {
            d = measurement::integer(argv[++i]);
        } else if (!std::strcmp(argv[i], "--vision") && i + 1 < argc) {
            vision_tokens.push_back(measurement::integer(argv[++i]));
        } else if (!std::strcmp(argv[i], "--profile-once")) {
            profile_once = true;
        } else {
            std::fprintf(stderr, "usage: %s [--d D] [--vision V] [--profile-once]\n", argv[0]);
            return 2;
        }
    }
    if (vision_tokens.empty()) {
        vision_tokens = {1, 2, 64, 1024, 12288, 16384};
    }
    for (const std::int32_t v : vision_tokens) { run(d, v, profile_once); }
    return 0;
} catch (const std::exception& error) {

    std::fprintf(stderr, "ninfer_scatter_bench: %s\n", error.what());
    return 2;
}
