#include "serve/serve_options.h"
#include "product/speculative_options.h"
#include "runtime/contract/continuation_checkpoint.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ninfer::serve {
namespace {

int parse_nonnegative_int(const char* text, const char* label) {
    char* end        = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 ||
        value > static_cast<long>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument(std::string("invalid ") + label + ": " + text);
    }
    return static_cast<int>(value);
}

float parse_float_in(const char* text, const char* label, float lo, float hi) {
    char* end          = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !(value >= lo) || !(value <= hi)) {
        throw std::invalid_argument(std::string("invalid ") + label + ": " + text);
    }
    return static_cast<float>(value);
}

std::uint64_t parse_u64(const char* text, const char* label) {
    if (text == nullptr || *text == '\0' || *text == '-') {
        throw std::invalid_argument(std::string("invalid ") + label + ": " +
                                    (text == nullptr ? "" : text));
    }
    errno                          = 0;
    char* end                      = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0') {
        throw std::invalid_argument(std::string("invalid ") + label + ": " + text);
    }
    return static_cast<std::uint64_t>(value);
}

KvCacheStorage parse_kv_dtype(const char* text) {
    const std::string value(text);
    if (value == "bf16") { return KvCacheStorage::BFloat16; }
    if (value == "int8") { return KvCacheStorage::Int8Group64; }
    if (value == "rk8v4") { return KvCacheStorage::RotatedInt8KeyInt4ValueGroup64; }
    throw std::invalid_argument("invalid kv-dtype: " + value);
}

KvCapacityPolicy parse_kv_capacity(const char* text) {
    if (std::string_view(text) == "auto") { return KvCapacityPolicy::automatic(); }
    const int value = parse_nonnegative_int(text, "kv-capacity");
    if (value == 0) { throw std::invalid_argument("--kv-capacity must be positive"); }
    return KvCapacityPolicy::explicit_capacity(static_cast<std::uint32_t>(value));
}

std::string parse_sha256(const char* text, const char* label) {
    const std::string value(text == nullptr ? "" : text);
    const bool valid =
        value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    if (!valid) {
        throw std::invalid_argument(std::string(label) +
                                    " must be a 64-character lowercase SHA-256");
    }
    return value;
}

std::string parse_profile_name(const char* text) {
    const std::string value(text == nullptr ? "" : text);
    const bool valid = !value.empty() && value.size() <= 64 &&
                       std::all_of(value.begin(), value.end(), [](unsigned char c) {
                           return std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-';
                       });
    if (!valid) {
        throw std::invalid_argument("--deployment-profile must match [A-Za-z0-9._-]{1,64}");
    }
    return value;
}

std::string read_api_key_file(const char* path) {
    if (path == nullptr || *path == '\0') {
        throw std::invalid_argument("--api-key-file must not be empty");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::invalid_argument("cannot open --api-key-file"); }
    std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) { throw std::invalid_argument("cannot read --api-key-file"); }
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    if (value.empty() || value.find('\0') != std::string::npos ||
        value.find('\n') != std::string::npos || value.find('\r') != std::string::npos) {
        throw std::invalid_argument("--api-key-file must contain exactly one non-empty line");
    }
    return value;
}

} // namespace

