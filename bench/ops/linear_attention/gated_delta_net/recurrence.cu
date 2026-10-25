// Public GDN and production-stage timing. Private launchers also measure the prefill
// recurrent/chunked crossover, independently of decode/ReplaySSM.
// Graph/eager timing and cache conditioning use the shared Op measurement harness.
#include "ops/linear_attention/gated_delta_net/recurrence/plan.h"
#include "core/buffer.h"
#include "ninfer/ops/gated_delta_net.h"
#include "ninfer/ops/l2norm.h"
#include "op_measurement.cuh"
#include "recurrence_fixture.cuh"
#include "ops/linear_attention/gated_delta_net/recurrence/chunked/launch.h"
#include "ops/linear_attention/gated_delta_net/recurrence/launch.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ninfer::bench::gdn_recurrence {
namespace gated_delta_net_detail = ninfer::ops::detail::gated_delta_net;
namespace chunked_detail         = ninfer::ops::detail::gated_delta_net::chunked;

struct BenchRow {
    const char* normalization = "";
    std::string implementation;
    std::int32_t full_chunks          = 0;
    std::int32_t tail_tokens          = 0;
    std::size_t workspace_bytes       = 0;
    double logical_bytes              = 0.0;
    double traffic_bytes              = 0.0;
    double intermediate_traffic_bytes = 0.0;
    double stage_share_pct            = -1.0;
    double relative_to_e2e_pct        = -1.0;
    measurement::Result measured;
    std::size_t workspace_peak = 0;
};

struct TrafficBytes {
    double total        = 0.0;
    double intermediate = 0.0;
};

template <class Launch>
measurement::Result measure_operation(
    Launch& launch, L2FlushBuffer& flush, CudaExecutionView cuda, const Options& options,
    const launch_fn& restore = [](CudaExecutionView) {}) {
    return measurement::measure(launch, restore, flush, cuda, options.timing,
                                options.timing.execution, options.timing.cache);
}

float gated_delta_net_scale() { return 1.0F / std::sqrt(128.0F); }

double running_logical_bytes(const Problem& problem) {
    const double tokens   = static_cast<double>(problem.tokens);
    const double batch    = static_cast<double>(problem.batch);
    const double qk_bytes = static_cast<double>(gated_delta_net_detail::kStateDim) *
                            problem.qk_heads * tokens * batch * sizeof(std::uint16_t);
    const double value_bytes = static_cast<double>(gated_delta_net_detail::kStateDim) *
                               problem.value_heads * tokens * batch * sizeof(std::uint16_t);
    const double gate_bytes =
        static_cast<double>(problem.value_heads) * tokens * batch * sizeof(float);
    const double state_bytes = static_cast<double>(gated_delta_net_detail::kStateDim) *
                               gated_delta_net_detail::kStateDim * problem.value_heads * batch *
                               sizeof(float);
    return 2.0 * qk_bytes + 2.0 * value_bytes + 2.0 * gate_bytes + 2.0 * state_bytes;
}

double batch_update_logical_bytes(const Problem& problem) {
    // The fixture passes the same selector tensor for source and destination slots.
    return running_logical_bytes(problem) + double(problem.batch) * sizeof(std::int32_t);
}

double qk_tensor_bytes(const Problem& problem) {
    return static_cast<double>(gated_delta_net_detail::kStateDim) * problem.qk_heads *
           problem.tokens * problem.batch * sizeof(std::uint16_t);
}

double value_tensor_bytes(const Problem& problem) {
    return static_cast<double>(gated_delta_net_detail::kStateDim) * problem.value_heads *
           problem.tokens * problem.batch * sizeof(std::uint16_t);
}

double gate_tensor_bytes(const Problem& problem) {
    return static_cast<double>(problem.value_heads) * problem.tokens * problem.batch *
           sizeof(float);
}

double state_tensor_bytes(const Problem& problem) {
    return static_cast<double>(gated_delta_net_detail::kStateDim) *
           gated_delta_net_detail::kStateDim * problem.value_heads * problem.batch * sizeof(float);
}

TrafficBytes prepare_traffic(const Problem& p, std::size_t workspace) {
    return {2.0 * qk_tensor_bytes(p) * (p.value_heads / p.qk_heads) + 2.0 * gate_tensor_bytes(p) +
                workspace,
            double(workspace)};
}

