#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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

using CheckpointDigest = std::array<std::byte, 32>;

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
    std::uint64_t magic           = kCheckpointMagic;
    std::uint32_t schema_version  = kCheckpointSchemaVersion;
    std::uint32_t journal_version = kCheckpointJournalVersion;
    std::uint64_t generation      = 0;
    CheckpointIdentity identity;
    std::vector<CheckpointPayloadDescriptor> payloads;
};

struct CheckpointPayloadView {
    CheckpointPayloadKind kind = CheckpointPayloadKind::StateImage;
    std::span<const std::byte> bytes;
};

struct CheckpointPayload {
    CheckpointPayloadKind kind = CheckpointPayloadKind::StateImage;
    std::vector<std::byte> bytes;
    CheckpointDigest sha256;
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
    Model,
    RuntimeSource,
    DeploymentProfile,
    Layout,
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

CheckpointCompatibility
validate_checkpoint_compatibility(const CheckpointManifestV1& expected,
                                  const CheckpointManifestV1& observed) noexcept;

enum class CheckpointPhase : std::uint8_t {
    Running,
    AdmissionPaused,
    TransactionsDrained,
    DeviceFenced,
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

struct CheckpointJournalReceipt {
    std::uint32_t journal_version = kCheckpointJournalVersion;
    std::uint64_t generation      = 0;
    CheckpointDigest manifest_sha256;
    bool committed = false;
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

    [[nodiscard]] virtual CheckpointBackendCapabilities capabilities() const noexcept = 0;
    virtual CheckpointJournalReceipt
    store_atomic(const CheckpointManifestV1& manifest,
                 std::span<const CheckpointPayloadView> payloads) = 0;
    virtual CheckpointImage load()                                = 0;
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
    std::uint64_t generation       = 0;
    CheckpointPhase terminal_phase = CheckpointPhase::Running;
    CheckpointJournalReceipt journal;
};

class CheckpointCoordinator {
public:
    CheckpointCoordinator(CheckpointQuiescence& quiescence, CheckpointBackend& backend)
        : quiescence_(quiescence), backend_(backend) {}

    CheckpointOperationReceipt save(const CheckpointManifestV1& manifest,
                                    std::span<const CheckpointPayloadView> payloads);
    CheckpointOperationReceipt restore(const CheckpointManifestV1& expected,
                                       CheckpointRestorer& restorer);

private:
    CheckpointQuiescence& quiescence_;
    CheckpointBackend& backend_;
};

} // namespace ninfer::runtime
