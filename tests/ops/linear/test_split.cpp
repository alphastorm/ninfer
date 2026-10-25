#include "ninfer/ops/linear_split.h"
#include "cuda_fixture.h"
#include "core/buffer.h"
#include "core/decode_graph.h"
#include "core/device.h"
#include "ops/input_projection_test_common.h"
#include "ops/linear/linear_test_common.h"

#include <array>
#include <memory>
#include <numeric>
#include <set>

using namespace ninfer;
using namespace ninfer::test;
using namespace ninfer::test::input_projection;

namespace {

constexpr int kInputRows = 2560;
// One A16 arithmetic profile for all three projections; the same BF16 output allowance as Linear.
constexpr ReductionCriterion kA16Criterion{1.0 / 256.0, 1.0 / 256.0, 2.0 / 256.0};

struct Segment {
    const char* name;
    int rows, head_dim;
};

struct Profile {
    const char* name;
    bool q8;
    std::vector<Segment> segments;
    std::vector<int> tokens;
};

void launch(const Tensor& x, const Weight& weight, std::vector<Tensor>& out,
            CudaExecutionView cuda) {
    ops::linear_split(x, weight, out, cuda);
}

struct Outputs {
    std::vector<std::unique_ptr<GuardedDeviceBuffer>> backing;
    std::vector<Tensor> views;
    std::vector<void*> gaps;

    Outputs(const Profile& profile, int tokens, bool shared) {
        std::uint16_t* next = nullptr;
        if (shared) {
            std::size_t words = 0;
            for (const auto& s : profile.segments) words += std::size_t(s.rows) * tokens + 128;
            backing.push_back(std::make_unique<GuardedDeviceBuffer>(words * 2));
            next = static_cast<std::uint16_t*>(backing.back()->data());
        }
        for (const auto& s : profile.segments) {
            void* data = next;
            if (shared) {
                next += std::size_t(s.rows) * tokens;
                gaps.push_back(next);
                next += 128;
            } else {
                backing.push_back(
                    std::make_unique<GuardedDeviceBuffer>(std::size_t(s.rows) * tokens * 2));
                data = backing.back()->data();
            }
            views.emplace_back(data, backing.back()->device(), DType::BF16,
                               std::initializer_list<std::int32_t>{s.rows, tokens});
        }
    }

    void poison() {
        for (auto& b : backing) b->fill(0xff);
    }

