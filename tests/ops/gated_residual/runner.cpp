#include "runner.h"
#include "oracle.h"
#include "criteria.h"
#include "core/decode_graph.h"

namespace ninfer::test::hc {
int verify(const Fixture& f, const std::string& label, bool exact_residual) {
    const auto ref = reference(f);
    int failed     = 0;
    if (f.form == Form::ExpandNorm)
        failed +=
            verify_exact((label + " expansion").c_str(),
                         from_device<std::uint16_t>(f.tr.data, f.h.size()), ref.residual_words);
    else if (f.form != Form::GroupedNorm)
        failed += verify_pointwise(label + " residual", from_device_bf16(f.tr.data, f.h.size()),
                                   ref.residual, kResidual);
    if (exact_residual)
        failed +=
            verify_exact((label + " midpoint residual").c_str(),
                         from_device<std::uint16_t>(f.tr.data, f.h.size()), ref.residual_words);
    if (f.form != Form::Combine) {
        const auto actual         = from_device_bf16(f.tz.data, f.h.size());
        const bool combined       = f.form == Form::CombineNorm || f.form == Form::AddNorm;
        const auto seam_reference = combined ? grouped_norm(from_device_bf16(f.tr.data, f.h.size()),
                                                            f.weight, f.columns, f.epsilon)
                                             : std::vector<double>{};
        for (int m = 0; m < f.columns; ++m)
            for (int s = 0; s < kStreams; ++s) {
                const auto base = (std::size_t(m) * kStreams + s) * kWidth;
                const auto part = [&](const std::vector<double>& values) {
                    return std::span<const double>(values.data() + base, kWidth);
                };
                const auto tag =
                    label + " column=" + std::to_string(m) + " stream=" + std::to_string(s);
                failed += verify_reduction(tag, part(actual), part(ref.normalized), kNormalized);
                if (combined)
                    failed += verify_reduction(tag + " actual seam", part(actual),
                                               part(seam_reference), kNormalized);
            }
    }
    return failed;
}

namespace {
int run(Form form, int m, Pattern pattern, bool alias, bool captured, CudaExecutionView e) {
    Fixture f(form, m, alias);
    f.set(pattern);
    f.upload(e.stream());
    DecodeGraphDefinition definition;
    DecodeGraphExecutable graph;
    if (captured) {
        definition.capture(e.stream(), [&] { f.invoke(e); });
        graph.instantiate(definition);
    }
    int failed = 0;
    for (int phase = 0; phase < (captured ? 2 : 1); ++phase) {
        if (phase) {
            f.set(pattern, phase);
            f.upload(e.stream());
        }
        if (captured)
            graph.launch(e.stream());
        else
            f.invoke(e);
        cuda_synchronize(e.stream());
        const auto label = std::string(name(form)) + " M=" + std::to_string(m) +
                           " pattern=" + std::to_string(int(pattern)) +
                           " alias=" + std::to_string(alias) +
                           " graph=" + std::to_string(captured) + " phase=" + std::to_string(phase);
        const bool exact_residual =
            pattern == Pattern::Midpoint && (form == Form::Combine || form == Form::CombineNorm);
        failed += verify(f, label, exact_residual) + f.check(label);
    }
    return failed;
}
} // namespace

int run_suite(Form form) {
    if (cuda_unavailable()) {
        std::cout << "SKIP: no CUDA device\n";
        return 77;
    }
    DeviceContext device;
    const auto e = device.execution_view();
    int failed   = 0;
    for (int m : {1, 3, 8, 16, 31, 32, 33, 127, 128, 129, 512, 2048})
        failed += run(form, m, Pattern::Random, false, false, e);
    for (auto pattern : {Pattern::Affine, Pattern::Zero, Pattern::Constant, Pattern::Cancellation,
                         Pattern::Midpoint, Pattern::Small})
        failed += run(form, 3, pattern, false, false, e);
    failed += run(form, 7 * 8, Pattern::Random, false, true, e);
    if (form == Form::ExpandNorm)
        for (int m : {2047, 2048, 2049})
            failed +=
                run(form, m, m == 2049 ? Pattern::Constant : Pattern::Random, false, m == 2048, e);
    if (form == Form::GroupedNorm)
        for (int m : {2815, 2816, 2817})
            failed +=
                run(form, m, m == 2817 ? Pattern::Constant : Pattern::Random, false, m == 2816, e);
    const bool combine =
        form == Form::Combine || form == Form::CombineNorm || form == Form::AddNorm;
    if (combine) {
        failed += run(form, 1, Pattern::Affine, true, false, e);
        failed += run(form, 129, Pattern::Random, true, false, e);
        failed += run(form, 7 * 8, Pattern::Midpoint, true, true, e);
    }
    if (form == Form::CombineNorm || form == Form::AddNorm) {
        for (int m : {1535, 1536, 1537, 1792, 1839, 1840, 1841, 3072, 3073, 3074})
            failed += run(form, m, Pattern::Random, false, false, e);
        failed += run(form, 1664, Pattern::Midpoint, false, true, e);
        for (int m : {1823, 1824, 1825, 3680, 3681, 3682})
            failed += run(form, m, Pattern::Random, true, false, e);
        failed += run(form, 2048, Pattern::Random, false, true, e);
        failed += run(form, 4096, Pattern::Random, true, false, e);
    }
    failed += validate(form, e);
    std::cout << (failed ? "FAIL " : "OK ") << name(form) << '\n';
    return failed ? 1 : 0;
}
} // namespace ninfer::test::hc
