#include "mix/oracle.h"
#include "core/decode_graph.h"

namespace {
using namespace ninfer;
using namespace ninfer::test::hc::mix;

int run(Weights& weights, std::span<const float> catalog, std::span<const Column> ref, int m,
        CudaExecutionView execution, bool captured = false) {
    Fixture f(weights, m);
    f.upload(catalog, execution.stream());
    DecodeGraphDefinition definition;
    DecodeGraphExecutable graph;
    if (captured) {
        definition.capture(execution.stream(), [&] { f.invoke(execution); });
        graph.instantiate(definition);
        graph.launch(execution.stream());
    } else
        f.invoke(execution);
    ninfer::test::cuda_synchronize(execution.stream());
    const auto label = std::string(name(weights.kind)) + " M=" + std::to_string(m) +
                       " pattern=" + std::to_string(int(weights.pattern)) +
                       " graph=" + std::to_string(captured);
    int failed = verify(f, ref, label) + f.check(label);
    if (captured) {
        const auto next     = input_catalog(weights.pattern, 1);
        const auto next_ref = reference(weights, next);
        f.upload(next, execution.stream());
        graph.launch(execution.stream());
        ninfer::test::cuda_synchronize(execution.stream());
        failed += verify(f, next_ref, label + " changed input") + f.check(label + " changed input");
    }
    return failed;
}

int suite(Kind kind, CudaExecutionView e) {
    Weights weights(kind, Pattern::Dense);
    const auto catalog = input_catalog(Pattern::Dense);
    const auto ref     = reference(weights, catalog);
    int failed         = 0;
    for (int m :
         {1,   2,   4,    5,    16,   31,   32,   33,   63,   64,   65,   95,   96,   97,   127,
          128, 129, 159,  160,  161,  191,  192,  193,  223,  224,  225,  255,  256,  257,  319,
          320, 321, 383,  384,  385,  447,  448,  449,  511,  512,  513,  671,  672,  673,  767,
          768, 769, 1024, 1199, 1200, 1201, 1359, 1360, 1361, 1536, 1631, 1632, 1633, 1792, 2048})
        failed += run(weights, catalog, ref, m, e);
    failed += run(weights, catalog, ref, 7 * 8, e, true);
    failed += run(weights, catalog, ref, 385, e, true);
    failed += validate(weights, ref, e);
    failed += weights.check();
    for (Pattern pattern :
         {Pattern::Zero, Pattern::InjectionOnly, Pattern::RowMapping, Pattern::Saturation}) {
        if (kind == Kind::Final && pattern == Pattern::InjectionOnly) continue;
        Weights structured(kind, pattern);
        const auto input    = input_catalog(pattern);
        const auto expected = reference(structured, std::span{input}.first(3 * kTotal));
        failed += run(structured, std::span{input}.first(3 * kTotal), expected, 3, e);
        if (pattern == Pattern::RowMapping) {
            for (int m : {65, 385, 769})
                failed += run(structured, std::span{input}.first(3 * kTotal), expected, m, e);
        }
        failed += structured.check();
    }
    std::cout << (failed ? "FAIL " : "OK ") << name(kind) << '\n';
    return failed;
}
} // namespace

int main() {
    if (ninfer::test::cuda_unavailable()) {
        std::cout << "SKIP: no CUDA device\n";
        return 77;
    }
    DeviceContext device;
    const auto e     = device.execution_view();
    const int failed = suite(Kind::Block, e) + suite(Kind::Final, e);
    return failed ? 1 : 0;
}