    int check_guards(const std::string& label) const {
        int failures = 0;
        for (const auto& b : backing) failures += b->verify_guards(label);
        for (const void* gap : gaps)
            failures +=
                verify_exact((label + " shared gap").c_str(), from_device<std::uint16_t>(gap, 128),
                             std::vector<std::uint16_t>(128, 0xffff));
        return failures;
    }
};

std::vector<int> rows_to_check(const Segment& segment, bool full) {
    if (full) {
        std::vector<int> rows(segment.rows);
        std::iota(rows.begin(), rows.end(), 0);
        return rows;
    }
    const auto sampled = sampled_rows(segment.rows);
    std::set<int> rows(sampled.begin(), sampled.end());
    for (int head = segment.head_dim; head < segment.rows; head += segment.head_dim) {
        rows.insert(head - 1);
        rows.insert(head);
    }
    return {rows.begin(), rows.end()};
}

std::vector<int> columns_to_check(int tokens) {
    if (tokens <= 32) {
        std::vector<int> columns(tokens);
        std::iota(columns.begin(), columns.end(), 0);
        return columns;
    }
    std::set<int> columns{0, 1, 15, 16, 31, 32, tokens / 2, tokens - 2, tokens - 1};
    return {columns.begin(), columns.end()};
}

int verify_segment(const std::string& label, const Segment& segment, int row_begin,
                   const quantized_weight::PackedWeight& parent, const std::vector<float>& input,
                   const Tensor& output) {
    const int tokens = output.ne[1];
    const auto words = from_device<std::uint16_t>(output.data, std::size_t(segment.rows) * tokens);
    for (auto word : words) {
        if ((word & 0x7f80) == 0x7f80) {
            std::cerr << label << ": output is unwritten or non-finite\n";
            return 1;
        }
    }
    const auto rows    = rows_to_check(segment, tokens <= 8);
    const auto columns = columns_to_check(tokens);
    std::vector<double> actual, expected;
    actual.reserve(rows.size() * columns.size());
    expected.reserve(actual.capacity());
    for (int row : rows) {
        std::array<double, kInputRows> decoded;
        for (int k = 0; k < kInputRows; ++k)
            decoded[k] = quantized_weight::logical_weight_fp64(parent, row_begin + row, k);
        for (int column : columns) {
            double sum = 0;
            for (int k = 0; k < kInputRows; ++k)
                sum += decoded[k] * double(input[std::size_t(column) * kInputRows + k]);
            actual.push_back(bf16_to_f32(words[std::size_t(column) * segment.rows + row]));
            expected.push_back(sum);
        }
    }
    return verify_reduction(label, actual, expected, kA16Criterion);
}

int run_case(DeviceContext& device, const Profile& profile, DevicePackedWeight& parent, int tokens,
             bool graph, bool shared = false) {
    DeviceBuffer data(device.device(), std::size_t(kInputRows) * tokens * 2);
    Tensor x(data.span(), DType::BF16, {kInputRows, tokens});
    Outputs out(profile, tokens, shared);
    DecodeGraphDefinition definition;
    DecodeGraphExecutable executable;
    int failures = 0;
    for (int phase = 0; phase < (graph ? 2 : 1); ++phase) {
        const auto input = make_bf16_activation(kInputRows, tokens, 317U + tokens + phase * 31);
        const auto words = bf16_bits(input);
        data.copy_from_host(words.data(), words.size() * 2);
        out.poison();
        // Upload/poison operations must finish before the nonblocking capture stream reads them.
        cuda_synchronize();
        if (graph && phase == 0) {
            definition.capture(device.stream(), [&] {
                launch(x, parent.view(), out.views, device.execution_view());
            });
            executable.instantiate(definition);
        }
        if (graph)
            executable.launch(device.stream());
        else
            launch(x, parent.view(), out.views, device.execution_view());
        device.synchronize();
        const auto label = std::string(profile.name) + " M=" + std::to_string(tokens) +
                           " graph=" + std::to_string(graph) + " phase=" + std::to_string(phase) +
                           " shared=" + std::to_string(shared);
        int row_begin = 0;
        for (std::size_t i = 0; i < profile.segments.size(); ++i) {
            const auto& segment = profile.segments[i];
            failures += verify_segment(label + " " + segment.name, segment, row_begin, parent.host,
                                       input, out.views[i]);
            row_begin += segment.rows;
        }
        failures += out.check_guards(label);
        failures += verify_preserved(label + " input", data, words);
    }
    return failures;
}

template <class F>
int rejects(const char* label, F&& call) {
    try {
        call();
    } catch (const std::invalid_argument&) { return 0; }
    std::cerr << label << ": accepted invalid operands\n";
    return 1;
}

int validation(DeviceContext& device, const Profile& profile, const Weight& weight) {
    DeviceBuffer data(ninfer::test::cuda_environment().context.device(), kInputRows * 2 * 2);
    Tensor x(data.span(), DType::BF16, {kInputRows, 2});
    Outputs outputs(profile, 2, false);
    int failures = 0;
    auto out     = outputs.views;
    out[0].ne[0] -= 1;
    failures +=
        rejects("wrong output shape", [&] { launch(x, weight, out, device.execution_view()); });
    out         = outputs.views;
    out[1].data = static_cast<std::byte*>(out[0].data) + 16;
    failures += rejects("overlapping output segments",
                        [&] { launch(x, weight, out, device.execution_view()); });
    out         = outputs.views;
    out[0].data = x.data;
    failures +=
        rejects("output overlaps x", [&] { launch(x, weight, out, device.execution_view()); });
    out         = outputs.views;
    out[0].data = const_cast<void*>(weight.qdata);
    failures +=
        rejects("output overlaps weight", [&] { launch(x, weight, out, device.execution_view()); });
    out          = outputs.views;
    out[0].dtype = DType::FP16;
    failures +=
        rejects("wrong output dtype", [&] { launch(x, weight, out, device.execution_view()); });
    out          = outputs.views;
    auto invalid = weight;
    invalid.padded_shape[1] += 128;
    failures += rejects("unsupported padded weight",
                        [&] { launch(x, invalid, out, device.execution_view()); });
    invalid       = weight;
    invalid.qtype = profile.q8 ? QType::BF16 : QType::Q8_G32_FP16;
    failures +=
        rejects("wrong weight type", [&] { launch(x, invalid, out, device.execution_view()); });
    if (profile.q8) {
        out[0].data = const_cast<void*>(weight.scales);
        failures += rejects("output overlaps scales",
                            [&] { launch(x, weight, out, device.execution_view()); });
        out           = outputs.views;
        invalid       = weight;
        invalid.group = 64;
        failures +=
            rejects("wrong Q8 group", [&] { launch(x, invalid, out, device.execution_view()); });
    }
    auto strided = x;
    strided.nb[1] += 16;
    failures +=
        rejects("strided input", [&] { launch(strided, weight, out, device.execution_view()); });
    auto empty  = x;
    empty.ne[1] = 0;
    failures += rejects("zero M", [&] { launch(empty, weight, out, device.execution_view()); });
    return failures;
}

} // namespace

