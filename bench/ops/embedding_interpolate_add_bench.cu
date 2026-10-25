#include "ninfer/ops/embedding.h"
#include "core/buffer.h"
#include "op_measurement.cuh"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace ninfer;
using namespace ninfer::bench;

namespace {

constexpr int kD    = 1152;
constexpr int kRows = 2304;

void run(std::int32_t patches, bool profile_once) {
    const std::size_t n = static_cast<std::size_t>(kD) * patches;
    DeviceBuffer table  = make_bf16(static_cast<std::size_t>(kD) * kRows, 101U);
    DeviceBuffer x      = make_bf16(n, 103U);
    std::vector<int> indices(static_cast<std::size_t>(patches) * 4);
    std::vector<float> weights(static_cast<std::size_t>(patches) * 4);
    for (int patch = 0; patch < patches; ++patch) {
        const float x          = fixture::uniform(patch, 211U, 0.F, 1.F);
        const float y          = fixture::uniform(patch, 213U, 0.F, 1.F);
        weights[patch * 4]     = (1.F - x) * (1.F - y);
        weights[patch * 4 + 1] = x * (1.F - y);
        weights[patch * 4 + 2] = (1.F - x) * y;
        weights[patch * 4 + 3] = x * y;
        for (int corner = 0; corner < 4; ++corner) {
            indices[static_cast<std::size_t>(patch) * 4 + corner] =
                (patch * 17 + corner * 49) % kRows;
        }
    }
    DeviceBuffer di(ninfer::bench::device_context().device(),
                    indices.size() * sizeof(std::int32_t));
    DeviceBuffer dw(ninfer::bench::device_context().device(), weights.size() * sizeof(float));
    cudaMemcpy(di.data(), indices.data(), di.size(), cudaMemcpyHostToDevice);
    cudaMemcpy(dw.data(), weights.data(), dw.size(), cudaMemcpyHostToDevice);
    SavedBuffer initial(x);
    const auto restore = [&](CudaExecutionView cuda) { initial.restore(cuda); };
    Tensor ttable(table.span(), DType::BF16, {kD, kRows});
    Tensor tx(x.span(), DType::BF16, {kD, patches});
    Tensor ti(di.span(), DType::I32, {4, patches});
    Tensor tw(dw.span(), DType::FP32, {4, patches});

    const auto launch = [&](CudaExecutionView cuda) {
        ops::embedding_interpolate_add(ttable, ti, tw, tx, cuda);
    };

    if (profile_once) {
        launch(default_execution());
        cudaDeviceSynchronize();
        std::printf("embedding_interpolate_add profile P=%d\n", patches);
        return;
    }

    // Count x read/write, four BF16 table gathers, and per-patch indices/weights. Reused table
    // rows may hit in cache, so logical throughput is not measured DRAM bandwidth.
    const double logical_bytes = static_cast<double>(n) * 12.0 + patches * 32.0;
    const Result result        = bench_loop_prepared(restore, launch, logical_bytes);
    std::printf("embedding_interpolate_add [1152,%d] median=%8.2f us min=%8.2f us p95=%8.2f us "
                "logical_bytes=%.0f logical=%8.1f GB/s\n",
                patches, result.median_us, result.min_us, result.p95_us, logical_bytes, result.gbs);
}

} // namespace

int main(int argc, char** argv) try {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        std::printf("SKIP: no usable CUDA device\n");
        return 0;
    }

    bool profile_once = false;
    std::vector<std::int32_t> patches;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--profile-once")) {
            profile_once = true;
        } else if (!std::strcmp(argv[i], "--patch") && i + 1 < argc) {
            patches.push_back(measurement::integer(argv[++i]));
        } else {
            std::fprintf(stderr, "usage: %s [--patch P] [--profile-once]\n", argv[0]);
            return 2;
        }
    }
    if (patches.empty()) {
        patches = {8, 256, 4096, 49152, 65536};
    }
    for (const std::int32_t p : patches) { run(p, profile_once); }
    return 0;
} catch (const std::exception& error) {

    std::fprintf(stderr, "ninfer_embedding_interpolate_add_bench: %s\n", error.what());
    return 2;
}
