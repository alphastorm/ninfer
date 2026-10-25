#include "ninfer/ops/gated_residual.h"
#include "ops/gated_residual/kernel/norm.cuh"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ninfer::ops::detail::hc {
namespace {
enum class NormSchedule { Pairs256, Vectors320, StreamingVectors320 };

constexpr int kExpandVectorStart        = 2048;
constexpr int kGroupedStreamingStart    = 2816;
constexpr int kDisjointStreamingStart   = 1536;
constexpr int kDisjointMaterializeStart = 1841;
constexpr int kDisjointMaterializeEnd   = 3072;
constexpr int kInPlaceMaterializeStart  = 1824;
constexpr int kInPlaceMaterializeEnd    = 3680;

template <Form Kind, NormSchedule Schedule = NormSchedule::Pairs256>
void launch_norm_rows(Operands o, CudaExecutionView e) {
    const std::int64_t rows = std::int64_t(o.columns) * kStreams;
    for (std::int64_t first = 0; first < rows;) {
        const auto count = std::min(rows - first, std::int64_t(std::numeric_limits<int>::max()));
        if constexpr (Schedule != NormSchedule::Pairs256)
            norm_vector_kernel<Kind, Schedule == NormSchedule::StreamingVectors320>
                <<<unsigned(count), 320, 0, e.stream()>>>(o, first);
        else
            norm_kernel<Kind, 256><<<unsigned(count), 256, 0, e.stream()>>>(o, first);
        CUDA_CHECK(cudaGetLastError());
        first += count;
    }
}

template <Form Kind>
void launch_combine(Operands o, CudaExecutionView e) {
    constexpr int block                = 256;
    const std::int64_t vectors         = std::int64_t(o.columns) * kStreams * (kWidth / 8);
    constexpr std::int64_t max_vectors = std::int64_t(std::numeric_limits<int>::max()) * block;
    for (std::int64_t first = 0; first < vectors;) {
        const auto count = std::min(vectors - first, max_vectors);
        combine_vector_kernel<Kind, block>
            <<<unsigned((count + block - 1) / block), block, 0, e.stream()>>>(o, first, count);
        CUDA_CHECK(cudaGetLastError());
        first += count;
    }
}

template <Form Kind>
void launch_materialized_norm(Operands o, CudaExecutionView execution) {
    launch_combine<Kind == Form::AddNorm ? Form::AddNorm : Form::Combine>(o, execution);
    o.input = o.residual;
    // The freshly produced residual uses the paired norm, without input eviction.
    launch_norm_rows<Form::GroupedNorm>(o, execution);
}

using Launch = void (*)(Operands, CudaExecutionView);

template <Form Kind>
Launch select_combine_norm(Operands o) {
    const bool in_place = o.input == o.residual;
    if (!in_place && o.columns >= kDisjointStreamingStart && o.columns < kDisjointMaterializeStart)
        return launch_norm_rows<Kind, NormSchedule::StreamingVectors320>;
    const bool materialize =
        in_place ? o.columns >= kInPlaceMaterializeStart && o.columns <= kInPlaceMaterializeEnd
                 : o.columns >= kDisjointMaterializeStart && o.columns <= kDisjointMaterializeEnd;
    return materialize ? launch_materialized_norm<Kind> : launch_norm_rows<Kind>;
}

Launch select_implementation(Form form, Operands o) {
    switch (form) {
    case Form::ExpandNorm:
        return o.columns >= kExpandVectorStart
                   ? launch_norm_rows<Form::ExpandNorm, NormSchedule::Vectors320>
                   : launch_norm_rows<Form::ExpandNorm>;
    case Form::GroupedNorm:
        return o.columns >= kGroupedStreamingStart
                   ? launch_norm_rows<Form::GroupedNorm, NormSchedule::StreamingVectors320>
                   : launch_norm_rows<Form::GroupedNorm>;
    case Form::Combine:
        return launch_combine<Form::Combine>;
    case Form::CombineNorm:
        return select_combine_norm<Form::CombineNorm>(o);
    case Form::AddNorm:
        return select_combine_norm<Form::AddNorm>(o);
    }
    throw std::logic_error("gated residual: invalid form");
}

void execute(Form form, const Tensor& input, const Tensor* block, const Tensor* injection,
             const Tensor* weight, float epsilon, Tensor* residual, Tensor* normalized,
             CudaExecutionView execution, const char* name) {
    const auto operands = validate(form, input, block, injection, weight, epsilon, residual,
                                   normalized, execution, name);
    select_implementation(form, operands)(operands, execution);
}
} // namespace
} // namespace ninfer::ops::detail::hc

namespace ninfer::ops {
namespace hc = detail::hc;

void hc_expand_norm(const Tensor& embedding, const Tensor& weight, float eps, Tensor& residual,
                    Tensor& normalized, CudaExecutionView execution) {
    hc::execute(hc::Form::ExpandNorm, embedding, nullptr, nullptr, &weight, eps, &residual,
                &normalized, execution, "hc_expand_norm");
}

void hc_grouped_rmsnorm(const Tensor& residual, const Tensor& weight, float eps, Tensor& normalized,
                        CudaExecutionView execution) {
    hc::execute(hc::Form::GroupedNorm, residual, nullptr, nullptr, &weight, eps, nullptr,
                &normalized, execution, "hc_grouped_rmsnorm");
}

void hc_combine(const Tensor& residual, const Tensor& block, const Tensor& injection,
                Tensor& updated, CudaExecutionView execution) {
    hc::execute(hc::Form::Combine, residual, &block, &injection, nullptr, 0, &updated, nullptr,
                execution, "hc_combine");
}

void hc_combine_norm(const Tensor& residual, const Tensor& block, const Tensor& injection,
                     const Tensor& weight, float eps, Tensor& updated, Tensor& normalized,
                     CudaExecutionView execution) {
    hc::execute(hc::Form::CombineNorm, residual, &block, &injection, &weight, eps, &updated,
                &normalized, execution, "hc_combine_norm");
}

void hc_combine_norm(const Tensor& residual, const Tensor& block, const Tensor& weight, float eps,
                     Tensor& updated, Tensor& normalized, CudaExecutionView execution) {
    hc::execute(hc::Form::AddNorm, residual, &block, nullptr, &weight, eps, &updated, &normalized,
                execution, "hc_combine_norm");
}
} // namespace ninfer::ops
