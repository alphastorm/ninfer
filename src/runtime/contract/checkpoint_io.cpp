#include "runtime/contract/checkpoint_io.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <type_traits>

namespace ninfer::runtime {
namespace {

using crypto::Sha256;
using crypto::sha256;

CheckpointCompatibility mismatch(CheckpointCompatibilityError error, std::string_view field) {
    return CheckpointCompatibility{error, field};
}

template <typename Integer>
void hash_integer(Sha256& hasher, Integer value) {
    static_assert(std::is_unsigned_v<Integer>);
    std::array<std::byte, sizeof(Integer)> bytes{};
    std::uint64_t remaining = value;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[bytes.size() - 1 - index] = static_cast<std::byte>(remaining & 0xffU);
        remaining >>= 8U;
    }
    hasher.update(bytes);
}

void hash_digest(Sha256& hasher, const CheckpointDigest& digest) {
    hasher.update(std::as_bytes(std::span(digest)));
}

bool descriptors_well_formed(const std::vector<CheckpointPayloadDescriptor>& descriptors) {
    std::array<bool, 4> observed{};
    for (const CheckpointPayloadDescriptor& descriptor : descriptors) {
        const std::size_t kind = static_cast<std::size_t>(descriptor.kind);
        if (kind >= observed.size() || observed[kind] || descriptor.bytes == 0) { return false; }
        observed[kind] = true;
    }
    return !descriptors.empty();
}

bool payload_descriptors_equal(const std::vector<CheckpointPayloadDescriptor>& expected,
                               const std::vector<CheckpointPayloadDescriptor>& observed) {
    if (expected.size() != observed.size()) { return false; }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const CheckpointPayloadDescriptor& left  = expected[index];
        const CheckpointPayloadDescriptor& right = observed[index];
        if (left.kind != right.kind || left.bytes != right.bytes || left.sha256 != right.sha256) {
            return false;
        }
    }
    return true;
}

void require_manifest_shape(const CheckpointManifestV1& manifest) {
    if (manifest.magic != kCheckpointMagic || manifest.schema_version != kCheckpointSchemaVersion ||
        manifest.journal_version != kCheckpointJournalVersion || manifest.generation == 0 ||
        manifest.identity.token_count == 0 || manifest.identity.context_capacity == 0 ||
        manifest.identity.token_count > manifest.identity.context_capacity ||
        !descriptors_well_formed(manifest.payloads)) {
        throw CheckpointContractError("checkpoint manifest structure is invalid");
    }
}

void require_backend_capabilities(const CheckpointBackendCapabilities capabilities) {
    if (!capabilities.atomic_replace || !capabilities.durable_flush) {
        throw CheckpointContractError(
            "checkpoint backend must provide atomic replace and durable flush");
    }
}

void require_save_payload_shapes(const CheckpointManifestV1& manifest,
                                 std::span<const CheckpointPayloadView> payloads) {
    if (manifest.payloads.size() != payloads.size()) {
        throw CheckpointContractError("checkpoint payload count differs from the manifest");
    }
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        if (manifest.payloads[index].kind != payloads[index].kind ||
            manifest.payloads[index].bytes != payloads[index].bytes.size()) {
            throw CheckpointContractError("checkpoint payload shape differs from the manifest");
        }
    }
}

std::vector<CheckpointPayload> snapshot_payloads(const CheckpointManifestV1& manifest,
                                                 std::span<const CheckpointPayloadView> payloads) {
    std::vector<CheckpointPayload> owned;
    owned.reserve(payloads.size());
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        const CheckpointPayloadView& payload = payloads[index];
        CheckpointPayload snapshot;
        snapshot.kind = payload.kind;
        snapshot.bytes.assign(payload.bytes.begin(), payload.bytes.end());
        if (manifest.payloads[index].sha256 != sha256(snapshot.bytes)) {
            throw CheckpointContractError("checkpoint payload bytes differ from the manifest");
        }
        owned.push_back(std::move(snapshot));
    }
    return owned;
}

