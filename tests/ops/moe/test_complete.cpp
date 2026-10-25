#include "fixture.h"

namespace ninfer::test::moe {

int test_complete(Fixture& fixture, CudaExecutionView execution) {
    const auto& profile          = fixture.profile();
    const bool q8                = profile.routed.ffn.gate_up_type == QType::Q8_G32_FP16;
    const bool q5                = profile.routed.ffn.down_type == QType::Q5_G64_FP16;
    const std::vector<int> cases = q8   ? std::vector<int>{1, 2, 19, 20, 383, 384, 768}
                                   : q5 ? std::vector<int>{1, 2, 46, 47, 768, 4097}
                                        : std::vector<int>{1, 2, 46, 47, 768};
    int failures                 = 0;
    std::size_t witness          = 0;
    for (int tokens : cases) {
        const auto batch = fixture.batch(tokens, tokens == 1 ? 1 : 0);
        DeviceBatch input(batch);
        GuardedDeviceBuffer storage(batch.residual.size() * 2);
        storage.copy_from_host(batch.residual.data(), storage.bytes());
        Tensor output(storage.span(), DType::BF16, {kHidden, tokens});
        const auto bytes = ops::topk_softmax_swiglu_moe_workspace_capacity_bytes(
            profile, ops::MoeEpilogue::AddResidual, tokens, tokens);
        witness = std::max(witness, bytes);
        GuardedDeviceBuffer scratch(bytes);
        MemoryArena arena(scratch.span());
        const auto run = [&] {
            cuda_check(cudaMemcpyAsync(output.data, input.residual.data, output.bytes(),
                                       cudaMemcpyDeviceToDevice, execution.stream()),
                       "seed residual");
            ops::topk_softmax_swiglu_moe(input.x, profile, fixture.weights(),
                                         ops::MoeEpilogue::AddResidual, output, arena, execution);
        };
        if (q5 && tokens == 2) {
            cudaGraph_t graph;
            cudaGraphExec_t executable;
            cuda_check(cudaStreamBeginCapture(execution.stream(), cudaStreamCaptureModeGlobal),
                       "capture MoE");
            run();
            cuda_check(cudaStreamEndCapture(execution.stream(), &graph), "end MoE capture");
            cuda_check(cudaGraphInstantiate(&executable, graph, nullptr, nullptr, 0),
                       "instantiate MoE");
            for (int i = 0; i < 2; ++i) {
                cuda_check(cudaGraphLaunch(executable, execution.stream()), "replay MoE");
            }
            cuda_synchronize(execution.stream());
            cuda_check(cudaGraphExecDestroy(executable), "destroy MoE executable");
            cuda_check(cudaGraphDestroy(graph), "destroy MoE graph");
        } else {
            run();
            cuda_synchronize(execution.stream());
        }
        const std::string label = "complete MoE T=" + std::to_string(tokens);
        failures += verify_reduction(label, from_device_bf16(output.data, batch.complete.size()),
                                     batch.complete, kCompleteA16);
        failures += storage.verify_guards(label);
        failures += scratch.verify_guards(label + " scratch");
        failures +=
            verify_exact("complete input preservation",
                         from_device<std::uint16_t>(input.x.data, batch.input.size()), batch.input);
        if (arena.used() != 0 || arena.peak_used() != bytes) {
            std::cerr << label << " workspace mismatch\n";
            ++failures;
        }
        if (tokens == 1) {
            for (const auto device :
                 {Device::cpu(), Device::cuda(execution.device().index() + 1)}) {
                auto invalid   = input.x;
                invalid.device = device;
                bool rejected  = false;
                try {
                    ops::topk_softmax_swiglu_moe(invalid, profile, fixture.weights(),
                                                 ops::MoeEpilogue::Write, output, arena, execution);
                } catch (const std::invalid_argument&) { rejected = true; }
                if (!rejected) {
                    std::cerr << "accepted a mismatched device\n";
                    ++failures;
                }
            }
        }
    }
    if (ops::topk_softmax_swiglu_moe_workspace_capacity_bytes(
            profile, ops::MoeEpilogue::AddResidual, 1, cases.back()) != witness) {
        std::cerr << "complete MoE interval capacity mismatch\n";
        ++failures;
    }
    // Write cannot consume the old destination, including when it contains NaNs.
    for (int tokens : {1, q8 ? 20 : 47}) {
        auto batch = fixture.batch(tokens);
        DeviceBatch input(batch);
        DeviceBuffer buffer(execution.device(), batch.input.size() * 2);
        buffer.fill(0xff);
        Tensor output(buffer.span(), DType::BF16, {kHidden, tokens});
        DeviceBuffer scratch(execution.device(),
                             ops::topk_softmax_swiglu_moe_workspace_capacity_bytes(
                                 profile, ops::MoeEpilogue::Write, tokens, tokens));
        MemoryArena arena(scratch.span());
        ops::topk_softmax_swiglu_moe(input.x, profile, fixture.weights(), ops::MoeEpilogue::Write,
                                     output, arena, execution);
        cuda_synchronize(execution.stream());
        auto reference = batch.complete;
        for (std::size_t i = 0; i < reference.size(); ++i) {
            reference[i] -= bf16_to_f32(batch.residual[i]);
        }
        failures +=
            verify_reduction("complete Write", from_device_bf16(output.data, reference.size()),
                             reference, kCompleteA16);
    }
    return failures;
}
} // namespace ninfer::test::moe
