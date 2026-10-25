#include "fixture.cuh"

using namespace ninfer;
using namespace ninfer::bench;

int main(int argc, char** argv) {
    try {
        const auto options = ple::parse_options(argc, argv);
        measurement::hardware();
        auto& device = ninfer::bench::device_context();
        L2FlushBuffer flush(options.timing.flush_bytes);
        measurement::Csv csv(options.timing.csv_out, "offset,inplace");
        for (int t : options.tokens) {
            measurement::Row row;
            row.op            = options.op;
            row.format        = "bf16";
            row.tokens        = t;
            row.batch         = options.batch;
            row.conv_channels = ple::D;
            const auto run    = [&](auto& fixture) {
                row.valid = ple::csv_list(fixture.counts);
                for (const auto& execution :
                     measurement::selections(options.timing.execution, "graph", "eager"))
                    for (const auto& cache :
                         measurement::selections(options.timing.cache, "cold", "warm")) {
                        const auto result = measurement::measure(
                            [&](CudaExecutionView cuda) { fixture.launch(cuda); },
                            [&](CudaExecutionView cuda) { fixture.restore(cuda); }, flush,
                            device.execution_view(), options.timing, execution, cache);
                        csv.write(row, result, execution, cache,
                                     std::to_string(options.offset) + "," +
                                         std::to_string(options.inplace));
                    }
            };
            if (options.op == "gate") {
                ple::GateFixture fixture(options, t);
                run(fixture);
            } else if (options.op == "state-extract") {
                ple::ExtractFixture fixture(options, t);
                row.commits = ple::csv_list(fixture.commits);
                run(fixture);
            } else {
                ple::ConvFixture fixture(options, t);
                run(fixture);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