std::string serve_usage_text(const char* argv0) {
    return std::string("usage: ") + argv0 +
           " <model.ninfer> [--host H] [--port N] [--api-key KEY|--api-key-file FILE] "
           "[--model-id ID] [--binary-sha256 SHA] [--artifact-sha256 SHA] "
           "[--config-sha256 SHA] [--deployment-profile NAME] "
           "[--max-context N] [--kv-capacity N|auto] [--max-concurrency N] "
           "[--max-pending-requests N] [--pending-timeout-ms N] "
           "[--prefill-chunk N] [--log-stats-interval-ms N] [--device N] "
           "[--max-request-mib N] [--request-log-jsonl FILE] "
           "[--response-store-max-records N] [--response-store-max-mib N] "
           "[--session-checkpoint-dir DIR] [--session-checkpoint-quota-mib N] "
           "[--session-checkpoint-staging-mib N] [--session-checkpoint-write-buffer-mib N] "
           "[--session-checkpoint-min-tokens N] [--session-checkpoint-require-origin-auth] "
           "[--kv-dtype bf16|int8|rk8v4] [--spec mtp|dflash --draft-tokens N] "
           "[--default-max-tokens N] "
           "[--vision] [--no-cuda-graph] [--no-prefix-reuse] "
           "[--lm-head-draft] [--no-thinking] [--preserve-thinking] "
           "[--reasoning-effort low|medium|xhigh] [--cors] "
           "[--temperature F] [--top-p F] [--top-k N] [--min-p F] [--presence-penalty F] "
           "[--frequency-penalty F] [--seed N] [--greedy]\n"
           "       serves OpenAI Responses/Chat Completions and Anthropic Messages endpoints\n"
           "       --default-max-tokens defaults to " +
           std::to_string(kDefaultMaxTokens) +
           " when omitted\n"
           "       --max-request-mib defaults to 384 and is enforced before JSON parsing\n"
           "       --request-log-jsonl appends full-precision server/request records\n"
           "       --model-id overrides the artifact identity.model_id reported by the server\n"
           "       Responses state is process-local and bounded to 1024 records / 256 MiB by "
           "default\n"
           "       --log-stats-interval-ms defaults to 5000; 0 disables periodic throughput logs\n"
           "       --session-checkpoint-dir enables authenticated restart continuation; lifecycle "
           "identities are required\n"
           "       --session-checkpoint-require-origin-auth refuses generations without a valid "
           "origin MAC (NAS/S3 import posture; 32-character bearer floor)\n"
           "       --vision enables media and loads the fixed Vision GPU allocations\n"
           "       --kv-capacity auto leaves " +
           std::to_string(kDefaultKvCapacityHeadroomBytes / (1024ULL * 1024ULL)) +
           " MiB of sizing headroom\n"
           "       --no-prefix-reuse disables compatible-prefix caching (enabled by default)\n"
           "       --preserve-thinking retains closed-turn assistant reasoning in later prompts\n"
           "       --reasoning-effort sets default thinking depth (low|medium|xhigh) when omitted by client\n"
           "       sampler defaults come from the loaded model and resolved thinking mode; "
           "server flags and request fields override individual values.\n"
           "       --greedy forces temperature 0 (exact argmax).\n";
}