int main() {
    if (cuda_unavailable()) return 77;
    try {
        DeviceContext device;
        const std::vector<Profile> profiles{
            {"qg",
             true,
             {{"query", 6144, 256}, {"gate", 6144, 256}},
             {1,   2,   4,   8,   15,  16,  17,  31,  32,   33,   34,   35,  39,  40,
              41,  63,  64,  65,  79,  80,  81,  95,  96,   97,   103,  104, 105, 109,
              110, 111, 112, 113, 127, 128, 129, 512, 1024, 2048, 4096, 4097}},
            {"kv-index",
             false,
             {{"key", 512, 256},
              {"value", 512, 256},
              {"index_query", 512, 128},
              {"index_key", 128, 128}},
             {1,  2,  4,  8,   15,  16,  17,  47,  48,  49,   63,   64,   65,
              95, 96, 97, 127, 128, 129, 511, 512, 513, 1024, 2048, 4096, 4097}},
            {"qkgv-index",
             false,
             {{"query", 6144, 256},
              {"key", 512, 256},
              {"gate", 6144, 256},
              {"value", 512, 256},
              {"index_query", 512, 128},
              {"index_key", 128, 128}},
             {1,  2,  4,  7,  8,  9,   15,  16,  17,  31,  32,  33,   47,   48,   49,  63,
              64, 65, 95, 96, 97, 127, 128, 129, 511, 512, 513, 1024, 2048, 4096, 4097}}};
        int failures = 0;
        for (const auto& profile : profiles) {
            int rows = 0;
            for (const auto& s : profile.segments) rows += s.rows;
            DevicePackedWeight parent(profile.q8
                                          ? linear::make_q8_g32_fp16_weight(rows, kInputRows, 317U)
                                          : linear::make_bf16_weight(rows, kInputRows, 331U));
            failures += validation(device, profile, parent.view());
            for (int t : profile.tokens)
                failures +=
                    run_case(device, profile, parent, t,
                             t == 1 || t == 8 || t == 32 || t == 64 || t == 128 || t == 129);
            failures += run_case(device, profile, parent, 17, true, true);
            failures += parent.verify_preserved(profile.name);
        }
        if (failures) return 1;
        std::cout << "OK LinearSplit A16 projections, segments, guards and Graph replay\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
