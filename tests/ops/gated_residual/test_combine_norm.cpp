#include "runner.h"

int main() {
    const int first = ninfer::test::hc::run_suite(ninfer::test::hc::Form::CombineNorm);
    if (first == 77) return 77;
    return first | ninfer::test::hc::run_suite(ninfer::test::hc::Form::AddNorm);
}