ServeOptions parse_serve_options(int argc, char** argv) {
    ServeOptions options;
    options.startup_argv.reserve(static_cast<std::size_t>(argc));
    bool redact_next = false;
    for (int i = 0; i < argc; ++i) {
        if (redact_next) {
            options.startup_argv.emplace_back("<redacted>");
            redact_next = false;
            continue;
        }
        options.startup_argv.emplace_back(argv[i] == nullptr ? "" : argv[i]);
        redact_next = options.startup_argv.back() == "--api-key";
    }
    bool default_max_tokens_explicit = false;
    bool kv_capacity_explicit        = false;
    bool api_key_direct              = false;
    bool api_key_file                = false;
    if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        options.help_requested = true;
        return options;
    }

    if (argc < 2) { throw std::invalid_argument("artifact path is required"); }
    options.artifact_path = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string arg    = argv[i];
        const auto require_value = [&](const char* flag) -> const char* {
            if (++i >= argc) { throw std::invalid_argument(std::string(flag) + " needs a value"); }
            return argv[i];
        };
        if (arg == "--host") {
            options.host = require_value("--host");
        } else if (arg == "--port") {
            options.port = parse_nonnegative_int(require_value("--port"), "port");
        } else if (arg == "--api-key") {
            if (api_key_file) {
                throw std::invalid_argument(
                    "--api-key and --api-key-file are mutually exclusive");
            }
            options.api_key = require_value("--api-key");
            if (options.api_key.empty()) {
                throw std::invalid_argument("--api-key must not be empty");
            }
            api_key_direct = true;
        } else if (arg == "--api-key-file") {
            if (api_key_direct) {
                throw std::invalid_argument(
                    "--api-key and --api-key-file are mutually exclusive");
            }
            options.api_key = read_api_key_file(require_value("--api-key-file"));
            api_key_file = true;
        } else if (arg == "--model-id") {
            options.model_id_override = require_value("--model-id");
            if (options.model_id_override->empty()) {
                throw std::invalid_argument("--model-id must not be empty");
            }
        } else if (arg == "--binary-sha256") {
            options.binary_sha256 =
                parse_sha256(require_value("--binary-sha256"), "--binary-sha256");
        } else if (arg == "--artifact-sha256") {
            options.artifact_sha256 =
                parse_sha256(require_value("--artifact-sha256"), "--artifact-sha256");
        } else if (arg == "--config-sha256") {
            options.config_sha256 =
                parse_sha256(require_value("--config-sha256"), "--config-sha256");
        } else if (arg == "--deployment-profile") {
            options.deployment_profile = parse_profile_name(require_value("--deployment-profile"));
        } else if (arg == "--max-context") {
            options.max_context = static_cast<std::uint32_t>(
                parse_nonnegative_int(require_value("--max-context"), "max-context"));
        } else if (arg == "--kv-capacity") {
            options.kv_capacity  = parse_kv_capacity(require_value("--kv-capacity"));
            kv_capacity_explicit = true;
        } else if (arg == "--max-concurrency") {
            options.max_concurrency = static_cast<std::uint32_t>(
                parse_nonnegative_int(require_value("--max-concurrency"), "max-concurrency"));
        } else if (arg == "--max-pending-requests") {
            options.max_pending_requests = static_cast<std::uint32_t>(parse_nonnegative_int(
                require_value("--max-pending-requests"), "max-pending-requests"));
        } else if (arg == "--pending-timeout-ms") {
            options.pending_timeout_ms = static_cast<std::uint32_t>(
                parse_nonnegative_int(require_value("--pending-timeout-ms"), "pending-timeout-ms"));
        } else if (arg == "--prefill-chunk") {
            options.prefill_chunk = static_cast<std::uint32_t>(
                parse_nonnegative_int(require_value("--prefill-chunk"), "prefill-chunk"));
        } else if (arg == "--log-stats-interval-ms") {
            options.log_stats_interval_ms = static_cast<std::uint32_t>(parse_nonnegative_int(
                require_value("--log-stats-interval-ms"), "log-stats-interval-ms"));
        } else if (arg == "--max-request-mib") {
            const std::uint64_t mib =
                parse_u64(require_value("--max-request-mib"), "max-request-mib");
            if (mib == 0 || mib > std::numeric_limits<std::size_t>::max() / (1ULL << 20)) {
                throw std::invalid_argument("--max-request-mib is out of range");
            }
            options.max_request_bytes = static_cast<std::size_t>(mib << 20);
        } else if (arg == "--request-log-jsonl") {
            options.request_log_jsonl = require_value("--request-log-jsonl");
            if (options.request_log_jsonl.empty()) {
                throw std::invalid_argument("--request-log-jsonl must not be empty");
            }
        } else if (arg == "--response-store-max-records") {
            const int records = parse_nonnegative_int(require_value("--response-store-max-records"),
                                                      "response-store-max-records");
            if (records == 0) {
                throw std::invalid_argument("--response-store-max-records must be positive");
            }
            options.response_store_max_records = static_cast<std::size_t>(records);
        } else if (arg == "--response-store-max-mib") {
            const std::uint64_t mib =
                parse_u64(require_value("--response-store-max-mib"), "response-store-max-mib");
            if (mib == 0 || mib > std::numeric_limits<std::size_t>::max() / (1ULL << 20)) {
                throw std::invalid_argument("--response-store-max-mib is out of range");
            }
            options.response_store_max_bytes = static_cast<std::size_t>(mib << 20);
        } else if (arg == "--session-checkpoint-dir") {
            options.session_checkpoint_root = require_value("--session-checkpoint-dir");
            if (options.session_checkpoint_root.empty()) {
                throw std::invalid_argument("--session-checkpoint-dir must not be empty");
            }
        } else if (arg == "--session-checkpoint-quota-mib") {
            const std::uint64_t mib = parse_u64(
                require_value("--session-checkpoint-quota-mib"), "session-checkpoint-quota-mib");
            if (mib == 0 || mib > std::numeric_limits<std::uint64_t>::max() / (1ULL << 20)) {
                throw std::invalid_argument("--session-checkpoint-quota-mib is out of range");
            }
            options.session_checkpoint_quota_bytes = mib << 20;
        } else if (arg == "--session-checkpoint-require-origin-auth") {
            options.session_checkpoint_require_origin_auth = true;
        } else if (arg == "--session-checkpoint-staging-mib") {
            const std::uint64_t mib = parse_u64(require_value("--session-checkpoint-staging-mib"),
                                                "session-checkpoint-staging-mib");
            if (mib == 0 || mib > std::numeric_limits<std::size_t>::max() / (1ULL << 20)) {
                throw std::invalid_argument("--session-checkpoint-staging-mib is out of range");
            }
            options.session_checkpoint_staging_bytes = static_cast<std::size_t>(mib << 20);
        } else if (arg == "--session-checkpoint-write-buffer-mib") {
            const std::uint64_t mib =
                parse_u64(require_value("--session-checkpoint-write-buffer-mib"),
                          "session-checkpoint-write-buffer-mib");
            if (mib == 0 || mib > std::numeric_limits<std::size_t>::max() / (1ULL << 20)) {
                throw std::invalid_argument(
                    "--session-checkpoint-write-buffer-mib is out of range");
            }
            options.session_checkpoint_write_buffer_bytes = static_cast<std::size_t>(mib << 20);
        } else if (arg == "--session-checkpoint-min-tokens") {
            const std::uint64_t tokens = parse_u64(require_value("--session-checkpoint-min-tokens"),
                                                   "session-checkpoint-min-tokens");
            if (tokens > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("--session-checkpoint-min-tokens is out of range");
            }
            options.session_checkpoint_min_tokens = static_cast<std::uint32_t>(tokens);
        } else if (arg == "--device") {
            options.device = parse_nonnegative_int(require_value("--device"), "device");
        } else if (arg == "--kv-dtype") {
            options.kv_cache = parse_kv_dtype(require_value("--kv-dtype"));
        } else if (arg == "--spec") {
            options.speculative.backend =
                product::parse_speculative_backend(require_value("--spec"));
        } else if (arg == "--draft-tokens") {
            options.speculative.draft_tokens = static_cast<std::uint32_t>(
                parse_nonnegative_int(require_value("--draft-tokens"), "draft-tokens"));
        } else if (arg == "--default-max-tokens") {
            options.default_max_tokens =
                parse_nonnegative_int(require_value("--default-max-tokens"), "default-max-tokens");
            default_max_tokens_explicit = true;
        } else if (arg == "--vision") {
            options.enable_vision = true;
        } else if (arg == "--no-cuda-graph") {
            options.use_cuda_graph = false;
        } else if (arg == "--no-prefix-reuse") {
            options.allow_prefix_reuse = false;
        } else if (arg == "--lm-head-draft") {
            options.speculative.proposal_head = ProposalHead::Optimized;
        } else if (arg == "--no-thinking") {
            options.enable_thinking = false;
        } else if (arg == "--preserve-thinking") {
            options.preserve_thinking = true;
        } else if (arg == "--reasoning-effort" || arg == "--thinking-effort") {
            const std::string val = require_value(arg.c_str());
            const auto effort     = parse_requested_reasoning_effort(val);
            if (!effort) {
                throw std::invalid_argument("invalid value for " + arg + ": '" + val +
                                            "' (expected low, medium, or xhigh)");
            }
            options.default_reasoning_effort = *effort;
        } else if (arg == "--cors") {
            options.enable_cors = true;
        } else if (arg == "--temperature") {
            options.sampling_overrides.temperature =
                parse_float_in(require_value("--temperature"), "temperature", 0.0f, 2.0f);
        } else if (arg == "--top-p") {
            options.sampling_overrides.top_p =
                parse_float_in(require_value("--top-p"), "top-p", 0.0f, 1.0f);
        } else if (arg == "--top-k") {
            options.sampling_overrides.top_k =
                parse_nonnegative_int(require_value("--top-k"), "top-k");
        } else if (arg == "--min-p") {
            options.sampling_overrides.min_p =
                parse_float_in(require_value("--min-p"), "min-p", 0.0f, 1.0f);
        } else if (arg == "--presence-penalty") {
            options.sampling_overrides.presence_penalty = parse_float_in(
                require_value("--presence-penalty"), "presence-penalty", -2.0f, 2.0f);
        } else if (arg == "--frequency-penalty") {
            options.sampling_overrides.frequency_penalty = parse_float_in(
                require_value("--frequency-penalty"), "frequency-penalty", -2.0f, 2.0f);
        } else if (arg == "--seed") {
            options.sampling_overrides.seed = parse_u64(require_value("--seed"), "seed");
        } else if (arg == "--greedy") {
            options.greedy = true;
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    if (!kv_capacity_explicit) {
        options.kv_capacity = KvCapacityPolicy::explicit_capacity(options.max_context);
    }
    if (options.port <= 0 || options.port > 65535) {
        throw std::invalid_argument("--port must be in [1,65535]");
    }
    if (options.max_context == 0) { throw std::invalid_argument("--max-context must be positive"); }
    if (options.kv_capacity.mode == KvCapacityMode::Explicit &&
        options.kv_capacity.explicit_tokens < options.max_context) {
        throw std::invalid_argument("--kv-capacity must be at least --max-context");
    }
    if (options.max_concurrency == 0 || options.max_concurrency > kMaximumConcurrency) {
        throw std::invalid_argument("--max-concurrency must be in [1,8]");
    }
    if (options.max_pending_requests == 0) {
        throw std::invalid_argument("--max-pending-requests must be positive");
    }
    if (options.pending_timeout_ms == 0) {
        throw std::invalid_argument("--pending-timeout-ms must be positive");
    }
    if (options.max_request_bytes == 0) {
        throw std::invalid_argument("--max-request-mib must be positive");
    }
    if (options.prefill_chunk == 0 || options.prefill_chunk % 128 != 0) {
        throw std::invalid_argument("--prefill-chunk must be a positive multiple of 128");
    }
    product::validate_speculative_cli_options(options.speculative);
    if (options.speculative.backend == SpeculativeBackend::DFlash && options.enable_vision) {
        throw std::invalid_argument("--spec dflash cannot be combined with --vision");
    }
    if (default_max_tokens_explicit) {
        if (options.default_max_tokens <= 0) {
            throw std::invalid_argument("--default-max-tokens must be positive");
        }
    }
    validate_session_checkpoint_options(options);
    return options;
}

