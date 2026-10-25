#include "cases.h"
#include "runner.h"
#include "validation.h"
#include <iostream>
using namespace ninfer;
using namespace ninfer::test;
namespace control = ninfer::test::gdn_control;

int main() {
    if (cuda_unavailable()) {
        std::cout << "SKIP: no CUDA device\n";
        return 77;
    }
    DeviceContext device;
    int failed = 0;
    for (auto profile : control::kProfiles) {
        if (profile.input_rows == 2560) continue;
        for (auto test_case : control::norm_cases(profile))
            failed += control::run(profile, test_case, true, device.execution_view());
        failed += control::validate(profile, true, device.execution_view());
        failed += control::validate_capacity(profile, true);
    }
    std::cout << (failed ? "FAIL" : "OK") << " GDN norm_projection\n";
    return failed ? 1 : 0;
}
