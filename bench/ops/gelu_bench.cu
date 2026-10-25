#include "ninfer/ops/gelu.h"
#include "core/buffer.h"
#include "op_measurement.cuh"

#include <cuda_bf16.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ninfer;
using namespace ninfer::bench;

namespace {

void run(ops::GeluMode mode, int d, int columns) {
    const std::size_t n = static_cast<std::size_t>(d) * static_cast<std::size_t>(columns);
    DeviceBuffer x      = make_bf16(n, 101U);
    SavedBuffer initial(x);
    const auto restore = [&](CudaExecutionView cuda) { initial.restore(cuda); };
    Tensor tx(x.span(), DType::BF16, {d, columns});

    const Result result = bench_loop_prepared(
        restore, [&](CudaExecutionView cuda) { ops::gelu(tx, mode, cuda); },
        static_cast<double>(n) * 4.0);

    char tag[96];
    const char* name = mode == ops::GeluMode::Tanh ? "gelu_tanh" : "gelu_exact";
    std::snprintf(tag, sizeof(tag), "%s [%d,%-5d]", name, d, columns);
    print_result(tag, result);
}

} // namespace

int main(int argc, char** argv) try {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
        std::printf("SKIP: no usable CUDA device\n");
        return 0;
    }

    int selected_columns        = 0;
    ops::GeluMode selected_mode = ops::GeluMode::Tanh;
    bool mode_selected          = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--columns") && i + 1 < argc) {
            selected_columns = measurement::integer(argv[++i], 0);
        } else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char* mode = argv[++i];
            if (!std::strcmp(mode, "tanh")) {
                selected_mode = ops::GeluMode::Tanh;
            } else if (!std::strcmp(mode, "exact")) {
                selected_mode = ops::GeluMode::Exact;
            } else {
                std::fprintf(stderr, "mode must be tanh or exact\n");
                return 2;
            }
            mode_selected = true;
        } else {
            std::fprintf(stderr, "usage: %s [--mode tanh|exact --columns C]\n", argv[0]);
            return 2;
        }
    }
    if (selected_columns < 0 || (selected_columns > 0 && !mode_selected)) {
        std::fprintf(stderr, "columns requires an explicit mode and must be positive\n");
        return 2;
    }

    if (selected_columns > 0) {
        const int d = selected_mode == ops::GeluMode::Tanh ? 4304 : 4608;
        run(selected_mode, d, selected_columns);
        return 0;
    }
    for (const int patches : {8, 256, 4096, 49152, 65536}) {
        run(ops::GeluMode::Tanh, 4304, patches);
    }
    for (const int merged : {2, 64, 1024, 12288, 16384}) {
        run(ops::GeluMode::Exact, 4608, merged);
    }
    return 0;
} catch (const std::exception& error) {

    std::fprintf(stderr, "ninfer_gelu_bench: %s\n", error.what());
    return 2;
}
