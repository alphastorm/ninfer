#include "ninfer/ops/copy_proposals.h"
#include "cuda_fixture.h"
#include "core/decode_graph.h"
#include "ops/op_tester.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

using namespace ninfer;
using namespace ninfer::test;

namespace {
int run_case(int k, int batch, int domain) {
    DeviceContext context;
    std::vector<std::int32_t> copies(k * batch), rows(batch);
    auto d_copies = to_device(copies);
    auto d_rows = to_device(rows);
    GuardedDeviceBuffer d_drafts(copies.size() * sizeof(std::int32_t));
    GuardedDeviceBuffer d_candidates(copies.size() * 16 * sizeof(std::int32_t));
    GuardedDeviceBuffer d_q(copies.size() * 16 * sizeof(float));
    Tensor source(d_copies.span(), DType::I32, {k, batch});
    Tensor selected(d_rows.span(), DType::I32, {batch});
    Tensor drafts(d_drafts.span(), DType::I32, {k, batch});
    Tensor candidates(d_candidates.span(), DType::I32, {16, k, batch});
    Tensor q(d_q.span(), DType::FP32, {16, k, batch});
    const auto launch = [&] {
        ops::overlay_copy_proposals(source, selected, domain, drafts, candidates, q,
                                     context.execution_view());
    };
    DecodeGraphDefinition definition;
    DecodeGraphExecutable graph;
    definition.capture(context.stream(), launch);
    graph.instantiate(definition);
    int failures = 0;
    for (int replay = 0; replay < 2; ++replay) {
        for (int mask = 0; mask < (1 << batch); ++mask) {
            std::vector<std::int32_t> expected_drafts(copies.size(), -3);
            std::vector<std::int32_t> expected_ids(copies.size() * 16, -7);
            std::vector<float> expected_q(copies.size() * 16, -2.0F);
            for (int row = 0; row < batch; ++row) {
                rows[row] = (mask >> row) & 1;
                for (int column = 0; column < k; ++column)
                    copies[row * k + column] = domain - 1 - (column + replay) % 16;
            }
            d_drafts.copy_from_host(expected_drafts.data(), d_drafts.bytes());
            d_candidates.copy_from_host(expected_ids.data(), d_candidates.bytes());
            d_q.copy_from_host(expected_q.data(), d_q.bytes());
            CUDA_CHECK(cudaMemcpyAsync(d_copies.data(), copies.data(), d_copies.size(),
                                       cudaMemcpyHostToDevice, context.stream()));
            CUDA_CHECK(cudaMemcpyAsync(d_rows.data(), rows.data(), d_rows.size(),
                                       cudaMemcpyHostToDevice, context.stream()));
            if (replay) graph.launch(context.stream()); else launch();
            context.synchronize();
            for (int row = 0; row < batch; ++row) {
                if (!rows[row]) continue;
                for (int column = 0; column < k; ++column) {
                    const auto index = row * k + column;
                    expected_drafts[index] = copies[index];
                    for (int candidate = 0; candidate < 16; ++candidate) {
                        expected_ids[index * 16 + candidate] = static_cast<std::int32_t>(
                            (static_cast<std::int64_t>(copies[index]) + candidate) % domain);
                        expected_q[index * 16 + candidate] = candidate == 0 ? 1.0F : 0.0F;
                    }
                }
            }
            failures += verify_exact("copy drafts", from_device<std::int32_t>(d_drafts.data(), copies.size()), expected_drafts);
            failures += verify_exact("copy law ids", from_device<std::int32_t>(d_candidates.data(), expected_ids.size()), expected_ids);
            failures += verify_exact("copy law q", from_device<float>(d_q.data(), expected_q.size()), expected_q);
            failures += verify_exact("copies read only", from_device<std::int32_t>(d_copies, copies.size()), copies);
            failures += verify_exact("rows read only", from_device<std::int32_t>(d_rows, rows.size()), rows);
        }
    }
    failures += d_drafts.verify_guards("copy drafts");
    failures += d_candidates.verify_guards("copy candidates");
    failures += d_q.verify_guards("copy q");
    bool rejected_alias = false;
    try {
        ops::overlay_copy_proposals(drafts, selected, domain, drafts, candidates, q,
                                     context.execution_view());
    } catch (const std::invalid_argument&) { rejected_alias = true; }
    if (!rejected_alias) ++failures;
    bool rejected_domain = false;
    try {
        ops::overlay_copy_proposals(source, selected, 15, drafts, candidates, q,
                                     context.execution_view());
    } catch (const std::invalid_argument&) { rejected_domain = true; }
    if (!rejected_domain) ++failures;
    return failures;
}
} // namespace

int main() {
    if (cuda_unavailable()) return 77;
    int failures = 0;
    for (int k = 1; k <= 15; ++k)
        for (int batch : {1, 2})
            for (int domain : {16, 257, std::numeric_limits<std::int32_t>::max()})
                failures += run_case(k, batch, domain);
    std::cout << "copy_proposals: " << (failures ? "FAIL" : "PASS") << '\n';
    return failures ? 1 : 0;
}
