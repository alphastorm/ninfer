#include <cuda_runtime.h>

namespace {
template <int Offset>
__global__ void kernel(int* value) {
    *value += Offset;
}
}

// No externally linked device entity: nvcc must generate a unique module ID.
void rdc_internal(int* value) {
    kernel<3><<<1, 1>>>(value);
}
