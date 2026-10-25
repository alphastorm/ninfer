#include "serve/server_identity.h"

#include "core/sha256.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

namespace ninfer::serve {
namespace {
using Json = nlohmann::json;

bool fields(const Json& value, std::initializer_list<const char*> keys) {
    return value.is_object() && value.size() == keys.size() &&
           std::all_of(keys.begin(), keys.end(), [&](const char* key) { return value.contains(key); });
}
bool text(const Json& value) {
    if (!value.is_string()) { return false; }
    const auto& s = value.get_ref<const std::string&>();
    return !s.empty() && s != "unknown" && s != "UNKNOWN" && s.find('\0') == std::string::npos;
}
bool hex(const Json& value, std::size_t size = 64) {
    if (!value.is_string()) { return false; }
    const auto& s = value.get_ref<const std::string&>();
    return s.size() == size && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool integer(const Json& value, std::uint64_t maximum, bool positive = false) {
    if (!value.is_number_integer()) { return false; }
    if (!value.is_number_unsigned() && value.get<std::int64_t>() < 0) { return false; }
    const auto n = value.get<std::uint64_t>();
    return n <= maximum && (!positive || n != 0);
}
bool u32(const Json& value, bool positive = false) {
    return integer(value, std::numeric_limits<std::uint32_t>::max(), positive);
}
bool u64(const Json& value, bool positive = false) {
    return integer(value, std::numeric_limits<std::uint64_t>::max(), positive);
}
bool named(const Json& value, std::initializer_list<std::string_view> names) {
    if (!value.is_string()) { return false; }
    const auto& s = value.get_ref<const std::string&>();
    return std::find(names.begin(), names.end(), s) != names.end();
}
std::string sha256_text(const std::string& value) {
    return crypto::sha256_hex(crypto::sha256(std::as_bytes(std::span(value.data(), value.size()))));
}
Json projection(const Json& value) {
    return {{"engine", value.at("engine")}, {"frontend", value.at("frontend")},
            {"model", value.at("model")}, {"native_layout", value.at("native_layout")},
            {"responses", value.at("responses")}};
}
Json fingerprint_json(const runtime::RuntimeFingerprintV2& v) {
    Json pools = Json::array();
    for (const auto& p : v.native_layout.pools) {
        pools.push_back({{"pool_id", p.pool_id}, {"name", p.name},
            {"layout_sha256", p.layout_sha256}, {"storage_type", p.storage_type},
            {"layers", p.layers}, {"tokens_per_page", p.tokens_per_page},
            {"kv_heads", p.kv_heads}, {"head_dim", p.head_dim}, {"page_bytes", p.page_bytes},
            {"host_serialization_stride", p.host_serialization_stride}});
    }
    return {
        {"artifact_type", runtime::kRuntimeFingerprintArtifact},
        {"schema_version", runtime::kRuntimeFingerprintVersion},
        {"build", {{"upstream_base_sha", v.build.upstream_base_sha},
            {"patch_stack_sha", v.build.patch_stack_sha}, {"source_dirty", v.build.source_dirty},
            {"build_profile", v.build.build_profile}, {"build_type", v.build.build_type},
            {"binary_sha256", v.build.binary_sha256}, {"platform", v.build.platform},
            {"cxx_compiler", v.build.cxx_compiler}, {"cuda_compiler", v.build.cuda_compiler},
            {"cuda_toolkit", v.build.cuda_toolkit}, {"cuda_architecture", v.build.cuda_architecture}}},
        {"model", {{"artifact_sha256", v.model.artifact_sha256},
            {"architecture", v.model.architecture}, {"model_id", v.model.model_id},
            {"weights_id", v.model.weights_id}}},
        {"frontend", {{"template_sha256", v.frontend.template_sha256},
            {"tokenizer_sha256", v.frontend.tokenizer_sha256},
            {"resources_sha256", v.frontend.resources_sha256},
            {"vision_preprocessing", v.frontend.vision_preprocessing},
            {"parser_codec", v.frontend.parser_codec}, {"identity_codec", v.frontend.identity_codec}}},
        {"native_layout", {{"continuation_codec", v.native_layout.continuation_codec},
            {"state_layout_sha256", v.native_layout.state_layout_sha256},
            {"state_bytes", v.native_layout.state_bytes}, {"pools", std::move(pools)}}},
        {"engine", {{"max_context", v.engine.max_context}, {"kv_capacity", v.engine.kv_capacity},
            {"kv_dtype", v.engine.kv_dtype}, {"max_concurrency", v.engine.max_concurrency},
            {"prefill_chunk", v.engine.prefill_chunk},
            {"speculative_backend", v.engine.speculative_backend}, {"draft_window", v.engine.draft_window},
            {"proposal_head", v.engine.proposal_head}, {"enable_vision", v.engine.enable_vision},
            {"use_cuda_graph", v.engine.use_cuda_graph}}},
        {"responses", {{"codec", v.responses.codec}}},
        {"deployment", {{"profile", v.deployment.profile},
            {"effective_config_sha256", v.deployment.effective_config_sha256}}},
    };
}
} // namespace

bool validate_session_checkpoint_runtime_fingerprint(const Json& value) noexcept {
    try {
        if (!fields(value, {"artifact_type", "schema_version", "build", "model", "frontend",
                            "native_layout", "engine", "responses", "deployment"})) { return false; }
        const auto& artifact_type = value.at("artifact_type");
        if (!artifact_type.is_string() ||
            artifact_type.get_ref<const std::string&>() != runtime::kRuntimeFingerprintArtifact ||
            !u32(value.at("schema_version")) ||
            value.at("schema_version") != runtime::kRuntimeFingerprintVersion) { return false; }
        const auto& b = value.at("build");
        if (!fields(b, {"upstream_base_sha", "patch_stack_sha", "source_dirty", "build_profile",
                        "build_type", "binary_sha256", "platform", "cxx_compiler", "cuda_compiler",
                        "cuda_toolkit", "cuda_architecture"}) ||
            !hex(b.at("upstream_base_sha"), 40) || !hex(b.at("patch_stack_sha"), 40) ||
            !b.at("source_dirty").is_boolean() || b.at("source_dirty").get<bool>() ||
            !hex(b.at("binary_sha256"))) { return false; }
        for (const auto* key : {"build_profile", "build_type", "platform", "cxx_compiler",
                                "cuda_compiler", "cuda_toolkit", "cuda_architecture"}) {
            if (!text(b.at(key))) { return false; }
        }
        const auto& m = value.at("model");
        if (!fields(m, {"artifact_sha256", "architecture", "model_id", "weights_id"}) ||
            !hex(m.at("artifact_sha256")) || !text(m.at("architecture")) ||
            !text(m.at("model_id")) || !text(m.at("weights_id"))) { return false; }
        const auto& f = value.at("frontend");
        if (!fields(f, {"template_sha256", "tokenizer_sha256", "resources_sha256",
                        "vision_preprocessing", "parser_codec", "identity_codec"}) ||
            !hex(f.at("template_sha256")) || !hex(f.at("tokenizer_sha256")) ||
            !hex(f.at("resources_sha256")) || !text(f.at("parser_codec")) ||
            !text(f.at("identity_codec"))) { return false; }
        const auto& n = value.at("native_layout");
        if (!fields(n, {"continuation_codec", "state_layout_sha256", "state_bytes", "pools"}) ||
            !text(n.at("continuation_codec")) || !hex(n.at("state_layout_sha256")) ||
            !u64(n.at("state_bytes"), true) || !n.at("pools").is_array()) { return false; }
        std::uint32_t previous_pool = 0;
        bool first_pool = true;
        for (const auto& p : n.at("pools")) {
            if (!fields(p, {"pool_id", "name", "layout_sha256", "storage_type", "layers",
                            "tokens_per_page", "kv_heads", "head_dim", "page_bytes",
                            "host_serialization_stride"}) || !u32(p.at("pool_id")) ||
                !text(p.at("name")) || !hex(p.at("layout_sha256")) ||
                !text(p.at("storage_type")) || !u32(p.at("layers"), true) ||
                !u32(p.at("tokens_per_page"), true) || !u32(p.at("kv_heads"), true) ||
                !u32(p.at("head_dim"), true) || !u64(p.at("page_bytes"), true) ||
                !u64(p.at("host_serialization_stride"), true)) { return false; }
            const auto pool_id = p.at("pool_id").get<std::uint32_t>();
            if (!first_pool && pool_id <= previous_pool) { return false; }
            first_pool = false;
            previous_pool = pool_id;
        }
        const auto& e = value.at("engine");
        if (!fields(e, {"max_context", "kv_capacity", "kv_dtype", "max_concurrency", "prefill_chunk",
                        "speculative_backend", "draft_window", "proposal_head", "enable_vision",
                        "use_cuda_graph"}) || !u32(e.at("max_context"), true) ||
            !u32(e.at("kv_capacity"), true) || !u32(e.at("max_concurrency"), true) ||
            !u32(e.at("prefill_chunk"), true) || !u32(e.at("draft_window")) ||
            !named(e.at("kv_dtype"), {"bf16", "int8", "fp8", "k8v4"}) ||
            !named(e.at("speculative_backend"), {"none", "mtp", "dflash", "dflash2"}) ||
            !named(e.at("proposal_head"), {"full", "optimized"}) ||
            !e.at("enable_vision").is_boolean() || !e.at("use_cuda_graph").is_boolean()) { return false; }
        if ((e.at("speculative_backend") == "none") != (e.at("draft_window") == 0)) { return false; }
        if (e.at("enable_vision").get<bool>() ? !hex(f.at("vision_preprocessing"))
                                              : f.at("vision_preprocessing") != "disabled") { return false; }
        if (!fields(value.at("responses"), {"codec"}) ||
            !text(value.at("responses").at("codec"))) { return false; }
        const auto& d = value.at("deployment");
        return fields(d, {"profile", "effective_config_sha256"}) && text(d.at("profile")) &&
               hex(d.at("effective_config_sha256")) &&
               d.at("effective_config_sha256") == sha256_text(projection(value).dump());
    } catch (...) { return false; }
}

std::string session_checkpoint_effective_config_sha256(const runtime::RuntimeFingerprintV2& value) {
    return sha256_text(projection(fingerprint_json(value)).dump());
}
Json session_checkpoint_runtime_fingerprint(const runtime::RuntimeFingerprintV2& value) {
    Json result = fingerprint_json(value);
    if (!validate_session_checkpoint_runtime_fingerprint(result)) {
        throw std::invalid_argument("incomplete or invalid checkpoint runtime fingerprint v2");
    }
    return result;
}

} // namespace ninfer::serve
