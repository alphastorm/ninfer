#pragma once

#include "runtime/contract/checkpoint_sha256.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ninfer::runtime {

inline constexpr std::uint64_t kCheckpointMagic          = 0x4e494e4643484b50ULL; // NINFCHKP
inline constexpr std::uint32_t kCheckpointSchemaVersion  = 1;
inline constexpr std::uint32_t kCheckpointJournalVersion = 1;

using CheckpointDigest = Sha256Digest;

enum class CheckpointPayloadKind : std::uint8_t {
    StateImage,
    MainKv,
    BackendKv,
    TailHidden,
};

struct CheckpointIdentity {
    CheckpointDigest model;
    CheckpointDigest runtime_source;
    CheckpointDigest deployment_profile;
    CheckpointDigest layout;
    std::uint32_t token_count      = 0;
    std::uint32_t context_capacity = 0;
};

struct CheckpointPayloadDescriptor {
    CheckpointPayloadKind kind = CheckpointPayloadKind::StateImage;
    std::uint64_t bytes        = 0;
    CheckpointDigest sha256;
};

struct CheckpointManifestV1 {
    std::uint64_t magic           = 0;
    std::uint32_t schema_version  = 0;
    std::uint32_t journal_version = 0;
    std::uint64_t generation      = 0;
    CheckpointIdentity identity;
    std::vector<CheckpointPayloadDescriptor> payloads;
};

struct CheckpointExpectation {
    CheckpointManifestV1 manifest;
    CheckpointDigest manifest_sha256;
};

struct CheckpointPayloadView {
    CheckpointPayloadKind kind = CheckpointPayloadKind::StateImage;
    std::span<const std::byte> bytes;
};

struct CheckpointPayload {
    CheckpointPayloadKind kind = CheckpointPayloadKind::StateImage;
    std::vector<std::byte> bytes;
};

struct CheckpointImage {
    CheckpointManifestV1 manifest;
    std::vector<CheckpointPayload> payloads;
};

enum class CheckpointCompatibilityError : std::uint8_t {
    None,
    Magic,
    SchemaVersion,
    JournalVersion,
    Generation,
    Model,
    RuntimeSource,
    DeploymentProfile,
    Layout,
    TokenCount,
    ContextCapacity,
    PayloadSet,
};

struct CheckpointCompatibility {
    CheckpointCompatibilityError error = CheckpointCompatibilityError::None;
    std::string_view field;

    [[nodiscard]] bool compatible() const noexcept {
        return error == CheckpointCompatibilityError::None;
    }
};

CheckpointDigest checkpoint_manifest_sha256(const CheckpointManifestV1& manifest);
CheckpointCompatibility
validate_checkpoint_compatibility(const CheckpointManifestV1& expected,
                                  const CheckpointManifestV1& observed) noexcept;

enum class CheckpointOperationKind : std::uint8_t {
    Save,
    Restore,
};

enum class CheckpointPhase : std::uint8_t {
    Running,
    AdmissionPaused,
    TransactionsDrained,
    DeviceFenced,
    BackendStaged,
    BackendCommitted,
    StateApplied,
    Resumed,
};

struct CheckpointPauseToken {
    std::uint64_t generation = 0;
};

struct CheckpointBackendCapabilities {
    bool atomic_replace   = false;
    bool durable_flush    = false;
    bool direct_to_device = false;
};

struct CheckpointStageReceipt {
    std::uint32_t journal_version = 0;
    std::uint64_t generation      = 0;
    CheckpointDigest manifest_sha256;
};

struct CheckpointJournalReceipt {
    std::uint32_t journal_version = 0;
    std::uint64_t generation      = 0;
    CheckpointDigest manifest_sha256;
};

class CheckpointQuiescence {
public:
    virtual ~CheckpointQuiescence() = default;

    virtual CheckpointPauseToken pause_admission()                     = 0;
    virtual void drain_transactions(CheckpointPauseToken token)        = 0;
    virtual void fence_device(CheckpointPauseToken token)              = 0;
    virtual void resume_admission(CheckpointPauseToken token) noexcept = 0;
};

class CheckpointBackend {
public:
    virtual ~CheckpointBackend() = default;

    [[nodiscard]] virtual CheckpointBackendCapabilities capabilities() const noexcept     = 0;
    virtual CheckpointStageReceipt stage(const CheckpointManifestV1& manifest,
                                         std::span<const CheckpointPayloadView> payloads) = 0;
    // commit must atomically replace the prior checkpoint and durably flush its journal. If it
    // throws, the prior committed checkpoint remains authoritative and the staged generation is
    // abortable.
    virtual void commit(const CheckpointStageReceipt& staged)         = 0;
    virtual void abort(const CheckpointStageReceipt& staged) noexcept = 0;
    virtual CheckpointImage load()                                    = 0;
};

class CheckpointRestorer {
public:
    virtual ~CheckpointRestorer()                    = default;
    virtual void apply(const CheckpointImage& image) = 0;
};

class CheckpointContractError final : public std::runtime_error {
public:
    explicit CheckpointContractError(std::string message)
        : std::runtime_error(std::move(message)) {}
};

struct CheckpointOperationReceipt {
    CheckpointOperationKind operation = CheckpointOperationKind::Save;
    std::uint64_t generation          = 0;
    CheckpointPhase terminal_phase    = CheckpointPhase::Running;
    std::optional<CheckpointJournalReceipt> journal;
};

class CheckpointCoordinator {
public:
    CheckpointCoordinator(CheckpointQuiescence& quiescence, CheckpointBackend& backend)
        : quiescence_(quiescence), backend_(backend) {}

    CheckpointOperationReceipt save(const CheckpointManifestV1& manifest,
                                    std::span<const CheckpointPayloadView> payloads);
    CheckpointOperationReceipt restore(const CheckpointExpectation& expected,
                                       CheckpointRestorer& restorer);

private:
    CheckpointQuiescence& quiescence_;
    CheckpointBackend& backend_;
    std::mutex operation_mutex_;
};

} // namespace ninfer::runtime
