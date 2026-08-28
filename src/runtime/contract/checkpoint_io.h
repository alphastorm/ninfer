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
    CheckpointDigest model{};
    CheckpointDigest runtime_source{};
    CheckpointDigest deployment_profile{};
    CheckpointDigest layout{};
    std::uint32_t token_count      = 0;
    std::uint32_t context_capacity = 0;
};

struct CheckpointPayloadDescriptor {
    CheckpointPayloadKind kind = CheckpointPayloadKind::StateImage;
    std::uint64_t bytes        = 0;
    CheckpointDigest sha256{};
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
    CheckpointDigest manifest_sha256{};
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

class CheckpointPauseToken {
public:
    explicit CheckpointPauseToken(std::uint64_t generation);

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
    std::uint64_t generation_;
};

struct CheckpointBackendCapabilities {
    bool atomic_replace   = false;
    bool durable_flush    = false;
    bool direct_to_device = false;
};

struct CheckpointStageKey {
    std::uint32_t journal_version = 0;
    std::uint64_t generation      = 0;
    CheckpointDigest manifest_sha256{};
};

struct CheckpointJournalReceipt {
    std::uint32_t journal_version = 0;
    std::uint64_t generation      = 0;
    CheckpointDigest manifest_sha256{};
};

class CheckpointQuiescence {
public:
    virtual ~CheckpointQuiescence() = default;

    // pause_admission either throws while leaving admission running, or returns one valid token for
    // a paused runtime. drain_transactions owns every writer of the supplied payload views;
    // fence_device makes those bytes immutable until resume_admission receives the same token.
    virtual CheckpointPauseToken pause_admission()                            = 0;
    virtual void drain_transactions(const CheckpointPauseToken& token)        = 0;
    virtual void fence_device(const CheckpointPauseToken& token)              = 0;
    virtual void resume_admission(const CheckpointPauseToken& token) noexcept = 0;
};

class CheckpointBackend {
public:
    virtual ~CheckpointBackend() = default;

    [[nodiscard]] virtual CheckpointBackendCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t committed_generation() const noexcept         = 0;
    // stage consumes coordinator-owned immutable payload copies. It either succeeds completely or
    // throws after removing all staging state; a throwing stage has nothing for abort to clean up.
    virtual void stage(const CheckpointManifestV1& manifest,
                       std::span<const CheckpointPayload> payloads,
                       const CheckpointStageKey& key) = 0;
    // commit atomically replaces the prior checkpoint and durably flushes its journal. If it
    // throws, the prior committed checkpoint remains authoritative and abort(key) removes staging
    // state.
    virtual void commit(const CheckpointStageKey& key)         = 0;
    virtual void abort(const CheckpointStageKey& key) noexcept = 0;
    // load must use the trusted expectation to reject descriptor sizes before allocation. Returned
    // bytes remain untrusted and are rehashed by the coordinator before runtime pause or apply.
    virtual CheckpointImage load(const CheckpointExpectation& expected) = 0;
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
    std::optional<CheckpointJournalReceipt> journal;
};

class CheckpointCoordinator {
public:
    // Every coordinator for one runtime/backend pair must share operation_mutex. Calls are
    // non-reentrant: backend/quiescence/restorer callbacks must not call this coordinator.
    CheckpointCoordinator(CheckpointQuiescence& quiescence, CheckpointBackend& backend,
                          std::mutex& operation_mutex)
        : quiescence_(quiescence), backend_(backend), operation_mutex_(operation_mutex) {}

    CheckpointOperationReceipt save(const CheckpointManifestV1& manifest,
                                    std::span<const CheckpointPayloadView> payloads);
    CheckpointOperationReceipt restore(const CheckpointExpectation& expected,
                                       CheckpointRestorer& restorer);

private:
    CheckpointQuiescence& quiescence_;
    CheckpointBackend& backend_;
    std::mutex& operation_mutex_;
};

} // namespace ninfer::runtime
