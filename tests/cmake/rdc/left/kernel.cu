#include <cuda_runtime.h>

__device__ int rdc_value(int);

namespace {
template <int Offset>
__global__ void kernel(int* value) {
    *value = rdc_value(*value) + Offset;
}
}

void rdc_left(int* value) {
    kernel<1><<<1, 1>>>(value);
}