TrafficBytes recurrence_traffic(const Problem& p, int slices) {
    const double packets =
        double(chunked_detail::chunk_count(p.tokens)) * p.value_heads *
        (sizeof(chunked_detail::QkChunk) + sizeof(chunked_detail::ControlChunk)) * slices;
    return {packets + 2.0 * value_tensor_bytes(p) + 2.0 * state_tensor_bytes(p), packets};
}

TrafficBytes batch_update_traffic(const Problem& problem, bool composed) {
    const double logical = batch_update_logical_bytes(problem);
    if (!composed) { return {logical, 0.0}; }
    const double normalized_qk_round_trip = 4.0 * qk_tensor_bytes(problem);
    return {
        logical + normalized_qk_round_trip,
        normalized_qk_round_trip,
    };
}

BenchRow run_batch_update(const Options& options, std::int32_t tokens, bench::L2FlushBuffer& flush,
                          CudaExecutionView cuda) {
    const Problem problem{options.qk_heads, options.value_heads, tokens, options.batch};
    Operands operands(problem, false);

    const std::size_t qk_elements = static_cast<std::size_t>(gated_delta_net_detail::kStateDim) *
                                    problem.qk_heads * tokens * problem.batch;
    const std::size_t state_elements = static_cast<std::size_t>(gated_delta_net_detail::kStateDim) *
                                       gated_delta_net_detail::kStateDim * problem.value_heads;
    const std::int32_t slots = problem.batch;
    DeviceBuffer states      = bench::make_f32(state_elements * slots, 211U, -.02F, .02F);
    SavedBuffer initial_states(states);
    const auto restore = [&](CudaExecutionView cuda) { initial_states.restore(cuda); };
    std::vector<std::int32_t> state_slots_host(static_cast<std::size_t>(problem.batch));
    for (std::int32_t row = 0; row < problem.batch; ++row) {
        state_slots_host[static_cast<std::size_t>(row)] = row;
    }
    DeviceBuffer state_slots(ninfer::bench::device_context().device(),
                             state_slots_host.size() * sizeof(std::int32_t));
    state_slots.copy_from_host(state_slots_host.data(), state_slots.size());
    DeviceBuffer q_normalized;
    DeviceBuffer k_normalized;
    if (options.qk_norm == "composed") {
        q_normalized = make_zeros(qk_elements * sizeof(std::uint16_t));
        k_normalized = make_zeros(qk_elements * sizeof(std::uint16_t));
    }

    Tensor q    = operands.query();
    Tensor k    = operands.key();
    Tensor v    = operands.value();
    Tensor g    = operands.gate();
    Tensor beta = operands.beta_tensor();
    Tensor out  = operands.output();
    Tensor ssm_states(states.span(), DType::FP32,
                      {gated_delta_net_detail::kStateDim, gated_delta_net_detail::kStateDim,
                       problem.value_heads, slots});
    Tensor selected_slots(state_slots.span(), DType::I32, {problem.batch});
    Tensor q_norm;
    Tensor k_norm;
    if (options.qk_norm == "composed") {
        q_norm =
            Tensor(q_normalized.span(), DType::BF16,
                   {gated_delta_net_detail::kStateDim, problem.qk_heads, tokens, problem.batch});
        k_norm =
            Tensor(k_normalized.span(), DType::BF16,
                   {gated_delta_net_detail::kStateDim, problem.qk_heads, tokens, problem.batch});
    }

    const bool composed = options.qk_norm == "composed";
    auto launch         = [&](CudaExecutionView cuda) {
        if (composed) {
            ops::l2norm(q, kQkNormEpsilon, q_norm, cuda);
            ops::l2norm(k, kQkNormEpsilon, k_norm, cuda);
        }
        const Tensor& q_input = composed ? q_norm : q;
        const Tensor& k_input = composed ? k_norm : k;
        ops::gated_delta_net_batch_update(q_input, k_input, v, g, beta, gated_delta_net_scale(),
                                                  !composed, ssm_states, selected_slots, selected_slots,
                                                  out, cuda);
    };
    const auto measured        = measure_operation(launch, flush, cuda, options, restore);
    const TrafficBytes traffic = batch_update_traffic(problem, composed);

    return {
        composed ? "composed" : "fused",
        composed ? "l2norm_x2+public.batch_update.qk_pre_normalized"
                 : "public.batch_update.qk_fused",
        0,
        0,
        0,
        batch_update_logical_bytes(problem),
        traffic.total,
        traffic.intermediate,
        -1.0,
        -1.0,
        measured,
    };
}

