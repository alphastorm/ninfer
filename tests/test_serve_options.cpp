#include "serve/serve_options.h"
#include "serve/translate.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ninfer::serve;

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << message << '\n';
    return 1;
}

ServeOptions parse(std::vector<std::string> arguments) {
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) { argv.push_back(argument.data()); }
    return parse_serve_options(static_cast<int>(argv.size()), argv.data());
}

} // namespace

int main() {
    int failures = 0;

    const std::string binary_sha(64, '1');
    const std::string artifact_sha(64, '2');
    const std::string config_sha(64, '3');
    const ServeOptions identity = parse(
        {"ninfer-serve", "model.ninfer", "--binary-sha256", binary_sha, "--artifact-sha256",
         artifact_sha, "--config-sha256", config_sha, "--deployment-profile", "release-profile"});
    failures += check(identity.binary_sha256 == binary_sha &&
                          identity.artifact_sha256 == artifact_sha &&
                          identity.config_sha256 == config_sha &&
                          identity.deployment_profile == "release-profile",
                      "lifecycle identity options were not preserved");
    for (const auto* flag : {"--binary-sha256", "--artifact-sha256", "--config-sha256"}) {
        for (const auto& value : {std::string{}, std::string(63, 'a'), std::string(65, 'a'),
                                  std::string(64, 'A'), std::string(64, 'g')}) {
            bool rejected = false;
            try {
                (void)parse({"ninfer-serve", "model.ninfer", flag, value});
            } catch (const std::invalid_argument&) { rejected = true; }
            failures += check(rejected, "invalid lifecycle SHA-256 was accepted");
        }
    }
    for (const auto& value : {std::string{}, std::string(65, 'a'), std::string("private/location"),
                              std::string("has space")}) {
        bool rejected = false;
        try {
            (void)parse({"ninfer-serve", "model.ninfer", "--deployment-profile", value});
        } catch (const std::invalid_argument&) { rejected = true; }
        failures += check(rejected, "invalid deployment profile was accepted");
    }
    failures += check(parse({"ninfer-serve", "model.ninfer", "--deployment-profile",
                              std::string(64, 'A')}).deployment_profile.size() == 64,
                      "maximum-length deployment profile was rejected");

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto key_path = std::filesystem::temp_directory_path() /
                          ("ninfer-api-key-" + std::to_string(stamp));
    std::ofstream(key_path, std::ios::binary) << "file-secret\r\n";
    const auto file_key = parse({"ninfer-serve", "model.ninfer", "--api-key-file", key_path.string()});
    failures += check(file_key.api_key == "file-secret", "API key file CRLF was not removed");
    failures += check(std::find(file_key.startup_argv.begin(), file_key.startup_argv.end(),
                               key_path.string()) == file_key.startup_argv.end(),
                      "startup argv retained the API key file path");
    for (const auto& arguments : {
             std::vector<std::string>{"ninfer-serve", "model.ninfer", "--api-key", "inline",
                                       "--api-key-file", key_path.string()},
             std::vector<std::string>{"ninfer-serve", "model.ninfer", "--api-key-file",
                                       key_path.string(), "--api-key", "inline"},
             std::vector<std::string>{"ninfer-serve", "model.ninfer", "--api-key", "first",
                                       "--api-key", "second"}}) {
        bool rejected = false;
        try { (void)parse(arguments); }
        catch (const std::invalid_argument&) { rejected = true; }
        failures += check(rejected, "multiple API key sources were accepted");
    }
    for (const auto& contents : {std::string{}, std::string("first\nsecond\n"),
                                 std::string(4097, 'x')}) {
        std::ofstream(key_path, std::ios::binary | std::ios::trunc) << contents;
        bool rejected = false;
        try { (void)parse({"ninfer-serve", "model.ninfer", "--api-key-file", key_path.string()}); }
        catch (const std::invalid_argument&) { rejected = true; }
        failures += check(rejected, "invalid API key file was accepted");
    }
    std::filesystem::remove(key_path);
    bool missing_key_rejected = false;
    try { (void)parse({"ninfer-serve", "model.ninfer", "--api-key-file", key_path.string()}); }
    catch (const std::invalid_argument&) { missing_key_rejected = true; }
    failures += check(missing_key_rejected, "missing API key file was accepted");
    for (const auto* flag : {"--api-key-file", "--binary-sha256", "--artifact-sha256",
                             "--config-sha256", "--deployment-profile"}) {
        failures += check(serve_usage_text("ninfer-serve").find(flag) != std::string::npos,
                          "serve help omitted a lifecycle identity option");
    }

    const ServeOptions defaults = parse({"ninfer-serve", "model.ninfer"});
    failures += check(defaults.gpu_keep_warm_ms == 0, "GPU keep-warm must default to disabled");
    for (const auto value : {0U, 60000U, std::numeric_limits<std::uint32_t>::max()}) {
        failures += check(parse({"ninfer-serve", "model.ninfer", "--gpu-keep-warm-ms",
                                 std::to_string(value)}).gpu_keep_warm_ms == value,
                          "GPU keep-warm duration was not preserved");
    }
    for (const char* value : {"-1", "4294967296", "18446744073709551616", "", "warm"}) {
        bool rejected = false;
        try {
            (void)parse({"ninfer-serve", "model.ninfer", "--gpu-keep-warm-ms", value});
        } catch (const std::invalid_argument&) { rejected = true; }
        failures += check(rejected, "invalid GPU keep-warm duration was accepted");
    }
    failures += check(serve_usage_text("ninfer-serve").find("--gpu-keep-warm-ms") != std::string::npos,
                      "serve help omits GPU keep-warm");
    failures += check(defaults.session_checkpoint_root.empty() &&
                          defaults.session_checkpoint_quota_bytes == (64ULL << 30) &&
                          defaults.session_checkpoint_staging_bytes == (256ULL << 20) &&
                          defaults.session_checkpoint_write_buffer_bytes == (6ULL << 30) &&
                          defaults.session_checkpoint_min_tokens == 32768 &&
                          !defaults.session_checkpoint_require_origin_auth &&
                          defaults.stop_event.empty() && defaults.shutdown_report.empty(),
                      "durable and managed-stop defaults changed");
    const std::vector<std::string> durable_args = {
        "ninfer-serve", "model.ninfer", "--api-key", std::string(32, 's'),
        "--binary-sha256", binary_sha, "--artifact-sha256", artifact_sha,
        "--config-sha256", config_sha, "--session-checkpoint-dir", "checkpoints"};
    const auto with_durable = [&](std::vector<std::string> extra) {
        auto arguments = durable_args;
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        return arguments;
    };
    const auto rejects = [](std::vector<std::string> arguments) {
        try { (void)parse(std::move(arguments)); }
        catch (const std::invalid_argument&) { return true; }
        return false;
    };
    const auto durable = parse(with_durable({
        "--session-checkpoint-quota-mib", "2048", "--session-checkpoint-staging-mib", "64",
        "--session-checkpoint-write-buffer-mib", "128", "--session-checkpoint-min-tokens", "4096",
        "--session-checkpoint-require-origin-auth", "--shutdown-report", "shutdown.json",
        "--spec", "dflash2", "--draft-tokens", "7"}));
    failures += check(durable.session_checkpoint_root == "checkpoints" &&
                          durable.session_checkpoint_quota_bytes == (2048ULL << 20) &&
                          durable.session_checkpoint_staging_bytes == (64ULL << 20) &&
                          durable.session_checkpoint_write_buffer_bytes == (128ULL << 20) &&
                          durable.session_checkpoint_min_tokens == 4096 &&
                          durable.session_checkpoint_require_origin_auth &&
                          durable.shutdown_report == "shutdown.json" &&
                          durable.speculative.backend == ninfer::SpeculativeBackend::DFlash2,
                      "durable configuration was not preserved");
    for (const auto* flag : {"--session-checkpoint-quota-mib", "--session-checkpoint-staging-mib",
                             "--session-checkpoint-write-buffer-mib"}) {
        for (const auto* value : {"0", "-1", "", "18446744073709551615"}) {
            failures += check(rejects(with_durable({flag, value})),
                              "invalid durable byte budget was accepted");
        }
    }
    failures += check(parse(with_durable({"--session-checkpoint-min-tokens", "0"}))
                              .session_checkpoint_min_tokens == 0 &&
                          parse(with_durable({"--session-checkpoint-min-tokens", "4294967295"}))
                              .session_checkpoint_min_tokens == UINT32_MAX,
                      "durable token threshold boundaries changed");
    failures += check(rejects(with_durable({"--session-checkpoint-min-tokens", "4294967296"})),
                      "overflowing durable token threshold was accepted");
    failures += check(rejects(with_durable({"--session-checkpoint-dir", ""})),
                      "empty durable root was accepted");
    failures += check(rejects(with_durable({"--no-prefix-reuse"})) &&
                          rejects(with_durable({"--host-context-mib", "0"})),
                      "durability was accepted without enabled Host context capacity");
    failures += check(rejects({"ninfer-serve", "model.ninfer", "--session-checkpoint-dir",
                               "checkpoints", "--binary-sha256", binary_sha,
                               "--artifact-sha256", artifact_sha, "--config-sha256", config_sha}) &&
                          rejects({"ninfer-serve", "model.ninfer", "--session-checkpoint-dir",
                                   "checkpoints", "--api-key", "secret"}),
                      "durability was accepted without authentication or lifecycle digests");
    failures += check(rejects({"ninfer-serve", "model.ninfer",
                               "--session-checkpoint-require-origin-auth"}),
                      "strict origin authentication was accepted without a durable root");
    auto short_key_args = durable_args;
    short_key_args[3] = std::string(31, 's');
    short_key_args.emplace_back("--session-checkpoint-require-origin-auth");
    failures += check(rejects(std::move(short_key_args)),
                      "strict origin authentication accepted a short credential");
    for (const auto* flag : {"--session-checkpoint-dir", "--session-checkpoint-quota-mib",
                             "--session-checkpoint-staging-mib",
                             "--session-checkpoint-write-buffer-mib",
                             "--session-checkpoint-min-tokens",
                             "--session-checkpoint-require-origin-auth", "--stop-event",
                             "--shutdown-report"}) {
        failures += check(serve_usage_text("ninfer-serve").find(flag) != std::string::npos,
                          "serve help omitted a durable or managed-stop option");
    }
    const std::string event_name = "Global\\NInfer-Serve-Stop-" + std::string(32, 'a');
#if defined(_WIN32)
    failures += check(parse({"ninfer-serve", "model.ninfer", "--stop-event", event_name}).stop_event ==
                          event_name,
                      "Windows stop-event name was not preserved");
#else
    failures += check(rejects({"ninfer-serve", "model.ninfer", "--stop-event", event_name}),
                      "Windows stop event was accepted off Windows");
#endif
    for (const auto& value : {std::string{}, std::string("Global\\unrelated"),
                              std::string("Local\\NInfer-Serve-Stop-") + std::string(32, 'A'),
                              std::string("NInfer-Serve-Stop-") + std::string(32, 'a')}) {
        failures += check(rejects({"ninfer-serve", "model.ninfer", "--stop-event", value}),
                          "invalid manager stop-event name was accepted");
    }
    failures += check(rejects({"ninfer-serve", "model.ninfer", "--shutdown-report", ""}),
                      "empty shutdown-report path was accepted");
    failures += check(defaults.allow_prefix_reuse, "prefix reuse is not enabled by default");
    failures +=
        check(!defaults.preserve_thinking, "thinking history is unexpectedly preserved by default");
    failures += check(!defaults.enable_vision, "Vision is not disabled by default");
    failures += check(!defaults.fp32_head_logits, "FP32 head logits must default off");
    failures += check(parse({"ninfer-serve", "model.ninfer", "--fp32-head-logits"}).fp32_head_logits,
                      "FP32 head logits opt-in was lost");
    failures += check(serve_usage_text("ninfer-serve").find("--fp32-head-logits") != std::string::npos,
                      "serve help omits FP32 head logits");
    failures += check(defaults.request_log_jsonl.empty(),
                      "request JSONL logging is not disabled by default");
    failures += check(defaults.context_cost_presets.empty(),
                      "external context-cost presets are unexpectedly configured by default");
    failures += check(defaults.log_stats_interval_ms == 5000,
                      "periodic throughput interval default mismatch");
    failures += check(defaults.media_cache_bytes == ninfer::kDefaultMediaCacheBytes &&
                          defaults.media_live_bytes == ninfer::kDefaultMediaLiveBytes &&
                          defaults.media_preprocess_threads == 0,
                      "media preparation resource defaults mismatch");
    failures += check(defaults.kv_capacity.mode == ninfer::KvCapacityMode::Explicit &&
                          defaults.kv_capacity.explicit_tokens == defaults.max_context,
                      "default KV capacity does not follow max context");
    failures += check(!defaults.admission_headroom_bytes,
                      "default admission headroom was unexpectedly overridden");
    for (const std::size_t mib : {256U, 640U, 768U, 1024U}) {
        const auto configured_headroom = parse({"ninfer-serve", "model.ninfer",
                                                "--admission-headroom-mib", std::to_string(mib)});
        failures += check(configured_headroom.admission_headroom_bytes == mib * (1ULL << 20),
                          "admission headroom was not preserved in bytes");
        failures += check(configured_headroom.kv_capacity.automatic_headroom_bytes == 0,
                          "admission headroom changed explicit KV policy");
    }
    for (const auto* value : {"0", "255", "-1", "256.5", "invalid", "18446744073709551616"}) {
        bool rejected = false;
        try {
            (void)parse({"ninfer-serve", "model.ninfer", "--admission-headroom-mib", value});
        } catch (const std::invalid_argument&) { rejected = true; }
        failures += check(rejected, "invalid admission headroom was accepted");
    }
    for (const auto& arguments : std::vector<std::vector<std::string>>{
             {"ninfer-serve", "model.ninfer", "--admission-headroom-mib"},
             {"ninfer-serve", "model.ninfer", "--admission-headroom-mib",
              std::to_string(std::numeric_limits<std::size_t>::max() / (1ULL << 20) + 1)}}) {
        bool rejected = false;
        try {
            (void)parse(arguments);
        } catch (const std::invalid_argument&) { rejected = true; }
        failures += check(rejected, "missing or overflowing admission headroom was accepted");
    }
    const auto automatic_headroom = parse({"ninfer-serve", "model.ninfer", "--kv-capacity", "auto",
                                           "--admission-headroom-mib", "640"});
    failures += check(automatic_headroom.admission_headroom_bytes == 640ULL * 1024 * 1024 &&
                          automatic_headroom.kv_capacity.automatic_headroom_bytes ==
                              ninfer::kDefaultKvCapacityHeadroomBytes,
                      "admission headroom weakened the independent automatic KV lower bound");
    failures += check(serve_usage_text("ninfer-serve").find("--admission-headroom-mib") !=
                          std::string::npos,
                      "serve help omits the admission headroom floor");
    failures += check(!defaults.context_cache.host_capacity_bytes.has_value(),
                      "default Host context capacity was resolved before Engine startup");
    failures += check(
        parse({"ninfer-serve", "model.ninfer", "--prefill-chunk", "8192"}).prefill_chunk == 8192,
        "explicit prefill chunk was not preserved");
    for (const auto* value : {"auto", "0", "129"}) {
        bool rejected = false;
        try {
            (void)parse({"ninfer-serve", "model.ninfer", "--prefill-chunk", value});
        } catch (const std::invalid_argument&) { rejected = true; }
        failures += check(rejected, "invalid prefill chunk was accepted");
    }
    failures += check(defaults.speculative.backend == ninfer::SpeculativeBackend::None,
                      "speculative decoding is not disabled by default");
    failures += check(defaults.response_store_max_records == kDefaultResponseStoreRecords &&
                          defaults.response_store_max_bytes == kDefaultResponseStoreBytes,
                      "Responses store defaults mismatch");
    failures += check(!defaults.model_id_override.has_value(),
                      "model id override is unexpectedly configured by default");
    failures += check(!defaults.default_thinking_budget,
                      "thinking budget is unexpectedly limited by default");
    failures += check(
        !defaults.sampling_overrides.temperature && !defaults.sampling_overrides.top_p &&
            !defaults.sampling_overrides.top_k && !defaults.sampling_overrides.presence_penalty &&
            !defaults.sampling_overrides.frequency_penalty,
        "server defaults unexpectedly override registered model sampling");
    failures += check(resolve_public_model_id(defaults, "artifact-model") == "artifact-model",
                      "artifact model id was not selected by default");

    const ServeOptions fp8 = parse({"ninfer-serve", "model.ninfer", "--kv-dtype", "fp8"});
    failures += check(fp8.kv_cache == ninfer::KvCacheStorage::Fp8E4M3Row256,
                      "--kv-dtype fp8 did not select row-scaled E4M3 KV");
    const ServeOptions nvfp4 = parse({"ninfer-serve", "model.ninfer", "--kv-dtype", "nvfp4"});
    failures += check(nvfp4.kv_cache == ninfer::KvCacheStorage::Nvfp4Group16,
                      "--kv-dtype nvfp4 did not select group-16 NVFP4 KV");
    const ServeOptions k8v4 = parse({"ninfer-serve", "model.ninfer", "--kv-dtype", "k8v4"});
    failures += check(k8v4.kv_cache == ninfer::KvCacheStorage::Fp8KeyNvfp4Value,
                      "--kv-dtype k8v4 did not select asymmetric K8V4 KV");
    const std::string kv_help = serve_usage_text("ninfer-serve");
    failures += check(kv_help.find("nvfp4") != std::string::npos &&
                          kv_help.find("k8v4") != std::string::npos,
                      "serve help omits a production KV storage mode");

    const ServeOptions model_alias =
        parse({"ninfer-serve", "model.ninfer", "--model-id", "deployment-alias"});
    failures +=
        check(model_alias.model_id_override == "deployment-alias" &&
                  resolve_public_model_id(model_alias, "artifact-model") == "deployment-alias",
              "explicit model id did not override the artifact identity");

    const ServeOptions context_cost =
        parse({"ninfer-serve", "model.ninfer", "--context-cost-presets", "local-costs.json"});
    failures += check(context_cost.context_cost_presets == "local-costs.json",
                      "--context-cost-presets did not preserve its path");

    const ServeOptions thinking_budget =
        parse({"ninfer-serve", "model.ninfer", "--default-thinking-budget", "37"});
    failures += check(thinking_budget.default_thinking_budget == 37,
                      "--default-thinking-budget did not preserve its positive value");
    bool zero_thinking_budget_rejected = false;
    try {
        (void)parse({"ninfer-serve", "model.ninfer", "--default-thinking-budget", "0"});
    } catch (const std::invalid_argument&) { zero_thinking_budget_rejected = true; }
    failures += check(zero_thinking_budget_rejected, "zero --default-thinking-budget was accepted");

    bool empty_model_id_rejected = false;
    try {
        (void)parse({"ninfer-serve", "model.ninfer", "--model-id", ""});
    } catch (const std::invalid_argument&) { empty_model_id_rejected = true; }
    failures += check(empty_model_id_rejected, "empty --model-id was accepted");

    const ServeOptions dflash = parse({"ninfer-serve", "model.ninfer", "--spec", "dflash",
                                       "--draft-tokens", "15", "--lm-head-draft"});
    failures += check(dflash.speculative.backend == ninfer::SpeculativeBackend::DFlash,
                      "--spec dflash did not select DFlash");
    failures += check(dflash.speculative.draft_tokens == 15,
                      "--draft-tokens did not preserve the DFlash window");
    failures += check(dflash.speculative.proposal_head == ninfer::ProposalHead::Optimized,
                      "--lm-head-draft did not select the optimized proposal head");

    for (const auto k : {1U, 2U, 7U, 15U}) {
        const auto options = parse({"ninfer-serve", "model.ninfer", "--spec", "dflash2",
                                    "--draft-tokens", std::to_string(k), "--lm-head-draft"});
        failures += check(options.speculative.backend == ninfer::SpeculativeBackend::DFlash2 &&
                              options.speculative.draft_tokens == k &&
                              options.speculative.proposal_head == ninfer::ProposalHead::Optimized,
                          "serve options did not preserve DFlash2 configuration");
    }

    const ServeOptions dflash_vision = parse(
        {"ninfer-serve", "model.ninfer", "--spec", "dflash", "--draft-tokens", "15", "--vision"});
    failures += check(dflash_vision.enable_vision &&
                          dflash_vision.speculative.backend == ninfer::SpeculativeBackend::DFlash &&
                          dflash_vision.speculative.draft_tokens == 15,
                      "serve options did not preserve combined DFlash and Vision features");

    bool implicit_backend_rejected = false;
    try {
        (void)parse({"ninfer-serve", "model.ninfer", "--draft-tokens", "3"});
    } catch (const std::invalid_argument&) { implicit_backend_rejected = true; }
    failures += check(implicit_backend_rejected, "--draft-tokens selected a backend implicitly");

    const ServeOptions configured = parse({"ninfer-serve",
                                           "model.ninfer",
                                           "--no-prefix-reuse",
                                           "--vision",
                                           "--max-concurrency",
                                           "4",
                                           "--max-pending-requests",
                                           "12",
                                           "--pending-timeout-ms",
                                           "2500",
                                           "--max-context",
                                           "4096",
                                           "--kv-capacity",
                                           "8192",
                                           "--log-stats-interval-ms",
                                           "0",
                                           "--preserve-thinking",
                                           "--media-cache-mib",
                                           "256",
                                           "--media-live-mib",
                                           "512",
                                           "--media-preprocess-threads",
                                           "6"});
    failures += check(!configured.allow_prefix_reuse,
                      "--no-prefix-reuse did not disable server prefix reuse");
    failures += check(!configured.context_cache.enabled &&
                          configured.context_cache.host_capacity_bytes ==
                              defaults.context_cache.host_capacity_bytes &&
                          configured.context_cache.device_state_slots ==
                              defaults.context_cache.device_state_slots,
                      "--no-prefix-reuse changed context capacities or retained cache enablement");
    failures += check(configured.enable_vision, "--vision did not enable Vision");
    failures += check(configured.preserve_thinking == true,
                      "--preserve-thinking did not reach serving options");
    failures +=
        check(configured.max_concurrency == 4, "--max-concurrency did not reach serving options");
    failures += check(configured.max_context == 4096 &&
                          configured.kv_capacity.mode == ninfer::KvCapacityMode::Explicit &&
                          configured.kv_capacity.explicit_tokens == 8192,
                      "context and KV capacity options were not kept distinct");
    failures += check(configured.max_pending_requests == 12,
                      "--max-pending-requests did not reach serving options");
    failures += check(configured.pending_timeout_ms == 2500,
                      "--pending-timeout-ms did not reach serving options");
    failures += check(configured.log_stats_interval_ms == 0,
                      "--log-stats-interval-ms did not disable periodic reporting");
    failures += check(configured.media_cache_bytes == (256ULL << 20) &&
                          configured.media_live_bytes == (512ULL << 20) &&
                          configured.media_preprocess_threads == 6,
                      "media preparation limits did not reach serving options");

    const ServeOptions logging = parse({"ninfer-serve", "model.ninfer", "--log-level", "debug"});
    failures += check(logging.log_level == ninfer::product::LogLevel::Debug,
                      "log level did not reach serving options");

    const ServeOptions context_cache = parse(
        {"ninfer-serve", "model.ninfer", "--device-state-slots", "3", "--host-context-mib", "64"});
    failures += check(context_cache.context_cache.enabled &&
                          context_cache.context_cache.device_state_slots == 3 &&
                          context_cache.context_cache.host_capacity_bytes == (64ULL << 20),
                      "context-cache capacities did not reach serving options");

    const ServeOptions zero_host_context =
        parse({"ninfer-serve", "model.ninfer", "--host-context-mib", "0"});
    failures += check(zero_host_context.context_cache.enabled &&
                          zero_host_context.context_cache.host_capacity_bytes == 0,
                      "zero Host context capacity was not retained as an explicit budget");

    for (const auto& [mib, bytes] : std::vector<std::pair<std::string, std::size_t>>{
             {"146.822265625", 153954304},
             {"587.2890625", 615817216},
             {"186.822265625", 195897344},
             {"0.00000095367431640625", 1},
             {"0.000000953674316406250000", 1},
             {"000146.822265625000", 153954304},
             {"64.0000", 64ULL << 20},
             {"000.0000", 0},
         }) {
        const ServeOptions exact_host_context =
            parse({"ninfer-serve", "model.ninfer", "--host-context-mib", mib});
        failures += check(exact_host_context.context_cache.host_capacity_bytes == bytes,
                          ("Host context MiB did not preserve exact bytes: " + mib).c_str());
    }

    constexpr std::size_t bytes_per_mib  = 1ULL << 20;
    constexpr std::size_t max_host_bytes = std::numeric_limits<std::size_t>::max();
    std::string maximum_host_mib         = std::to_string(max_host_bytes / bytes_per_mib);
    std::size_t fractional_bytes         = max_host_bytes % bytes_per_mib;
    if (fractional_bytes != 0) { maximum_host_mib += '.'; }
    while (fractional_bytes != 0) {
        fractional_bytes *= 10;
        maximum_host_mib += static_cast<char>('0' + fractional_bytes / bytes_per_mib);
        fractional_bytes %= bytes_per_mib;
    }
    const ServeOptions maximum_host_context =
        parse({"ninfer-serve", "model.ninfer", "--host-context-mib", maximum_host_mib});
    failures += check(maximum_host_context.context_cache.host_capacity_bytes == max_host_bytes,
                      "maximum size_t Host context capacity was not accepted exactly");

    bool overflowing_host_context_rejected = false;
    try {
        (void)parse({"ninfer-serve", "model.ninfer", "--host-context-mib",
                     std::to_string(std::numeric_limits<std::size_t>::max() / (1ULL << 20) + 1)});
    } catch (const std::invalid_argument&) { overflowing_host_context_rejected = true; }
    failures +=
        check(overflowing_host_context_rejected, "overflowing Host context capacity was accepted");

    for (const std::string mib : {"0.1", "0.0000001", "0.000000476837158203125", "-1", "-0", "+1",
                                  "nan", "inf", "1e3", "1.", ".5", "", " 1", "1 ", "1.2.3"}) {
        bool invalid_host_context_rejected = false;
        try {
            (void)parse({"ninfer-serve", "model.ninfer", "--host-context-mib", mib});
        } catch (const std::invalid_argument&) { invalid_host_context_rejected = true; }
        failures +=
            check(invalid_host_context_rejected,
                  ("invalid or fractional-byte Host context MiB was accepted: " + mib).c_str());
    }

    for (const std::string option : {"--max-request-mib", "--media-cache-mib", "--media-live-mib",
                                     "--response-store-max-mib"}) {
        bool fractional_mib_rejected = false;
        try {
            (void)parse({"ninfer-serve", "model.ninfer", option, "1.5"});
        } catch (const std::invalid_argument&) { fractional_mib_rejected = true; }
        failures += check(fractional_mib_rejected,
                          ("integer-only MiB option accepted a fraction: " + option).c_str());
    }

    for (const bool disable_first : {false, true}) {
        std::vector<std::string> arguments = {"ninfer-serve", "model.ninfer"};
        if (disable_first) { arguments.push_back("--no-prefix-reuse"); }
        arguments.insert(arguments.end(),
                         {"--device-state-slots", "3", "--host-context-mib", "64"});
        if (!disable_first) { arguments.push_back("--no-prefix-reuse"); }
        const ServeOptions disabled_cache = parse(std::move(arguments));
        failures +=
            check(!disabled_cache.allow_prefix_reuse && !disabled_cache.context_cache.enabled &&
                      disabled_cache.context_cache.device_state_slots == 3 &&
                      disabled_cache.context_cache.host_capacity_bytes == (64ULL << 20),
                  "--no-prefix-reuse did not preserve independently configured capacities");
    }

    const ServeOptions response_store =
        parse({"ninfer-serve", "model.ninfer", "--response-store-max-records", "42",
               "--response-store-max-mib", "8"});
    failures += check(response_store.response_store_max_records == 42 &&
                          response_store.response_store_max_bytes == (8ULL << 20),
                      "Responses store limits did not reach serving options");

    const ServeOptions sampling =
        parse({"ninfer-serve", "model.ninfer", "--temperature", "0", "--top-p", "0.9", "--top-k",
               "20", "--min-p", "0.1", "--presence-penalty", "1.25", "--frequency-penalty", "-0.5",
               "--seed", "0"});
    failures += check(sampling.sampling_overrides.temperature == 0.0F &&
                          sampling.sampling_overrides.top_p == 0.9F &&
                          sampling.sampling_overrides.top_k == 20 &&
                          sampling.sampling_overrides.min_p == 0.1F &&
                          sampling.sampling_overrides.presence_penalty == 1.25F &&
                          sampling.sampling_overrides.frequency_penalty == -0.5F &&
                          sampling.sampling_overrides.seed == 0,
                      "server sampling flags did not preserve explicit values and zeros");
    bool oversized_top_k_rejected = false;
    try {
        (void)parse({"ninfer-serve", "model.ninfer", "--top-k", "21"});
    } catch (const std::invalid_argument&) { oversized_top_k_rejected = true; }
    failures += check(oversized_top_k_rejected,
                      "server accepted top_k beyond the executable candidate domain");

    GenerationRequest request;
    request.max_tokens   = 1;
    const auto semantics = resolve_prompt_semantics(request, defaults);
    failures += check(!semantics.reasoning_effort && !semantics.enable_thinking &&
                          !semantics.reasoning_effort,
                      "omitted reasoning effort did not resolve to the template default");
    failures +=
        check(to_request_options(request, defaults, semantics, true).execution.allow_prefix_reuse,
              "resolved read-write cache policy did not reach Engine options");
    failures +=
        check(!to_request_options(request, defaults, semantics, false).execution.allow_prefix_reuse,
              "resolved disabled cache policy inherited external enablement");
    const ninfer::RequestOptions inherited_sampling =
        to_request_options(request, sampling, semantics, sampling.allow_prefix_reuse);
    failures += check(inherited_sampling.execution.sampling.temperature == 0.0F &&
                          inherited_sampling.execution.sampling.top_p == 0.9F &&
                          inherited_sampling.execution.sampling.seed == 0,
                      "server sampling overrides did not reach Engine options");
    request.sampling.temperature = 1.1;
    failures += check(to_request_options(request, sampling, semantics, sampling.allow_prefix_reuse)
                              .execution.sampling.temperature == 1.1F,
                      "request sampling override did not win over the server override");
    failures += check(
        to_request_options(request, thinking_budget, semantics, thinking_budget.allow_prefix_reuse)
                .execution.thinking.budget == 37,
        "thinking-enabled request did not inherit the server budget");
    request.enable_thinking = false;
    const auto non_thinking = resolve_prompt_semantics(request, thinking_budget);
    failures += check(!non_thinking.reasoning_effort,
                      "disabled thinking retained an effective reasoning effort");
    failures += check(!to_request_options(request, thinking_budget, non_thinking,
                                          thinking_budget.allow_prefix_reuse)
                           .execution.thinking.budget,
                      "non-thinking request inherited the server thinking budget");
    request.enable_thinking.reset();
    request.reasoning_effort   = RequestedReasoningEffort::Low;
    const auto explicit_effort = resolve_prompt_semantics(request, defaults);
    failures += check(explicit_effort.reasoning_effort == ninfer::ReasoningEffort::Low &&
                          explicit_effort.enable_thinking == true,
                      "explicit reasoning effort did not remain the effective effort");
    request.reasoning_effort.reset();
    failures += check(resolve_prompt_semantics(request, configured).preserve_thinking == true,
                      "server preserve-thinking default was not resolved");
    request.preserve_thinking = false;
    failures += check(resolve_prompt_semantics(request, configured).preserve_thinking == false,
                      "request preserve-thinking override did not win");

    failures +=
        check(serve_usage_text("ninfer-serve").find("--no-prefix-reuse") != std::string::npos,
              "serve help omits --no-prefix-reuse");
    failures +=
        check(serve_usage_text("ninfer-serve").find("--host-context-mib") != std::string::npos,
              "serve help omits context-cache capacities");
    failures += check(serve_usage_text("ninfer-serve").find("device-state=max-concurrency") !=
                          std::string::npos,
                      "serve help omits context-cache defaults");
    failures +=
        check(serve_usage_text("ninfer-serve").find("--preserve-thinking") != std::string::npos,
              "serve help omits --preserve-thinking");
    failures += check(serve_usage_text("ninfer-serve").find("--default-thinking-budget") !=
                          std::string::npos,
                      "serve help omits --default-thinking-budget");
    failures += check(serve_usage_text("ninfer-serve").find("--vision") != std::string::npos,
                      "serve help omits --vision");
    failures +=
        check(serve_usage_text("ninfer-serve").find("--log-stats-interval-ms") != std::string::npos,
              "serve help omits --log-stats-interval-ms");
    failures += check(serve_usage_text("ninfer-serve").find("--log-level") != std::string::npos,
                      "serve help omits the log-level control");
    failures += check(serve_usage_text("ninfer-serve").find("--media-preprocess-threads") !=
                          std::string::npos,
                      "serve help omits media preparation controls");
    failures += check(serve_usage_text("ninfer-serve").find("--kv-capacity") != std::string::npos,
                      "serve help omits --kv-capacity");
    failures += check(serve_usage_text("ninfer-serve").find("--response-store-max-mib") !=
                          std::string::npos,
                      "serve help omits Responses store limits");
    failures +=
        check(serve_usage_text("ninfer-serve").find("--context-cost-presets") != std::string::npos,
              "serve help omits external context-cost presets");
    failures += check(serve_usage_text("ninfer-serve").find("metadata.name") != std::string::npos,
                      "serve help omits the artifact-derived model id default");

    const ServeOptions inherited =
        parse({"ninfer-serve", "model.ninfer", "--max-context", "16384"});
    failures += check(inherited.kv_capacity.mode == ninfer::KvCapacityMode::Explicit &&
                          inherited.kv_capacity.explicit_tokens == 16384,
                      "omitted --kv-capacity did not follow --max-context");

    const ServeOptions automatic = parse({"ninfer-serve", "model.ninfer", "--kv-capacity", "auto"});
    failures += check(automatic.kv_capacity.mode == ninfer::KvCapacityMode::Automatic &&
                          automatic.kv_capacity.explicit_tokens == 0 &&
                          automatic.kv_capacity.automatic_headroom_bytes ==
                              ninfer::kDefaultKvCapacityHeadroomBytes,
                      "--kv-capacity auto did not select automatic sizing");

    const ServeOptions logged = parse({"ninfer-serve", "model.ninfer", "--request-log-jsonl",
                                       "requests.jsonl", "--api-key", "do-not-log"});
    failures += check(logged.request_log_jsonl == "requests.jsonl",
                      "--request-log-jsonl did not preserve its path");
    failures +=
        check(serve_usage_text("ninfer-serve").find("--request-log-jsonl") != std::string::npos,
              "serve help omits --request-log-jsonl");
    bool secret_present    = false;
    bool redaction_present = false;
    for (const std::string& argument : logged.startup_argv) {
        secret_present    = secret_present || argument == "do-not-log";
        redaction_present = redaction_present || argument == "<redacted>";
    }
    failures += check(!secret_present, "startup argv retained the API key");
    failures += check(redaction_present, "startup argv omitted the API-key redaction marker");

    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
