#include "ninfer/ops/residual_add.h"
#include "core/buffer.h"
#include "op_measurement.cuh"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ninfer;
using namespace ninfer::bench;

namespace {

void run(int d, int tokens) {
    const std::size_t n = static_cast<std::size_t>(d) * static_cast<std::size_t>(tokens);
    DeviceBuffer y      = make_bf16(n, 101U);
    DeviceBuffer x      = make_bf16(n, 103U);
    SavedBuffer initial(x);
    const auto restore = [&](CudaExecutionView cuda) { initial.restore(cuda); };
    Tensor ty(y.span(), DType::BF16, {d, tokens});
    Tensor tx(x.span(), DType::BF16, {d, tokens});

    const Result result = bench_loop_prepared(
        restore, [&](CudaExecutionView cuda) { ops::residual_add(ty, tx, cuda); },
        static_cast<double>(n) * 6.0);

    char tag[96];
    std::snprintf(tag, sizeof(tag), "residual_add [%d,%-5d]", d, tokens);
    print_result(tag, result);
}

} // namespace

int main(int argc, char** argv) try {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        std::printf("SKIP: no usable CUDA device\n");
        return 0;
    }

    int d = 1152;
    std::vector<int> tokens;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--patches") && i + 1 < argc) {
            if (!tokens.empty()) {
                std::fprintf(stderr, "--patches and --tokens are mutually exclusive\n");
                return 2;
            }
            d      = 1152;
            tokens = {measurement::integer(argv[++i])};
        } else if (!std::strcmp(argv[i], "--d") && i + 1 < argc) {
            d = measurement::integer(argv[++i]);
            if (d != 1152 && d != 2048 && d != 5120) {
                std::fprintf(stderr, "D must be 1152, 2048, or 5120\n");
                return 2;
            }
        } else if (!std::strcmp(argv[i], "--tokens") && i + 1 < argc) {
            if (!tokens.empty()) {
                std::fprintf(stderr, "token extents were specified more than once\n");
                return 2;
            }
            tokens = measurement::integers(argv[++i]);
        } else {
            std::fprintf(stderr,
                         "usage: %s [--d 1152|2048|5120] [--tokens T[,T...] | --patches P]\n",
                         argv[0]);
            return 2;
        }
    }
    for (const int tokens_value : tokens) {
        if (tokens_value <= 0) {
            std::fprintf(stderr, "tokens must be positive\n");
            return 2;
        }
    }
    if (tokens.empty()) {
        tokens = {8, 256, 4096, 49152, 65536};
    }
    for (const int tokens_value : tokens) { run(d, tokens_value); }
    return 0;
} catch (const std::exception& error) {

    std::fprintf(stderr, "ninfer_residual_add_bench: %s\n", error.what());
    return 2;
}
