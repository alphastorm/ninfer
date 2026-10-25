#include "runtime/contract/durable_checkpoint_set.h"
#include "runtime/contract/durable_engine_api.h"
#include "runtime/contract/runtime_fingerprint.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

using namespace ninfer::runtime;

static_assert(kDurableSessionSchemaVersion == 3);
static_assert(kRuntimeFingerprintVersion == 2);
static_assert(kDurableCheckpointSetFormat == "ninfer-durable-checkpoint-set/1");
static_assert(!std::is_copy_constructible_v<DurableEngineImport>);
static_assert(std::is_nothrow_move_constructible_v<DurableEngineImport>);
static_assert(std::is_nothrow_move_assignable_v<DurableEngineImport>);
static_assert(std::is_same_v<DurableLocalId, std::uint32_t>);
static_assert(std::is_same_v<decltype(&CheckpointEngineAccess::checkpoint_layout),
                             DurableNativeLayout (*)(const ninfer::Engine&)>);
static_assert(std::is_same_v<decltype(&CheckpointEngineAccess::frontend_identity),
                             DurableFrontendIdentity (*)(const ninfer::Engine&)>);
static_assert(std::is_same_v<decltype(&CheckpointEngineAccess::set_pressure_checkpoint_handler),
                             void (*)(ninfer::Engine&, std::shared_ptr<PressureCheckpointHandler>)>);
static_assert(std::is_same_v<decltype(&CheckpointEngineAccess::set_reclaimable_session_oracle),
                             void (*)(ninfer::Engine&, std::shared_ptr<const ReclaimableSessionOracle>)>);
static_assert(std::is_same_v<decltype(&CheckpointEngineAccess::notify_checkpoint_progress),
                             void (*)(ninfer::Engine&) noexcept>);
static_assert(std::is_same_v<decltype(&CheckpointEngineAccess::make_read_queue),
                             std::shared_ptr<ContinuationCheckpointReadQueue> (*)(
                                 ninfer::Engine&, const std::filesystem::path&)>);

static_assert(durable_reason_name(DurableReason::None) == "none");
static_assert(durable_reason_name(DurableReason::StoreDisabled) == "store_disabled");
static_assert(durable_reason_name(DurableReason::CacheDisabled) == "cache_disabled");
static_assert(durable_reason_name(DurableReason::NoSessionRecords) == "no_session_records");
static_assert(durable_reason_name(DurableReason::EmptyTag) == "empty_tag");
static_assert(durable_reason_name(DurableReason::SessionNotIndexed) == "session_not_indexed");
static_assert(durable_reason_name(DurableReason::TransactionBusy) == "transaction_busy");
static_assert(durable_reason_name(DurableReason::TagMismatch) == "tag_mismatch");
static_assert(durable_reason_name(DurableReason::StalePublication) == "stale_publication");
static_assert(durable_reason_name(DurableReason::StaleRevision) == "stale_revision");
static_assert(durable_reason_name(DurableReason::UnsupportedCodec) == "unsupported_codec");
static_assert(durable_reason_name(DurableReason::IncompatibleSchema) == "incompatible_schema");
static_assert(durable_reason_name(DurableReason::FingerprintMismatch) == "fingerprint_mismatch");
static_assert(durable_reason_name(DurableReason::OriginRequired) == "origin_required");
static_assert(durable_reason_name(DurableReason::OriginKeyUnavailable) == "origin_key_unavailable");
static_assert(durable_reason_name(DurableReason::OriginMacMismatch) == "origin_mac_mismatch");
static_assert(durable_reason_name(DurableReason::InvalidManifest) == "invalid_manifest");
static_assert(durable_reason_name(DurableReason::InvalidIndex) == "invalid_index");
static_assert(durable_reason_name(DurableReason::PayloadSizeMismatch) == "payload_size_mismatch");
static_assert(durable_reason_name(DurableReason::PayloadDigestMismatch) == "payload_digest_mismatch");
static_assert(durable_reason_name(DurableReason::MetadataLimit) == "metadata_limit");
static_assert(durable_reason_name(DurableReason::StagingLimit) == "staging_limit");
static_assert(durable_reason_name(DurableReason::Capacity) == "capacity");
static_assert(durable_reason_name(DurableReason::Quota) == "quota");
static_assert(durable_reason_name(DurableReason::ForeignSessionCollision) == "foreign_session_collision");
static_assert(durable_reason_name(DurableReason::RequiredResponseMissing) == "required_response_missing");
static_assert(durable_reason_name(DurableReason::IoUnavailable) == "io_unavailable");
static_assert(durable_reason_name(DurableReason::DurabilityUnsupported) == "durability_unsupported");
static_assert(durable_reason_name(DurableReason::Cancelled) == "cancelled");
static_assert(durable_reason_name(static_cast<DurableReason>(255)) == "unknown");
static_assert(noexcept(durable_reason_name(DurableReason::StalePublication)));

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected durable fixture directory");
        for (const char* name : {"manifest-text.json", "manifest-dflash2.json", "ack-posix.json", "ack-windows.json"}) {
            std::ifstream input(std::string(argv[1]) + "/" + name, std::ios::binary);
            if (!input) throw std::runtime_error("cannot open canonical fixture");
            const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            const auto value = nlohmann::json::parse(bytes);
            if (value.dump() != bytes) throw std::runtime_error("production JSON encoder differs from canonical fixture");
        }
        const nlohmann::json value = {
            {"z", {true, false, nullptr, std::numeric_limits<std::int64_t>::min(),
                    std::numeric_limits<std::uint64_t>::max()}},
            {"a", std::string("\xc3\xa9/\"\\\b\t\n\f\r\0\x1f", 12)}
        };
        const std::string expected = "{\"a\":\"\xc3\xa9/\\\"\\\\\\b\\t\\n\\f\\r\\u0000\\u001f\",\"z\":[true,false,null,-9223372036854775808,18446744073709551615]}";
        if (value.dump() != expected) throw std::runtime_error("production JSON encoder differs on Unicode/control/integer vector");
        std::cout << "durable headers and 5 canonical encoder vectors passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
