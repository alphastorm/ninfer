#include "replay_fixture.cuh"

using namespace ninfer;
using namespace ninfer::bench;

int main(int argc, char** argv) {
    try {
        const auto options = gdn_replay::parse_options(argc, argv);
        measurement::hardware();
        auto& device = ninfer::bench::device_context();
        L2FlushBuffer flush(options.timing.flush_bytes);
        measurement::Csv csv(options.timing.csv_out);
        for (int t : options.tokens) {
            measurement::Row row;
            row.op            = options.op;
            row.tokens        = t;
            row.batch         = options.batch;
            row.conv_channels = options.op == "fold" ? options.profile.conv_channels : 0;
            row.qk_heads      = 16;
            row.value_heads   = options.profile.value_heads;
            row.layers        = options.op == "fold" ? options.profile.layers : 1;
            const auto run    = [&](auto&& launch, auto&& restore) {
                for (const auto& execution :
                     measurement::selections(options.timing.execution, "graph", "eager"))
                    for (const auto& cache :
                         measurement::selections(options.timing.cache, "cold", "warm")) {
                        const auto r =
                            measurement::measure(launch, restore, flush, device.execution_view(),
                                                 options.timing, execution, cache);
                        csv.write(row, r, execution, cache);
                    }
            };
            if (options.op == "fold") {
                gdn_replay::FoldFixture f(options.profile, t);
                const auto counts = gdn_replay::extents(options.commits, t, options.batch, true);
                constexpr int slots[] = {10, 2, 8, 0, 6, 4, 9, 1};
                std::vector<ops::GdnReplayFoldRow> rows;
                for (int b = 0; b < options.batch; ++b)
                    rows.push_back({slots[b], slots[b], counts[b]});
                row.commits = gdn_replay::csv_extent(options.commits);
                run([&](CudaExecutionView cuda) { f.fold_plan().execute(rows, cuda); },
                    [&](CudaExecutionView cuda) { f.restore(cuda); });
            } else {
                auto valid = gdn_replay::extents(options.valid, t, options.batch, false);
                if (options.valid == "dense") valid.clear();
                gdn_replay::RecordFixture f(options.profile, t, options.batch, valid);
                row.valid = gdn_replay::csv_extent(options.valid);
                run([&](CudaExecutionView cuda) { f.launch_record(cuda); }, [](auto) {});
            }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
