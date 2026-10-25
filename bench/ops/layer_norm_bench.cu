#include "ninfer/ops/layer_norm.h"
#include "core/buffer.h"
#include "op_measurement.cuh"

#include <cuda_bf16.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ninfer;
using namespace ninfer::bench;

namespace {

void run(int patches) {
    constexpr int d     = 1152;
    const std::size_t n = static_cast<std::size_t>(d) * static_cast<std::size_t>(patches);
    DeviceBuffer x      = make_bf16(n, 101U);
    DeviceBuffer weight = make_bf16(d, 103U, .8F, 1.2F);
    DeviceBuffer bias   = make_bf16(d, 105U);
    DeviceBuffer out    = make_zeros(n * 2);
    Tensor tx(x.span(), DType::BF16, {d, patches});
    Tensor tw(weight.span(), DType::BF16, {d});
    Tensor tb(bias.span(), DType::BF16, {d});
    Tensor tout(out.span(), DType::BF16, {d, patches});

    const Result result = bench_loop(
        [&](CudaExecutionView cuda) { ops::layer_norm(tx, tw, tb, 1.0e-6f, tout, cuda); },
        n * 4.0 + d * 4.0);

    char tag[80];
    std::snprintf(tag, sizeof(tag), "layer_norm [1152,%-5d]", patches);
    print_result(tag, result);
}

} // namespace

int main(int argc, char** argv) try {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        std::printf("SKIP: no usable CUDA device\n");
        return 0;
    }

    int selected_patches = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--patches") && i + 1 < argc) {
            selected_patches = measurement::integer(argv[++i], 0);
        } else {
            std::fprintf(stderr, "usage: %s [--patches P]\n", argv[0]);
            return 2;
        }
    }
    if (selected_patches < 0 || (selected_patches > 0 && selected_patches % 4 != 0)) {
        std::fprintf(stderr, "patches must be a positive multiple of 4\n");
        return 2;
    }

    if (selected_patches > 0) {
        run(selected_patches);
        return 0;
    }
    for (const int patches : {8, 256, 4096, 49152, 65536}) { run(patches); }
    return 0;
} catch (const std::exception& error) {

    std::fprintf(stderr, "ninfer_layer_norm_bench: %s\n", error.what());
    return 2;
}
