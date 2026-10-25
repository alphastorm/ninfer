#include "fixture.cuh"
using namespace ninfer;
using namespace ninfer::bench;

int main(int argc, char** argv) {
    try {
        const auto o = rmsnorm::parse(argc, argv);
        measurement::hardware();
        auto& device = ninfer::bench::device_context();
        L2FlushBuffer flush(o.timing.flush_bytes);
        measurement::Csv csv(o.timing.csv_out);
        for (int columns : o.tokens) {
            rmsnorm::Fixture f(o, columns);
            for (const auto& execution :
                 measurement::selections(o.timing.execution, "graph", "eager"))
                for (const auto& cache : measurement::selections(o.timing.cache, "cold", "warm")) {
                    const auto result = measurement::measure(
                        [&](CudaExecutionView cuda) { f.launch(cuda); }, [](auto) {}, flush,
                        device.execution_view(), o.timing, execution, cache);
                    measurement::Row row;
                    row.op          = rmsnorm::name(o.op);
                    row.format      = "bf16";
                    row.n           = o.dimension;
                    row.value_heads = o.heads;
                    row.tokens      = columns;
                    row.batch       = o.batch;
                    csv.write(row, result, execution, cache);
                }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
