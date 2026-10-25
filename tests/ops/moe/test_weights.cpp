#include "ninfer/ops/moe.h"

#include <limits>
#include <stdexcept>

namespace ninfer::test::moe {
namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template <class F>
void rejects(F&& call) {
    try {
        call();
    } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid expert activation usage was accepted");
}

} // namespace

int test_weights() {
    const WeightView weight;
    ops::ExpertInputUsage usage;
    usage.include({weight, ops::LinearPolicy::AllowA4, 2.0F});
    usage.include({weight, ops::LinearPolicy::AllowA4, 2.0F});
    require(usage.policy() == ops::LinearPolicy::AllowA4 &&
                usage.activation_divisor(QType::NVFP4) == 2.0F,
            "matching expert calibration was not preserved");
    require(!usage.activation_divisor(QType::Q4_G64_FP16),
            "non-NVFP4 input acquired an activation divisor");
    usage.include({weight, ops::LinearPolicy::AllowA8, {}});
    require(usage.policy() == ops::LinearPolicy::AllowA8 && !usage.activation_divisor(QType::NVFP4),
            "A8 restriction did not remove incomplete A4 calibration");
    usage.include({weight, ops::LinearPolicy::A16Only, 3.0F});
    usage.include({weight, ops::LinearPolicy::AllowA4, 2.0F});
    require(usage.policy() == ops::LinearPolicy::A16Only && !usage.activation_divisor(QType::NVFP4),
            "expert permissions were widened or unequal calibration was retained");

    for (const auto divisor : {std::optional<float>{}, std::optional<float>{3.0F}}) {
        ops::ExpertInputUsage invalid;
        invalid.include({weight, ops::LinearPolicy::AllowA4, 2.0F});
        invalid.include({weight, ops::LinearPolicy::AllowA4, divisor});
        rejects([&] { (void)invalid.activation_divisor(QType::NVFP4); });
    }
    rejects([&] { (void)ops::ExpertInputUsage{}.activation_divisor(QType::NVFP4); });
    for (const auto divisor : {0.0F, -1.0F, std::numeric_limits<float>::infinity(),
                               std::numeric_limits<float>::quiet_NaN()})
        rejects([&] { usage.include({weight, ops::LinearPolicy::AllowA4, divisor}); });
    return 0;
}

} // namespace ninfer::test::moe
