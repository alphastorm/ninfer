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
        for (auto test_case : control::projection_cases(profile))
            failed += control::run(profile, test_case, false, device.execution_view());
        failed += control::validate(profile, false, device.execution_view());
        failed += control::validate_capacity(profile, false);
    }
    std::cout << (failed ? "FAIL" : "OK") << " GDN projection\n";
    return failed ? 1 : 0;
}
