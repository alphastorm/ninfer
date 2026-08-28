#include "runtime/contract/checkpoint_io.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ninfer::runtime {
namespace {

CheckpointCompatibility mismatch(CheckpointCompatibilityError error, std::string_view field) {
    return CheckpointCompatibility{error, field};
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

bool loaded_payloads_match(const std::vector<CheckpointPayloadDescriptor>& descriptors,
                           const std::vector<CheckpointPayload>& payloads) {
    if (descriptors.size() != payloads.size()) { return false; }
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
        const CheckpointPayloadDescriptor& descriptor = descriptors[index];
        const CheckpointPayload& payload              = payloads[index];
        if (descriptor.kind != payload.kind || descriptor.bytes != payload.bytes.size() ||
            descriptor.sha256 != payload.sha256) {
            return false;
        }
    }
    return true;
}

void require_backend_capabilities(const CheckpointBackendCapabilities capabilities) {
    if (!capabilities.atomic_replace || !capabilities.durable_flush) {
        throw CheckpointContractError(
            "checkpoint backend must provide atomic replace and durable flush");
    }
}

void require_save_manifest(const CheckpointManifestV1& manifest,
                           std::span<const CheckpointPayloadView> payloads) {
    if (manifest.magic != kCheckpointMagic || manifest.schema_version != kCheckpointSchemaVersion ||
        manifest.journal_version != kCheckpointJournalVersion || manifest.generation == 0) {
        throw CheckpointContractError("checkpoint save manifest identity is invalid");
    }
    if (manifest.payloads.size() != payloads.size()) {
        throw CheckpointContractError("checkpoint payload count differs from the manifest");
    }
    std::array<bool, 4> observed{};
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        const CheckpointPayloadView& payload          = payloads[index];
        const CheckpointPayloadDescriptor& descriptor = manifest.payloads[index];
        const std::size_t kind                        = static_cast<std::size_t>(payload.kind);
        if (kind >= observed.size() || observed[kind]) {
            throw CheckpointContractError("checkpoint payload kinds must be unique and known");
        }
        observed[kind] = true;
        if (descriptor.kind != payload.kind || descriptor.bytes != payload.bytes.size()) {
            throw CheckpointContractError("checkpoint payload shape differs from the manifest");
        }
    }
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
    if (observed.identity.context_capacity > expected.identity.context_capacity) {
        return mismatch(CheckpointCompatibilityError::ContextCapacity, "context_capacity");
    }
    if (!payload_descriptors_equal(expected.payloads, observed.payloads)) {
        return mismatch(CheckpointCompatibilityError::PayloadSet, "payloads");
    }
    return {};
}

CheckpointOperationReceipt
CheckpointCoordinator::save(const CheckpointManifestV1& manifest,
                            std::span<const CheckpointPayloadView> payloads) {
    require_backend_capabilities(backend_.capabilities());
    require_save_manifest(manifest, payloads);

    CheckpointOperationReceipt operation;
    operation.generation             = manifest.generation;
    const CheckpointPauseToken token = quiescence_.pause_admission();
    operation.terminal_phase         = CheckpointPhase::AdmissionPaused;
    ResumeGuard resume(quiescence_, token);

    quiescence_.drain_transactions(token);
    operation.terminal_phase = CheckpointPhase::TransactionsDrained;
    quiescence_.fence_device(token);
    operation.terminal_phase = CheckpointPhase::DeviceFenced;
    operation.journal        = backend_.store_atomic(manifest, payloads);
    if (!operation.journal.committed ||
        operation.journal.journal_version != kCheckpointJournalVersion ||
        operation.journal.generation != manifest.generation) {
        throw CheckpointContractError("checkpoint backend returned an invalid commit receipt");
    }
    operation.terminal_phase = CheckpointPhase::BackendCommitted;
    resume.resume();
    operation.terminal_phase = CheckpointPhase::Resumed;
    return operation;
}

CheckpointOperationReceipt CheckpointCoordinator::restore(const CheckpointManifestV1& expected,
                                                          CheckpointRestorer& restorer) {
    require_backend_capabilities(backend_.capabilities());
    CheckpointImage image = backend_.load();
    const CheckpointCompatibility compatibility =
        validate_checkpoint_compatibility(expected, image.manifest);
    if (!compatibility.compatible()) {
        throw CheckpointContractError("checkpoint is incompatible at " +
                                      std::string(compatibility.field));
    }
    if (!loaded_payloads_match(image.manifest.payloads, image.payloads)) {
        throw CheckpointContractError("checkpoint payload bytes do not match the manifest");
    }

    CheckpointOperationReceipt operation;
    operation.generation              = image.manifest.generation;
    operation.journal.journal_version = image.manifest.journal_version;
    operation.journal.generation      = image.manifest.generation;
    operation.journal.committed       = true;
    const CheckpointPauseToken token  = quiescence_.pause_admission();
    operation.terminal_phase          = CheckpointPhase::AdmissionPaused;
    ResumeGuard resume(quiescence_, token);

    quiescence_.drain_transactions(token);
    operation.terminal_phase = CheckpointPhase::TransactionsDrained;
    quiescence_.fence_device(token);
    operation.terminal_phase = CheckpointPhase::DeviceFenced;
    restorer.apply(image);
    operation.terminal_phase = CheckpointPhase::StateApplied;
    resume.resume();
    operation.terminal_phase = CheckpointPhase::Resumed;
    return operation;
}

} // namespace ninfer::runtime
