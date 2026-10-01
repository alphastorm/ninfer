#include <iostream>
#include <string_view>

int run_softmax_attention_causal_cache_tests(bool nvfp4_only);
int run_softmax_attention_plain_and_packed_tests();
int run_softmax_attention_context_tests();

int main(int argc, char** argv) {
    bool causal_only = false;
    bool nvfp4_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--causal-only") {
            causal_only = true;
        } else if (argument == "--kv-dtype" && i + 1 < argc &&
                   (std::string_view(argv[i + 1]) == "nvfp4" ||
                    std::string_view(argv[i + 1]) == "all")) {
            nvfp4_only = std::string_view(argv[++i]) == "nvfp4";
            causal_only = true;
        } else {
            std::cerr << "usage: ninfer_softmax_attention_test [--causal-only] "
                         "[--kv-dtype nvfp4|all]\n";
            return 2;
        }
    }
    const int causal = run_softmax_attention_causal_cache_tests(nvfp4_only);
    if (causal == 77) return 77;
    if (causal_only) return causal;

    const int plain_and_packed = run_softmax_attention_plain_and_packed_tests();
    if (plain_and_packed == 77) return 77;

    const int context = run_softmax_attention_context_tests();
    if (context == 77) return 77;

    const int failures = causal + plain_and_packed + context;
    std::cout << (failures == 0 ? "softmax_attention: PASS\n" : "softmax_attention: FAIL\n");
    return failures == 0 ? 0 : 1;
}
