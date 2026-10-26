#include "ops/linear/kernels/bf16/bf16_a16_tma_mma.cuh"
#include "ops/linear/kernels/fp8/fp8_a8_tma_mma.cuh"
#include "ops/linear/kernels/nvfp4/nvfp4_a4_tma.cuh"

#include <cuda_runtime.h>
#include <array>
#include <cstdio>
#include <type_traits>

namespace {
using namespace ninfer::ops::detail;

template <class Descriptors>
inline constexpr int map_count = std::is_same_v<Descriptors, Nvfp4A4TmaDescriptors> ? 4 : 2;

template <class Descriptors>
__host__ __device__ auto& map_at(Descriptors& descriptors, int index) {
    if constexpr (std::is_same_v<std::remove_const_t<Descriptors>, Nvfp4A4TmaDescriptors>) {
        if (index == 0) return descriptors.a_codes;
        if (index == 1) return descriptors.b_codes;
        if (index == 2) return descriptors.a_scales;
        return descriptors.b_scales;
    } else {
        return index == 0 ? descriptors.weight : descriptors.activation;
    }
}

// This is a parameter ABI test, not a second implementation of a Linear Op. Real
// TMA reads every actual production descriptor through the by-value host stub.
template <class Descriptors>
__global__ void transfer(const __grid_constant__ Descriptors descriptors, unsigned* output) {
    using namespace ninfer::ops::common;
    constexpr int maps = map_count<Descriptors>;
    static_assert(sizeof(Descriptors) == maps * sizeof(CUtensorMap));
#if defined(__CUDA_ARCH__)
    static_assert(alignof(Descriptors) == 128);
#endif
    __shared__ __align__(128) unsigned tile[maps][128];
    __shared__ __align__(8) std::uint64_t barrier;
    if (threadIdx.x == 0) {
        cta_mbarrier_init(&barrier, 1);
        cta_mbarrier_fence_init();
        cta_mbarrier_arrive_expect_tx(&barrier, sizeof(tile));
#pragma unroll
        for (int i = 0; i < maps; ++i) {
            const auto* map = &map_at(descriptors, i);
            tma_load_2d(tile[i], map, 0, 0, &barrier);
            output[maps * 128 + i] = reinterpret_cast<std::uintptr_t>(map) % 128;
        }
    }
    __syncthreads();
    cta_mbarrier_wait(&barrier, 0);
    for (int i = threadIdx.x; i < maps * 128; i += blockDim.x)
        output[i] = tile[i / 128][i % 128];
}

void check(cudaError_t error) {
    if (error != cudaSuccess) throw std::runtime_error(cudaGetErrorString(error));
}

template <class Descriptors>
void exercise(const char* name, unsigned* input, unsigned* output, cudaStream_t stream) {
    constexpr int maps = map_count<Descriptors>;
    alignas(128) Descriptors descriptors{};
    const cuuint64_t dimensions[]{32, 4}, strides[]{128};
    const cuuint32_t box[]{32, 4}, steps[]{1, 1};
    for (int i = 0; i < maps; ++i) {
        const auto status = cuTensorMapEncodeTiled(
            &map_at(descriptors, i), CU_TENSOR_MAP_DATA_TYPE_UINT32, 2, input,
            dimensions, strides, box, steps, CU_TENSOR_MAP_INTERLEAVE_NONE,
            CU_TENSOR_MAP_SWIZZLE_NONE, CU_TENSOR_MAP_L2_PROMOTION_NONE,
            CU_TENSOR_MAP_FLOAT_OOB_FILL_NONE);
        if (status != CUDA_SUCCESS) throw std::runtime_error("tensor-map encoding failed");
    }
    cudaGraph_t graph{};
    cudaGraphExec_t executable{};
    for (bool capture : {false, true}) {
        if (capture) check(cudaStreamBeginCapture(stream, cudaStreamCaptureModeGlobal));
        transfer<<<1, 32, 0, stream>>>(descriptors, output);
        check(cudaGetLastError());
        if (capture) {
            check(cudaStreamEndCapture(stream, &graph));
            check(cudaGraphInstantiate(&executable, graph, 0));
            check(cudaGraphLaunch(executable, stream));
        }
        check(cudaStreamSynchronize(stream));
        std::array<unsigned, maps * 128 + maps> values{};
        check(cudaMemcpy(values.data(), output, sizeof(values), cudaMemcpyDeviceToHost));
        for (int i = 0; i < maps * 128; ++i)
            if (values[i] != 17 * (i % 128) + 3)
                throw std::runtime_error("TMA payload mismatch");
        for (int i = 0; i < maps; ++i)
            if (values[maps * 128 + i] != 0)
                throw std::runtime_error("TMA parameter alignment mismatch");
        std::printf("%s: %d descriptors, %d exact values, %s passed\n",
                    name, maps, maps * 128, capture ? "graph" : "direct");
    }
    check(cudaGraphExecDestroy(executable));
    check(cudaGraphDestroy(graph));
}
} // namespace

int main() {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) return 77;
    cudaDeviceProp properties{};
    if (cudaGetDeviceProperties(&properties, 0) != cudaSuccess || properties.major != 12)
        return 77;
    unsigned* input = nullptr;
    unsigned* output = nullptr;
    cudaStream_t stream{};
    int result = 0;
    try {
        std::array<unsigned, 128> values{};
        for (unsigned i = 0; i < values.size(); ++i) values[i] = 17 * i + 3;
        check(cudaMalloc(&input, sizeof(values)));
        check(cudaMalloc(&output, (4 * 128 + 4) * sizeof(unsigned)));
        check(cudaMemcpy(input, values.data(), sizeof(values), cudaMemcpyHostToDevice));
        check(cudaStreamCreate(&stream));
        exercise<Bf16TmaDescriptors>("bf16", input, output, stream);
        exercise<Fp8TmaDescriptors>("fp8", input, output, stream);
        exercise<Nvfp4A4TmaDescriptors>("nvfp4", input, output, stream);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        result = 1;
    }
    if (stream) cudaStreamDestroy(stream);
    if (output) cudaFree(output);
    if (input) cudaFree(input);
    return result;
}