void validate_session_checkpoint_options(const ServeOptions& options) {
    if (options.session_checkpoint_require_origin_auth && options.session_checkpoint_root.empty()) {
        throw std::invalid_argument(
            "--session-checkpoint-require-origin-auth requires --session-checkpoint-dir");
    }
    if (options.session_checkpoint_root.empty()) { return; }
    if (options.api_key.empty()) {
        throw std::invalid_argument("--session-checkpoint-dir requires --api-key");
    }
    // The persisted origin tag is an offline verifier for the bearer credential: a
    // checkpoint-root reader can test key guesses against manifest.mac without touching the
    // server. The strict import posture therefore demands a credential outside dictionary
    // reach.
    if (options.session_checkpoint_require_origin_auth && options.api_key.size() < 32) {
        throw std::invalid_argument(
            "--session-checkpoint-require-origin-auth requires an API key of at least "
            "32 characters");
    }
    if (!runtime::AuthenticatedCheckpointNamespace::valid_sha256(options.binary_sha256) ||
        !runtime::AuthenticatedCheckpointNamespace::valid_sha256(options.artifact_sha256) ||
        !runtime::AuthenticatedCheckpointNamespace::valid_sha256(options.config_sha256) ||
        options.deployment_profile.empty()) {
        throw std::invalid_argument(
            "--session-checkpoint-dir requires nonempty --binary-sha256, --artifact-sha256, "
            "--config-sha256, and --deployment-profile identities");
    }
    if (options.session_checkpoint_quota_bytes == 0 ||
        options.session_checkpoint_staging_bytes == 0 ||
        options.session_checkpoint_staging_bytes > options.session_checkpoint_quota_bytes) {
        throw std::invalid_argument(
            "session checkpoint quota and staging must be positive with staging no larger than quota");
    }
}

std::string resolve_public_model_id(const ServeOptions& options,
                                    std::string_view artifact_model_id) {
    if (options.model_id_override.has_value()) { return *options.model_id_override; }
    if (artifact_model_id.empty()) {
        throw std::logic_error("loaded artifact model_id must not be empty");
    }
    return std::string(artifact_model_id);
}

} // namespace ninfer::serve
