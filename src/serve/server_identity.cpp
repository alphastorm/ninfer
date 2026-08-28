#include "serve/server_identity.h"

#include "core/sha256.h"
#include "ninfer/build_info.h"

#include <nlohmann/json.hpp>

#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ninfer::serve {
namespace {

nlohmann::json optional_identity(const std::string& value) {
    return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
}

} // namespace

nlohmann::json server_identity_json(const ServeOptions& options, const ninfer::LoadSummary& load) {
    const ninfer::BuildInfo build = ninfer::build_info();
    return nlohmann::json{
        {"upstream_base_sha", std::string(build.upstream_base_sha)},
        {"patch_stack_sha", std::string(build.patch_stack_sha)},
        {"source_dirty", build.source_dirty},
        {"build_profile", std::string(build.build_profile)},
        {"build_type", std::string(build.build_type)},
        {"cxx_compiler", std::string(build.cxx_compiler)},
        {"cuda_compiler", std::string(build.cuda_compiler)},
        {"cuda_toolkit", std::string(build.cuda_toolkit)},
        {"cuda_architecture", std::string(build.cuda_architecture)},
        {"deployment_profile", optional_identity(options.deployment_profile)},
        {"binary_sha256", optional_identity(options.binary_sha256)},
        {"model_artifact_sha256", optional_identity(options.artifact_sha256)},
        {"config_sha256", optional_identity(options.config_sha256)},
        {"target", load.target},
        {"model_id", load.model_id},
        {"weights_id", load.weights_id},
    };
}

std::string session_checkpoint_tenant_sha256(std::string_view api_key) {
    if (api_key.empty()) {
        throw std::invalid_argument("checkpoint tenant derivation requires an API key");
    }
    constexpr std::string_view domain = "ninfer-session-checkpoint-tenant-v1";
    constexpr std::byte separator{0};
    crypto::Sha256 hasher;
    hasher.update(std::as_bytes(std::span(domain.data(), domain.size())));
    hasher.update(std::span(&separator, 1));
    hasher.update(std::as_bytes(std::span(api_key.data(), api_key.size())));
    return crypto::sha256_hex(hasher.finish());
}

nlohmann::json session_checkpoint_runtime_fingerprint(
    const ServeOptions& options, const ninfer::EngineOptions& engine,
    const ninfer::LoadSummary& load, const ninfer::MemorySummary& memory,
    const ninfer::BuildInfo& build) {
    return nlohmann::json{
        {"artifact_type", "ninfer_session_checkpoint_runtime"},
        {"schema_version", 1},
        {"identity",
         {{"binary_sha256", options.binary_sha256},
          {"artifact_sha256", options.artifact_sha256},
          {"config_sha256", options.config_sha256},
          {"deployment_profile", options.deployment_profile},
          {"target", load.target},
          {"model_id", load.model_id},
          {"weights_id", load.weights_id}}},
        {"build",
         {{"upstream_base_sha", std::string(build.upstream_base_sha)},
          {"patch_stack_sha", std::string(build.patch_stack_sha)},
          {"build_profile", std::string(build.build_profile)},
          {"build_type", std::string(build.build_type)},
          {"cxx_compiler", std::string(build.cxx_compiler)},
          {"cuda_compiler", std::string(build.cuda_compiler)},
          {"cuda_toolkit", std::string(build.cuda_toolkit)},
          {"cuda_architecture", std::string(build.cuda_architecture)},
          {"source_dirty", build.source_dirty}}},
        {"engine",
         {{"max_context", engine.max_context},
          {"kv_capacity_mode", static_cast<int>(engine.kv_capacity.mode)},
          {"kv_capacity_tokens", engine.kv_capacity.explicit_tokens},
          {"kv_capacity_headroom_bytes", engine.kv_capacity.automatic_headroom_bytes},
          {"max_concurrency", engine.max_concurrency},
          {"prefill_chunk", engine.prefill_chunk},
          {"kv_cache", static_cast<int>(engine.kv_cache)},
          {"speculative_backend", static_cast<int>(engine.speculative.backend)},
          {"speculative_draft_tokens", engine.speculative.draft_tokens},
          {"speculative_proposal_head", static_cast<int>(engine.speculative.proposal_head)},
          {"enable_vision", engine.enable_vision},
          {"use_cuda_graph", engine.use_cuda_graph},
          {"allow_prefix_reuse", options.allow_prefix_reuse}}},
        {"target_layout",
         {{"max_context", memory.max_context},
          {"kv_capacity_tokens", memory.kv_capacity},
          {"kv_capacity_page_groups", memory.kv_capacity_page_groups},
          {"kv_cache", static_cast<int>(memory.kv_cache)},
          {"sequence_capacity_bytes", memory.sequence.capacity_bytes},
          {"workspace_capacity_bytes", memory.workspace.capacity_bytes},
          {"request_transient_capacity_bytes", memory.request_transient.capacity_bytes},
          {"minimum_runtime_reservation_bytes", memory.minimum_runtime_reservation_bytes},
          {"runtime_reservation_bytes", memory.runtime_reservation_bytes},
          {"kv_capacity_increment_bytes", memory.kv_capacity_increment_bytes},
          {"kv_payload_bytes", memory.kv_payload_bytes}}},
        {"responses",
         {{"max_records", options.response_store_max_records},
          {"max_bytes", options.response_store_max_bytes}}},
    };
}

std::string format_server_identity(const ServeOptions& options, const ninfer::LoadSummary& load) {
    const ninfer::BuildInfo build = ninfer::build_info();
    const auto value_or_unknown   = [](const std::string& value) -> const std::string& {
        static const std::string unknown = "unknown";
        return value.empty() ? unknown : value;
    };
    std::ostringstream out;
    out << "identity upstream=" << build.upstream_base_sha << " patch=" << build.patch_stack_sha
        << " dirty=" << (build.source_dirty ? "true" : "false") << " build=" << build.build_profile
        << " deployment=" << value_or_unknown(options.deployment_profile)
        << " binary_sha256=" << value_or_unknown(options.binary_sha256)
        << " artifact_sha256=" << value_or_unknown(options.artifact_sha256)
        << " config_sha256=" << value_or_unknown(options.config_sha256)
        << " model=" << load.model_id << " weights=" << load.weights_id;
    return out.str();
}

} // namespace ninfer::serve