bool loaded_payloads_match(const std::vector<CheckpointPayloadDescriptor>& descriptors,
                           const std::vector<CheckpointPayload>& payloads) {
    if (descriptors.size() != payloads.size()) { return false; }
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
        const CheckpointPayloadDescriptor& descriptor = descriptors[index];
        const CheckpointPayload& payload              = payloads[index];
        if (descriptor.kind != payload.kind || descriptor.bytes != payload.bytes.size() ||
            descriptor.sha256 != sha256(payload.bytes)) {
            return false;
        }
    }
    return descriptors_well_formed(descriptors);
}

class ResumeGuard {
public:
    ResumeGuard(CheckpointQuiescence& quiescence, CheckpointPauseToken token)
        : quiescence_(quiescence), token_(token) {}

    ResumeGuard(const ResumeGuard&)            = delete;
    ResumeGuard& operator=(const ResumeGuard&) = delete;

    ~ResumeGuard() {
        if (active_) { quiescence_.resume_admission(token_); }
    }

    void resume() noexcept {
        if (!active_) { return; }
        quiescence_.resume_admission(token_);
        active_ = false;
    }

private:
    CheckpointQuiescence& quiescence_;
    CheckpointPauseToken token_;
    bool active_ = true;
};

} // namespace

CheckpointPauseToken::CheckpointPauseToken(std::uint64_t generation) : generation_(generation) {
    if (generation == 0) {
        throw CheckpointContractError("checkpoint pause token must be nonzero");
    }
}

CheckpointDigest checkpoint_manifest_sha256(const CheckpointManifestV1& manifest) {
    Sha256 hasher;
    hash_integer(hasher, manifest.magic);
    hash_integer(hasher, manifest.schema_version);
    hash_integer(hasher, manifest.journal_version);
    hash_integer(hasher, manifest.generation);
    hash_digest(hasher, manifest.identity.model);
    hash_digest(hasher, manifest.identity.runtime_source);
    hash_digest(hasher, manifest.identity.deployment_profile);
    hash_digest(hasher, manifest.identity.layout);
    hash_integer(hasher, manifest.identity.token_count);
    hash_integer(hasher, manifest.identity.context_capacity);
    if (manifest.payloads.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw CheckpointContractError("checkpoint payload count exceeds the canonical schema");
    }
    hash_integer(hasher, static_cast<std::uint32_t>(manifest.payloads.size()));
    for (const CheckpointPayloadDescriptor& payload : manifest.payloads) {
        hash_integer(hasher, static_cast<std::uint8_t>(payload.kind));
        hash_integer(hasher, payload.bytes);
        hash_digest(hasher, payload.sha256);
    }
    return hasher.finish();
}

CheckpointCompatibility
validate_checkpoint_compatibility(const CheckpointManifestV1& expected,
                                  const CheckpointManifestV1& observed) noexcept {
    if (observed.magic != kCheckpointMagic || expected.magic != kCheckpointMagic) {
        return mismatch(CheckpointCompatibilityError::Magic, "magic");
    }
    if (observed.schema_version != kCheckpointSchemaVersion ||
        expected.schema_version != kCheckpointSchemaVersion) {
        return mismatch(CheckpointCompatibilityError::SchemaVersion, "schema_version");
    }
    if (observed.journal_version != kCheckpointJournalVersion ||
        expected.journal_version != kCheckpointJournalVersion) {
        return mismatch(CheckpointCompatibilityError::JournalVersion, "journal_version");
    }
    if (observed.generation == 0 || observed.generation != expected.generation) {
        return mismatch(CheckpointCompatibilityError::Generation, "generation");
    }
    if (observed.identity.model != expected.identity.model) {
        return mismatch(CheckpointCompatibilityError::Model, "model");
    }
    if (observed.identity.runtime_source != expected.identity.runtime_source) {
        return mismatch(CheckpointCompatibilityError::RuntimeSource, "runtime_source");
    }
    if (observed.identity.deployment_profile != expected.identity.deployment_profile) {
        return mismatch(CheckpointCompatibilityError::DeploymentProfile, "deployment_profile");
    }
    if (observed.identity.layout != expected.identity.layout) {
        return mismatch(CheckpointCompatibilityError::Layout, "layout");
    }
    if (observed.identity.token_count == 0 ||
        observed.identity.token_count > observed.identity.context_capacity ||
        observed.identity.token_count != expected.identity.token_count) {
        return mismatch(CheckpointCompatibilityError::TokenCount, "token_count");
    }
    if (observed.identity.context_capacity != expected.identity.context_capacity) {
        return mismatch(CheckpointCompatibilityError::ContextCapacity, "context_capacity");
    }
    if (!descriptors_well_formed(expected.payloads) ||
        !descriptors_well_formed(observed.payloads) ||
        !payload_descriptors_equal(expected.payloads, observed.payloads)) {
        return mismatch(CheckpointCompatibilityError::PayloadSet, "payloads");
    }
    return {};
}

