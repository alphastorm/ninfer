#include "ninfer/engine.h"
#include "serve/openai_responses_store.h"
#include "serve/serve_options.h"
#include "serve/session_checkpoint_service.h"
#include "checkpoint_write_fault.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {
using namespace ninfer;
using namespace ninfer::serve;

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}
} // namespace

// omp-ninfer#89 L6: the shutdown report acknowledges the final saves only once its bytes are
// durable. A failed close of its staged file, including a deferred write error, fails
// final_save() and publishes no report.
int main() {
    const char* artifact = std::getenv("NINFER_TEST_ARTIFACT");
    if (!artifact || !*artifact) { std::cout << "SKIP: NINFER_TEST_ARTIFACT is required\n"; return 77; }
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto root  = std::filesystem::temp_directory_path() /
                          ("ninfer-shutdown-report-test-" + std::to_string(stamp));
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } cleanup{root};

        EngineOptions engine_options;
        engine_options.artifact_path   = artifact;
        engine_options.max_context     = 1024;
        engine_options.kv_capacity     = KvCapacityPolicy::explicit_capacity(1024);
        engine_options.max_concurrency = 1;
        engine_options.kv_cache        = KvCacheStorage::BFloat16;
        engine_options.enable_vision   = false;
        engine_options.use_cuda_graph  = false;
        Engine engine(engine_options);

        // Durable serving requires authentication and lifecycle provenance; the service only
        // validates their form here.
        ServeOptions options;
        options.api_key                 = std::string(32, 'k');
        options.binary_sha256           = std::string(64, 'b');
        options.artifact_sha256         = std::string(64, 'a');
        options.deployment_profile      = "shutdown-report-proof";
        options.session_checkpoint_root = root / "checkpoints";
        const auto report               = root / "shutdown.json";
        options.shutdown_report         = report.string();
        OpenAIResponsesStore responses(8, 1ULL << 20);

        int failures = 0;
        {
            SessionCheckpointService service(engine, options, responses, std::nullopt);
            checkpoint_write_fault::CloseScope refused(ENOSPC, "shutdown.json.");
            const bool acknowledged = service.final_save();
            failures += check(checkpoint_write_fault::fired == 1 && !acknowledged &&
                                  !std::filesystem::exists(report),
                              "failed shutdown-report close acknowledged the final saves");
        }
        {
            SessionCheckpointService service(engine, options, responses, std::nullopt);
            const bool acknowledged = service.final_save();
            std::ifstream input(report, std::ios::binary);
            const auto value = nlohmann::json::parse(input, nullptr, false);
            failures += check(acknowledged && value.is_object() &&
                                  value.value("artifact_type", "") == "ninfer_serve_shutdown_report" &&
                                  value.value("complete", false) && value.value("refused", 1) == 0,
                              "durable shutdown report was not acknowledged");
        }
        if (failures == 0) { std::cout << "session checkpoint service real tests passed\n"; }
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