std::vector<BenchRow> run_prefill(const Options& options, std::int32_t tokens,
                                  bench::L2FlushBuffer& flush, CudaExecutionView execution) {
    constexpr auto state_dim = gated_delta_net_detail::kStateDim;
    const bool normalize     = options.mode != Mode::ChunkedNormalized;
    const bool force_chunked =
        options.mode == Mode::ChunkedRaw || options.mode == Mode::ChunkedNormalized;
    const bool force_recurrent = options.mode == Mode::Recurrent;
    const bool chunked =
        force_chunked ||
        (!force_recurrent && gated_delta_net_detail::select_recurrence_route(tokens) ==
                                 gated_delta_net_detail::RecurrenceRoute::Chunked);
    const Problem problem{options.qk_heads, options.value_heads, tokens};
    Operands operands(problem, !normalize);
    const std::size_t state_elements = std::size_t(state_dim) * state_dim * problem.value_heads;
    DeviceBuffer state_in            = bench::make_f32(state_elements, 211U, -.02F, .02F);
    DeviceBuffer state_out           = make_zeros(state_elements * sizeof(float));
    Tensor q = operands.query(), k = operands.key(), v = operands.value();
    Tensor g = operands.gate(), beta = operands.beta_tensor(), out = operands.output();
    Tensor si(state_in.span(), DType::FP32, {state_dim, state_dim, problem.value_heads});
    Tensor so(state_out.span(), DType::FP32, {state_dim, state_dim, problem.value_heads});
    const auto layout =
        chunked_detail::workspace_layout(problem.qk_heads, problem.value_heads, tokens);
    std::size_t bytes = 0;
    if (force_chunked) {
        bytes = layout.total_bytes;
    } else if (!force_recurrent) {
        bytes = ops::gated_delta_net_workspace_capacity_bytes(problem.qk_heads, problem.value_heads,
                                                              tokens, tokens);
    }
    ninfer::DeviceBuffer workspace_storage(ninfer::bench::device_context().device(),
                                           std::max<std::size_t>(256, bytes));
    MemoryArena workspace(workspace_storage.span());
    const auto backing = workspace.storage().subspan(0, bytes);
    auto* qk =
        chunked ? static_cast<chunked_detail::QkChunk*>(layout.qk.bind(backing).data) : nullptr;
    auto* control =
        chunked ? static_cast<chunked_detail::ControlChunk*>(layout.control.bind(backing).data)
                : nullptr;
    const gated_delta_net_detail::RecurrenceOperands args{static_cast<const __nv_bfloat16*>(q.data),
                                                          static_cast<const __nv_bfloat16*>(k.data),
                                                          static_cast<const __nv_bfloat16*>(v.data),
                                                          static_cast<const float*>(g.data),
                                                          static_cast<const float*>(beta.data),
                                                          static_cast<const float*>(si.data),
                                                          static_cast<float*>(so.data),
                                                          static_cast<__nv_bfloat16*>(out.data),
                                                          problem.qk_heads,
                                                          problem.value_heads,
                                                          tokens,
                                                          gated_delta_net_scale()};
    const int tile              = chunked ? ops::detail::gated_delta_net::select_value_tile(
                                   problem.value_heads, execution.multiprocessor_count())
                                          : 0;
    const auto prep_io          = prepare_traffic(problem, bytes);
    const auto rec_io           = recurrence_traffic(problem, tile ? state_dim / tile : 0);
    const TrafficBytes total_io = chunked ? TrafficBytes{prep_io.total + rec_io.total,
                                                         prep_io.intermediate + rec_io.intermediate}
                                          : TrafficBytes{running_logical_bytes(problem), 0};
    auto prepare                = [&](CudaExecutionView cuda) {
        const auto stream = cuda.stream();
        chunked_detail::launch_prepare(args, qk, control, normalize, stream);
    };
    auto recurrence = [&](CudaExecutionView cuda) {
        const auto stream = cuda.stream();
        chunked_detail::launch_recurrence(args, qk, control, tile, stream);
    };
    auto pipeline = [&](CudaExecutionView cuda) {
        const auto stream = cuda.stream();
        if (force_chunked) {
            prepare(cuda);
            recurrence(cuda);
        } else if (force_recurrent)
            gated_delta_net_detail::launch_recurrent_inout(
                q, k, v, g, beta, gated_delta_net_scale(), normalize, si, so, out, stream);
        else
            ops::gated_delta_net(q, k, v, g, beta, gated_delta_net_scale(), normalize, workspace,
                                 si, so, out, cuda);
    };
    std::vector<BenchRow> rows;
    auto measure = [&](const char* name, TrafficBytes io, auto& body, double logical) {
        const auto measured = measure_operation(body, flush, execution, options);
        rows.push_back({normalize ? "fused" : "pre_normalized", name,
                        chunked ? tokens / chunked_detail::kChunkSize : 0,
                        chunked ? tokens % chunked_detail::kChunkSize : 0, bytes, logical, io.total,
                        io.intermediate, -1.0, -1.0, measured,
                        force_chunked ? bytes : workspace.peak_used()});
    };
    measure(force_chunked     ? "forced.chunked"
            : force_recurrent ? "forced.recurrent"
                              : "public.total",
            total_io, pipeline, running_logical_bytes(problem));
    if (options.breakdown && chunked) {
        measure("chunked.prepare", prep_io, prepare, 0);
        measure("chunked.recurrence", rec_io, recurrence, 0);
        const double sum = rows[1].measured.gpu.median_us + rows[2].measured.gpu.median_us;
        for (int i = 1; i < 3; ++i) {
            rows[i].stage_share_pct = 100.0 * rows[i].measured.gpu.median_us / sum;
            rows[i].relative_to_e2e_pct =
                100.0 * rows[i].measured.gpu.median_us / rows[0].measured.gpu.median_us;
        }
    }
    return rows;
}

} // namespace ninfer::bench::gdn_recurrence

