#include "fixture.cuh"
#include "mix_fixture.cuh"
#include "conditioning.cuh"

using namespace ninfer;
using namespace ninfer::bench;

int main(int argc, char** argv) {
    try {
        const auto options = hc::parse(argc, argv);
        measurement::hardware();
        auto& device = ninfer::bench::device_context();
        L2FlushBuffer flush(options.timing.flush_bytes);
        measurement::Csv csv(options.timing.csv_out, "alias,condition_us");
        for (int tokens : options.tokens) {
            const auto measure = [&](auto& f) {
                for (const auto& execution :
                     measurement::selections(options.timing.execution, "graph", "eager"))
                    for (const auto& cache :
                         measurement::selections(options.timing.cache, "cold", "warm")) {
                        const auto result = measurement::measure(
                            [&](CudaExecutionView cuda) { f.launch(cuda); },
                            [&](CudaExecutionView cuda) {
                                hc::condition(cuda, options.condition_us);
                                f.restore(cuda);
                            },
                            flush, device.execution_view(), options.timing, execution, cache);
                        measurement::Row row;
                        row.op              = hc::name(options.op);
                        row.format          = "bf16";
                        row.n               = hc::kWidth;
                        row.value_heads     = hc::kStreams;
                        row.tokens          = tokens;
                        row.batch           = options.batch;
                        row.workspace_bytes = f.workspace_capacity();
                        row.workspace_peak  = f.workspace_peak();
                        csv.write(row, result, execution, cache,
                                  std::string(options.in_place ? "inplace," : "disjoint,") +
                                      std::to_string(options.condition_us));
                    }
            };
            if (hc::mixes(options.op)) {
                hc::MixFixture f(options, tokens);
                measure(f);
            } else {
                hc::Fixture f(options, tokens);
                measure(f);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