CheckpointOperationReceipt
CheckpointCoordinator::save(const CheckpointManifestV1& manifest,
                            std::span<const CheckpointPayloadView> payloads) {
    std::lock_guard operation_lock(operation_mutex_);
    require_backend_capabilities(backend_.capabilities());
    require_manifest_shape(manifest);
    require_save_payload_shapes(manifest, payloads);
    if (manifest.generation <= backend_.committed_generation()) {
        throw CheckpointContractError("checkpoint generation must advance the committed journal");
    }
    const CheckpointDigest manifest_digest = checkpoint_manifest_sha256(manifest);
    const CheckpointStageKey key{kCheckpointJournalVersion, manifest.generation, manifest_digest};

    const CheckpointPauseToken token = quiescence_.pause_admission();
    ResumeGuard resume(quiescence_, token);
    quiescence_.drain_transactions(token);
    quiescence_.fence_device(token);
    const std::vector<CheckpointPayload> owned = snapshot_payloads(manifest, payloads);
    backend_.stage(manifest, owned, key);
    try {
        backend_.commit(key);
    } catch (...) {
        backend_.abort(key);
        throw;
    }
    resume.resume();
    return {
        CheckpointOperationKind::Save, manifest.generation,
        CheckpointJournalReceipt{kCheckpointJournalVersion, manifest.generation, manifest_digest}};
}

CheckpointOperationReceipt CheckpointCoordinator::restore(const CheckpointExpectation& expected,
                                                          CheckpointRestorer& restorer) {
    std::lock_guard operation_lock(operation_mutex_);
    require_backend_capabilities(backend_.capabilities());
    require_manifest_shape(expected.manifest);
    if (checkpoint_manifest_sha256(expected.manifest) != expected.manifest_sha256) {
        throw CheckpointContractError("trusted checkpoint expectation digest is invalid");
    }

    CheckpointImage image = backend_.load(expected);
    const CheckpointCompatibility compatibility =
        validate_checkpoint_compatibility(expected.manifest, image.manifest);
    if (!compatibility.compatible()) {
        throw CheckpointContractError("checkpoint is incompatible at " +
                                      std::string(compatibility.field));
    }
    if (checkpoint_manifest_sha256(image.manifest) != expected.manifest_sha256) {
        throw CheckpointContractError("checkpoint manifest differs from the trusted journal");
    }
    if (!loaded_payloads_match(image.manifest.payloads, image.payloads)) {
        throw CheckpointContractError("checkpoint payload bytes do not match the manifest");
    }

    const CheckpointPauseToken token = quiescence_.pause_admission();
    ResumeGuard resume(quiescence_, token);
    quiescence_.drain_transactions(token);
    quiescence_.fence_device(token);
    restorer.apply(image);
    resume.resume();
    return {CheckpointOperationKind::Restore, image.manifest.generation, std::nullopt};
}

} // namespace ninfer::runtime