int main(int argc, char** argv) {
    using namespace ninfer;
    using namespace ninfer::bench;
    using namespace ninfer::bench::gdn_recurrence;
    try {
        const auto requested = parse_options(argc, argv);
        measurement::hardware();
        auto& device = ninfer::bench::device_context();
        L2FlushBuffer flush(requested.timing.flush_bytes);
        measurement::Csv csv(
            requested.timing.csv_out,
            "normalization,full_chunks,tail_tokens,logical_bytes,tensor_io_request_bytes,workspace_"
            "request_bytes,stage_share_pct,relative_to_total_pct");
        for (const auto& execution :
             measurement::selections(requested.timing.execution, "graph", "eager"))
            for (const auto& cache :
                 measurement::selections(requested.timing.cache, "cold", "warm")) {
                auto options             = requested;
                options.timing.execution = execution;
                options.timing.cache     = cache;
                for (const int t : options.tokens) {
                    const auto rows = options.mode == Mode::BatchUpdate
                                          ? std::vector<BenchRow>{run_batch_update(
                                                options, t, flush, device.execution_view())}
                                          : run_prefill(options, t, flush, device.execution_view());
                    for (const auto& r : rows) {
                        measurement::Row row;
                        row.op              = r.implementation;
                        row.tokens          = t;
                        row.batch           = options.batch;
                        row.qk_heads        = options.qk_heads;
                        row.value_heads     = options.value_heads;
                        row.workspace_bytes = r.workspace_bytes;
                        row.workspace_peak  = r.workspace_peak;
                        const auto extra    = std::string(r.normalization) + "," +
                                           std::to_string(r.full_chunks) + "," +
                                           std::to_string(r.tail_tokens) + "," +
                                           std::to_string(r.logical_bytes) + "," +
                                           std::to_string(r.traffic_bytes) + "," +
                                           std::to_string(r.intermediate_traffic_bytes) + "," +
                                           std::to_string(r.stage_share_pct) + "," +
                                           std::to_string(r.relative_to_e2e_pct);
                        csv.write(row, r.measured, execution, cache, extra);
                    }
                }
            }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
