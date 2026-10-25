#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ninfer::runtime {

inline constexpr std::uint32_t kRuntimeFingerprintVersion = 2;
inline constexpr std::string_view kRuntimeFingerprintArtifact = "ninfer_session_checkpoint_runtime";

// All digests are 64 lowercase hexadecimal characters, commits are full 40-character SHAs.
// Wire keys match member names. No enum ordinals, optional identity fields, paths or secrets.
struct DurableBuildIdentity {
    std::string upstream_base_sha;
    std::string patch_stack_sha;
    bool source_dirty = false;
    std::string build_profile;
    std::string build_type;
    std::string binary_sha256;
    std::string platform;
    std::string cxx_compiler;
    std::string cuda_compiler;
    std::string cuda_toolkit;
    std::string cuda_architecture;
    friend bool operator==(const DurableBuildIdentity&, const DurableBuildIdentity&) = default;
};

struct DurableModelIdentity {
    std::string artifact_sha256;
    std::string architecture;
    std::string model_id;
    std::string weights_id;
    friend bool operator==(const DurableModelIdentity&, const DurableModelIdentity&) = default;
};

struct DurableFrontendIdentity {
    std::string template_sha256;
    std::string tokenizer_sha256;
    std::string resources_sha256;
    // "disabled" when Vision is disabled; otherwise a SHA-256 digest of preprocessing identity.
    std::string vision_preprocessing;
    std::string parser_codec;
    std::string identity_codec;
    friend bool operator==(const DurableFrontendIdentity&, const DurableFrontendIdentity&) = default;
};

// Ordered by pool_id, not by runtime address. A stride is layout, not pool capacity.
struct DurableKvPoolLayout {
    std::uint32_t pool_id = 0;
    std::string name;
    std::string layout_sha256;
    std::string storage_type;
    std::uint32_t layers = 0;
    std::uint32_t tokens_per_page = 0;
    std::uint32_t kv_heads = 0;
    std::uint32_t head_dim = 0;
    std::uint64_t page_bytes = 0;
    std::uint64_t host_serialization_stride = 0;
    friend bool operator==(const DurableKvPoolLayout&, const DurableKvPoolLayout&) = default;
};

struct DurableNativeLayout {
    std::string continuation_codec;
    std::string state_layout_sha256;
    std::uint64_t state_bytes = 0;
    std::vector<DurableKvPoolLayout> pools;
    friend bool operator==(const DurableNativeLayout&, const DurableNativeLayout&) = default;
};

struct DurableEngineIdentity {
    std::uint32_t max_context = 0;
    // Resolved token capacity, never the string "auto" or a sizing headroom request.
    std::uint32_t kv_capacity = 0;
    std::string kv_dtype;
    std::uint32_t max_concurrency = 0;
    std::uint32_t prefill_chunk = 0;
    std::string speculative_backend;
    std::uint32_t draft_window = 0;
    std::string proposal_head;
    bool enable_vision = false;
    bool use_cuda_graph = false;
    friend bool operator==(const DurableEngineIdentity&, const DurableEngineIdentity&) = default;
};

struct DurableResponsesIdentity {
    std::string codec;
    friend bool operator==(const DurableResponsesIdentity&, const DurableResponsesIdentity&) = default;
};

struct DurableDeploymentIdentity {
    std::string profile;
    // Hash of the canonical identity projection ONLY; excludes admission/storage/location knobs.
    // This is not the full --config-sha256 receipt, which remains startup provenance.
    std::string effective_config_sha256;
    friend bool operator==(const DurableDeploymentIdentity&, const DurableDeploymentIdentity&) = default;
};

// Exact object comparison AFTER schema/field validation. V1, missing/extra fields and unknown
// provenance fail closed. No cross-build/platform/architecture StateImage portability is promised.
// Host/cache/Responses capacities, retention, keep-warm, UUID and physical device index are absent:
// a different admission budget may refuse import, but must not masquerade as identity mismatch.
struct RuntimeFingerprintV2 {
    DurableBuildIdentity build;
    DurableModelIdentity model;
    DurableFrontendIdentity frontend;
    DurableNativeLayout native_layout;
    DurableEngineIdentity engine;
    DurableResponsesIdentity responses;
    DurableDeploymentIdentity deployment;
    friend bool operator==(const RuntimeFingerprintV2&, const RuntimeFingerprintV2&) = default;
};

} // namespace ninfer::runtime
