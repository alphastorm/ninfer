#include "ops/linear/kernels/group64_a8/mma.cuh"
#include "core/layout.h"

#include <stdexcept>
#include <utility>

namespace ninfer::ops::detail::group64_a8 {
namespace {

std::size_t padded_tokens(int tokens) {
    return (static_cast<std::size_t>(tokens) + 511) / 512 * 512;
}

template <class Arena>
auto allocate(Arena& workspace, int k, int tokens) {
    const auto padded = padded_tokens(tokens);
    const auto codes = workspace.alloc_bytes(padded * k, 256);
    const auto scales = workspace.alloc_bytes(padded * (k / 64) * sizeof(float), 256);
    return std::pair{codes, scales};
}

struct Activations {
    std::int8_t* codes;
    float* scales;
};

template <int K, int BN>
Activations prepare(const Tensor& x, MemoryArena& workspace, cudaStream_t stream) {
    const auto [codes, scales] = allocate(workspace, K, x.ne[1]);
    Activations result{static_cast<std::int8_t*>(codes.data), static_cast<float*>(scales.data)};
    const auto padded = (static_cast<std::size_t>(x.ne[1]) + BN - 1) / BN * BN;
    quantize<K, BN><<<static_cast<unsigned>(padded), 128, 0, stream>>>(
        static_cast<const __nv_bfloat16*>(x.data), x.ne[1], result.codes, result.scales);
    CUDA_CHECK(cudaGetLastError());
    return result;
}

template <bool High, int K, int NT, bool Paired, class Epilogue>
void project(const Weight& w, Activations x, int tokens, int row_begin, int rows,
             Epilogue epilogue, cudaStream_t stream) {
    constexpr int BN = 32 * NT;
    constexpr auto kernel = gemm<High, K, NT, Paired, Epilogue>;
    constexpr int bytes = shared_bytes<High, K, NT, Paired>();
    const int dynamic = common::prepare_shared<bytes, kernel, true>();
    const dim3 grid(static_cast<unsigned>(rows / 64),
                    static_cast<unsigned>((static_cast<std::size_t>(tokens) + BN - 1) / BN));
    kernel<<<grid, kThreads, dynamic, stream>>>(
        static_cast<const std::uint8_t*>(w.qdata), static_cast<const std::uint8_t*>(w.qhigh),
        static_cast<const __half*>(w.scales), x.codes, x.scales, tokens, row_begin, rows, epilogue);
    CUDA_CHECK(cudaGetLastError());
}

template <int K, int NT, class Epilogue>
void project_format(const Weight& w, Activations x, int tokens, int row_begin, int rows,
                    Epilogue epilogue, cudaStream_t stream) {
    if (w.qtype == QType::Q4_G64_FP16)
        project<false, K, NT, false>(w, x, tokens, row_begin, rows, epilogue, stream);
    else if (w.qtype == QType::Q5_G64_FP16)
        project<true, K, NT, false>(w, x, tokens, row_begin, rows, epilogue, stream);
    else throw std::invalid_argument("group64 A8 requires Q4/Q5 weights");
}

template <class Function>
void select_tile(int tokens, Function&& launch) {
    if (tokens <= 128) launch.template operator()<4>();
    else if (tokens <= 256) launch.template operator()<8>();
    else launch.template operator()<16>();
}

template <int K>
void add_k(const Tensor& x, const Weight& w, Tensor& out, MemoryArena& ws, cudaStream_t stream) {
    select_tile(x.ne[1], [&]<int NT>() {
        const auto activations = prepare<K, 32 * NT>(x, ws, stream);
        project_format<K, NT>(w, activations, x.ne[1], 0, w.n,
                             Add{static_cast<__nv_bfloat16*>(out.data), w.n}, stream);
    });
}

} // namespace

std::size_t workspace_bytes(int k, int tokens) {
    if (k <= 0 || k % 64 || tokens <= 0)
        throw std::invalid_argument("group64 A8 workspace: invalid geometry");
    WorkspaceLayoutBuilder layout;
    (void)allocate(layout, k, tokens);
    return layout.peak_bytes(1);
}

void add(const Tensor& x, const Weight& w, Tensor& out, MemoryArena& ws, cudaStream_t stream) {
    auto scope = ws.scope();
    if (w.n != 5120 || (w.k != 6144 && w.k != 17408))
        throw std::invalid_argument("group64 A8 add: unsupported geometry");
    if (w.k == 6144) add_k<6144>(x, w, out, ws, stream);
    else add_k<17408>(x, w, out, ws, stream);
}

void swiglu(const Tensor& x, const Weight& w, Tensor& out, MemoryArena& ws, cudaStream_t stream) {
    if (w.qtype != QType::Q4_G64_FP16 || w.n != 34816 || w.k != 5120)
        throw std::invalid_argument("group64 A8 SwiGLU: unsupported geometry");
    auto scope = ws.scope();
    const auto activations = prepare<5120, 128>(x, ws, stream);
    project<false, 5120, 4, true>(w, activations, x.ne[1], 0, out.ne[0],
        SwiGlu{static_cast<__nv_bfloat16*>(out.data), out.ne[0]}, stream);
}

void split(const Tensor& x, const Weight& first, const Weight& second,
           std::span<const Destination> first_outputs, std::span<const Destination> second_outputs,
           MemoryArena& ws, cudaStream_t stream) {
    if (first.k != 5120 || second.k != 5120)
        throw std::invalid_argument("group64 A8 split: unsupported input geometry");
    auto scope = ws.scope();
    select_tile(x.ne[1], [&]<int NT>() {
        const auto activations = prepare<5120, 32 * NT>(x, ws, stream);
        const auto write = [&](const Weight& weight, std::span<const Destination> outputs) {
            for (const auto& output : outputs) {
                if (output.count <= 0 || output.count % 64 || output.weight_row < 0 ||
                    output.weight_row + output.count > weight.n || output.offset < 0 ||
                    output.offset + output.count > output.rows)
                    throw std::invalid_argument("group64 A8 split: invalid destination");
                project_format<5120, NT>(weight, activations, x.ne[1], output.weight_row,
                                        output.count, Store{output}, stream);
            }
        };
        write(first, first_outputs);
        write(second, second_outputs);
    });
}

} // namespace ninfer::ops::detail::group64_a8
