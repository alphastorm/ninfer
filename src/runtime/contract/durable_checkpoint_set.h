#pragma once

#include "runtime/contract/runtime_fingerprint.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ninfer::runtime {

inline constexpr std::uint32_t kDurableSessionSchemaVersion = 3;
inline constexpr std::string_view kDurableCheckpointSetFormat = "ninfer-durable-checkpoint-set/1";
inline constexpr std::string_view kDurableOriginKeyDomain = "ninfer-checkpoint-manifest-origin-v1";
inline constexpr std::uint64_t kDurableManifestMaxBytes = 16ULL << 20;
inline constexpr std::uint32_t kDurableMaxFiles = 4096;
inline constexpr std::uint32_t kDurableMaxPoints = 64;

// These are file-local table indices, NOT runtime handles, generations, slots or CUDA rows.
using DurableLocalId = std::uint32_t;

struct DurablePayload {
    std::string path;
    std::uint64_t bytes = 0;
    std::string sha256;
    std::uint32_t alignment = 1;
};

// Wire names: input_replay, long_anchor, continuation. Unrelated shared prefixes are not exported.
enum class DurablePointRole : std::uint8_t { InputReplay, LongAnchor, Continuation };
// Master captures are semantic. Intermediate is reserved for explicitly identified native captures.
enum class DurablePointOrigin : std::uint8_t { Semantic, Intermediate };

struct DurableTokenSpan {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;
};

// Exact identity metadata, not a retained media buffer. timestamps_bits encodes each finite
// IEEE-754 binary64 timestamp as 16 lowercase hexadecimal digits (most significant digit first).
// This avoids lossy JSON float conversion; reconstruction uses bit_cast, never numeric conversion.
struct DurableVisionIdentity {
    std::string modality; // image or video
    std::int32_t temporal = 0;
    std::int32_t height = 0;
    std::int32_t width = 0;
    std::uint64_t patch_begin = 0;
    std::uint64_t patch_count = 0;
    std::string content_sha256;
    std::vector<std::string> timestamps_bits;
    std::vector<DurableTokenSpan> token_spans;
};

struct DurableIdentity {
    std::vector<std::int32_t> ledger;
    std::vector<std::uint8_t> token_types;
    std::array<std::vector<std::int32_t>, 3> positions;
    std::vector<std::uint32_t> execution_frontiers;
    std::vector<DurableVisionIdentity> vision_items;
    // Derived accelerator only; importer MUST reconstruct and compare, never trust it as identity.
    std::array<std::uint64_t, 2> shortlist_tag{};
};

struct DurablePage {
    DurableLocalId payload = 0;
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
    // Maximum committed columns needed by any retained prefix referring to this logical page.
    std::uint32_t committed_tokens = 0;
};

struct DurablePool {
    DurableKvPoolLayout layout;
    std::vector<DurablePage> pages;
};

struct DurableHistory {
    DurableLocalId pool = 0;
    std::vector<DurableLocalId> pages; // logical order, indices into that pool's page inventory
    std::uint32_t committed_tokens = 0;
};

struct DurableState {
    std::string layout_sha256;
    DurableLocalId payload = 0;
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
};

struct DurableHistoryPrefix {
    DurableLocalId history = 0;
    std::uint32_t frontier = 0;
};

struct DurablePoint {
    DurablePointRole role = DurablePointRole::Continuation;
    DurablePointOrigin origin = DurablePointOrigin::Semantic;
    // execution_frontier, text_kv_valid and backend_kv_valid in native ContinuationMetadata.
    std::uint32_t frontier = 0;
    std::uint32_t text_kv_frontier = 0;
    std::uint32_t backend_frontier = 0;
    std::int32_t rope_delta = 0;
    bool tail_hidden_valid = false;
    std::uint32_t dflash_context_frontier = 0;
    DurableLocalId identity = 0;
    DurableLocalId state = 0;
    std::vector<DurableHistoryPrefix> histories; // ascending pool ID; no DFlash2 backend-KV fiction
};

// engine/index.cbor is the field-by-field encoding of this document plus format=the constant above.
// Tables use dense array-position IDs; payloads sorted by path. Dedup by immutable native content
// owner while exporting, not by persisted runtime identifiers. No raw C++ struct serialization.
struct DurableCheckpointSet {
    std::string architecture;
    std::string native_codec;
    std::string state_layout_sha256;
    DurableLocalId endpoint = 0;
    std::vector<DurablePoint> points;
    std::vector<DurableIdentity> identities;
    std::vector<DurableHistory> histories;
    std::vector<DurableState> states;
    std::vector<DurablePool> pools;
    // Only engine/state/* and engine/kv/*; index.cbor cannot recursively hash itself.
    std::vector<DurablePayload> payloads;
};

struct DurableCheckpointStats {
    std::uint32_t frontier_tokens = 0;
    std::uint32_t restored_tokens = 0;
    std::uint64_t payload_bytes = 0;
    std::uint32_t points = 0;
    std::uint32_t states = 0;
    std::uint32_t pages = 0;
};

// Wire keys are these member names plus artifact_type="ninfer_session_checkpoint",
// schema_version=3. All fields are mandatory; manifest.mac is not part of files.
struct DurableSessionManifest {
    std::string session_sha256;
    std::string generation;
    std::uint64_t created_at_unix_ms = 0;
    RuntimeFingerprintV2 runtime_fingerprint;
    std::string latest_response_id;
    std::uint64_t response_records = 0;
    std::uint32_t frontier_tokens = 0;
    std::uint32_t restored_tokens = 0;
    std::uint64_t engine_payload_bytes = 0;
    std::uint64_t payload_bytes = 0;
    std::vector<DurablePayload> files;
};

} // namespace ninfer::runtime
