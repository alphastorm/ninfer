#include <cuda_runtime.h>

__device__ int rdc_value(int value) {
    return value + 7;
}

namespace {
template <int Offset>
__global__ void kernel(int* value) {
    *value += Offset;
}
}

void rdc_right(int* value) {
    kernel<2><<<1, 1>>>(value);
}
