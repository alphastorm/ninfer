#include "runtime/engine/resource_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ninfer::runtime::ConsumeStatus;
using ninfer::runtime::DeviceResources;
using ninfer::runtime::HostResources;
using ninfer::runtime::LaneId;
using ninfer::runtime::MaterializationPressureEffect;
using ninfer::runtime::RequestPlanSummary;
using ninfer::runtime::ResourceDelta;
using ninfer::runtime::ResourceDemand;
using ninfer::runtime::ResourceVector;

ninfer::runtime::ContextCostModel test_cost_model() {
    ninfer::runtime::ContextCostModel model;
    for (auto& transfer : model.transfer) {
        transfer.batch_ns        = 1;
        transfer.operation_ns    = 1;
        transfer.ns_per_byte_q32 = ninfer::runtime::kContextCostQ32One;
    }
    model.prefill.token_ns_q32        = 100ULL * ninfer::runtime::kContextCostQ32One;
    model.prefill.vision_item_ns      = 1;
    model.prefill.vision_patch_ns_q32 = ninfer::runtime::kContextCostQ32One;
    return model;
}

int failures = 0;

void expect(bool condition, const char* message) {
    if (condition) { return; }
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

DeviceResources converted(DeviceResources active, DeviceResources source) {
    return DeviceResources{
        .state_slots      = std::min(active.state_slots, source.state_slots),
        .main_kv_pages    = std::min(active.main_kv_pages, source.main_kv_pages),
        .backend_kv_pages = std::min(active.backend_kv_pages, source.backend_kv_pages),
    };
}

DeviceResources additional(DeviceResources active, DeviceResources source_conversion) {
    return DeviceResources{
        .active_lanes     = active.active_lanes,
        .state_slots      = active.state_slots - source_conversion.state_slots,
        .main_kv_pages    = active.main_kv_pages - source_conversion.main_kv_pages,
        .backend_kv_pages = active.backend_kv_pages - source_conversion.backend_kv_pages,
    };
}

constexpr ResourceVector resources(DeviceResources device, HostResources host = {}) noexcept {
    return ResourceVector{.device = device, .host = host};
}

ResourceDemand demand(DeviceResources active, DeviceResources source = {}) {
    const DeviceResources conversion  = converted(active, source);
    const ResourceVector entitlement  = resources(active);
    const ResourceVector source_value = resources(source);
    const ResourceVector credit       = resources(conversion);
    const ResourceVector added        = resources(additional(active, conversion));
    return ResourceDemand{
        .active_entitlement       = entitlement,
        .reservation_added        = added,
        .reservation_credit       = credit,
        .physical_peak_additional = added,
        .final_removed            = source_value,
        .final_added              = entitlement,
    };
}

struct FakePreparedPrompt {
    std::uint32_t key = 0;
    bool allow_reuse  = true;
};

struct FakeCacheSessionKey {
    std::uint32_t value                                              = 0;
    friend bool operator==(FakeCacheSessionKey, FakeCacheSessionKey) = default;

    [[nodiscard]] std::string_view view() const noexcept {
        return {reinterpret_cast<const char*>(&value), sizeof(value)};
    }
};

struct FakeShortlistKey {
    std::uint32_t digest                                       = 0;
    std::uint32_t frontier                                     = 0;
    friend bool operator==(FakeShortlistKey, FakeShortlistKey) = default;
};

struct FakeContextCache {
    std::optional<FakeCacheSessionKey> session_key;
    ninfer::runtime::RetentionClass retention = ninfer::runtime::RetentionClass::RecentPrivate;
    bool update_session_index                 = true;
};

struct FakeRequestBasePlan {
    RequestPlanSummary value;
    ResourceDemand resources;
    FakeContextCache cache;
    std::uint32_t shortlist_digest = 0;

    [[nodiscard]] const RequestPlanSummary& summary() const noexcept { return value; }

    [[nodiscard]] const ResourceDemand& root_demand() const noexcept { return resources; }

    [[nodiscard]] const FakeContextCache& context_cache() const noexcept { return cache; }

    [[nodiscard]] std::optional<FakeShortlistKey>
    prefix_shortlist_key(std::uint32_t frontier) const noexcept {
        if (frontier != 16) { return std::nullopt; }
        return FakeShortlistKey{.digest = shortlist_digest, .frontier = frontier};
    }
};

struct FakeAdmissionPlan {
    RequestPlanSummary value;
    ResourceDemand resources;
    LaneId destination;
    std::uint32_t key       = 0;
    bool source             = false;
    std::uint64_t root_work = 0;
    ResourceVector source_value;
    std::vector<ninfer::runtime::ContextTransferRequirement> transfers;
    ninfer::runtime::ClaimDisposition disposition =
        ninfer::runtime::ClaimDisposition::ConsumedToActive;

    FakeAdmissionPlan()                                        = default;
    FakeAdmissionPlan(FakeAdmissionPlan&&) noexcept            = default;
    FakeAdmissionPlan& operator=(FakeAdmissionPlan&&) noexcept = default;
    FakeAdmissionPlan(const FakeAdmissionPlan&)                = delete;
    FakeAdmissionPlan& operator=(const FakeAdmissionPlan&)     = delete;

    [[nodiscard]] const RequestPlanSummary& summary() const noexcept { return value; }

    [[nodiscard]] const ResourceDemand& demand() const noexcept { return resources; }

    [[nodiscard]] ResourceVector source_resources() const noexcept { return source_value; }

    [[nodiscard]] ninfer::runtime::ClaimDisposition source_disposition() const noexcept {
        return disposition;
    }

    [[nodiscard]] bool needs_transfer() const noexcept { return !transfers.empty(); }

    [[nodiscard]] bool temporal_eligible() const noexcept { return transfers.empty(); }

    [[nodiscard]] ninfer::runtime::PrefillWork remaining_prefill_work() const noexcept {
        return {.tokens = value.service_work_quanta};
    }

    [[nodiscard]] std::span<const ninfer::runtime::ContextTransferRequirement>
    transfer_requirements() const noexcept {
        return transfers;
    }
};

struct FakeReplicaValueImpact {
    ninfer::runtime::CheckpointRef checkpoint;
    ninfer::runtime::PrefillWork fallback_rebuild_work;
    std::vector<ninfer::runtime::ContextTransferRequirement> fallback_restore_requirements;
    std::vector<ninfer::runtime::ContextTransferRequirement> host_restore_requirements;

    friend bool operator==(const FakeReplicaValueImpact&, const FakeReplicaValueImpact&) = default;
};

enum class FakePressureKVActionKind : std::uint8_t {
    None,
    DropDeviceDuplicate,
    DemoteToHost,
    DropHostDuplicate,
};

struct FakePressureKVAction {
    std::uint32_t begin_page      = 0;
    std::uint32_t page_count      = 0;
    FakePressureKVActionKind kind = FakePressureKVActionKind::None;

    friend bool operator==(FakePressureKVAction, FakePressureKVAction) = default;
};

struct FakePressureCheckpointImpact {
    ninfer::runtime::CheckpointRef checkpoint;
    ninfer::runtime::PrefillWork fallback_rebuild_work;
    std::vector<ninfer::runtime::ContextTransferRequirement> current_restore_requirements;
    std::vector<ninfer::runtime::ContextTransferRequirement> fallback_restore_requirements;
    std::vector<ninfer::runtime::ContextTransferRequirement> added_restore_requirements;
    bool drops_checkpoint = false;

    friend bool operator==(const FakePressureCheckpointImpact&,
                           const FakePressureCheckpointImpact&) = default;
};

struct FakePressureOption {
    std::uint64_t id = 0;
    FakePressureKVAction main_kv;
    FakePressureKVAction backend_kv;
    std::optional<ninfer::runtime::CheckpointRef> dropped_checkpoint;
    ResourceDelta effect;
    std::uint64_t transfer_bytes = 0;
    std::vector<ninfer::runtime::ContextTransferRequirement> transfer_requirements;
    std::vector<FakePressureCheckpointImpact> checkpoint_impacts;
    std::vector<FakeReplicaValueImpact> removed_host_replica_impacts;
    bool evicts_continuation = false;
    bool shared_owner        = false;

    friend bool operator==(const FakePressureOption&, const FakePressureOption&) = default;
};

struct FakeSequenceHandle {
    LaneId lane;
    std::uint64_t generation = 0;
};

struct FakeContinuationHandle {
    std::uint32_t key = 0;
    mutable ResourceVector resources;
    std::uint64_t rebuild_work = 0;
    bool valid                 = false;

    FakeContinuationHandle() = default;

    FakeContinuationHandle(std::uint32_t key_, ResourceVector resources_,
                           std::uint64_t rebuild_work_)
        : key(key_), resources(resources_), rebuild_work(rebuild_work_), valid(true) {}

    FakeContinuationHandle(FakeContinuationHandle&& other) noexcept
        : key(other.key), resources(other.resources), rebuild_work(other.rebuild_work),
          valid(std::exchange(other.valid, false)) {}

    FakeContinuationHandle& operator=(FakeContinuationHandle&&)      = delete;
    FakeContinuationHandle(const FakeContinuationHandle&)            = delete;
    FakeContinuationHandle& operator=(const FakeContinuationHandle&) = delete;
};

struct FakeSharedPrefixHandle {
    FakeSharedPrefixHandle()                                             = default;
    FakeSharedPrefixHandle(FakeSharedPrefixHandle&&) noexcept            = default;
    FakeSharedPrefixHandle& operator=(FakeSharedPrefixHandle&&) noexcept = default;
    FakeSharedPrefixHandle(const FakeSharedPrefixHandle&)                = delete;
    FakeSharedPrefixHandle& operator=(const FakeSharedPrefixHandle&)     = delete;
};

struct FakeProtectedPrivateOwner {
    const FakeContinuationHandle* handle = nullptr;
    std::uint32_t owner_mask             = 0;
};

struct FakeProtectedSharedOwner {
    const FakeSharedPrefixHandle* handle = nullptr;
    std::uint32_t owner_mask             = 0;
};

struct FakeCaptureOffer {
    FakeCaptureOffer()                                       = default;
    FakeCaptureOffer(FakeCaptureOffer&&) noexcept            = default;
    FakeCaptureOffer& operator=(FakeCaptureOffer&&) noexcept = default;
    FakeCaptureOffer(const FakeCaptureOffer&)                = delete;
    FakeCaptureOffer& operator=(const FakeCaptureOffer&)     = delete;
};

struct FakeStartResult {
    FakeSequenceHandle sequence;
    ResourceVector active_resources;
};

struct FakeTargetKVRequirement {
    std::uint32_t main_pages    = 0;
    std::uint32_t backend_pages = 0;

    friend bool operator==(FakeTargetKVRequirement, FakeTargetKVRequirement) = default;
};

struct FakeCheckpointSummary {
    ninfer::runtime::CheckpointRef ref;
    ninfer::runtime::CheckpointScope scope = ninfer::runtime::CheckpointScope::Private;

    FakeShortlistKey shortlist_key;

    FakeTargetKVRequirement required_kv;
    ninfer::runtime::PrefillWork rebuild_work;

    friend bool operator==(const FakeCheckpointSummary&, const FakeCheckpointSummary&) = default;
};

struct FakeContinuationSummary {
    std::optional<FakeCheckpointSummary> endpoint;
    std::optional<FakeCheckpointSummary> rewrite;
    std::vector<FakeCheckpointSummary> long_anchors;
    std::uint32_t active_references = 0;

    friend bool operator==(const FakeContinuationSummary&,
                           const FakeContinuationSummary&) = default;
};

struct FakeRestoredContinuation {
    FakeContinuationHandle handle;
    FakeContinuationSummary summary;
    ResourceVector resources;
    ninfer::runtime::ContinuationCheckpointStats stats;
};

struct FakeSharedPrefixSummary {
    FakeCheckpointSummary checkpoint;
    std::uint32_t active_references = 0;

    friend bool operator==(const FakeSharedPrefixSummary&,
                           const FakeSharedPrefixSummary&) = default;
};

struct FakeCaptureAssessment {
    ResourceDemand demand;
    ResourceDelta active_entitlement_delta;
    ResourceVector capacity_preparation_removed;
    FakeShortlistKey shortlist_key;
    ninfer::runtime::PrefillWork protected_rebuild_work;
    std::vector<ninfer::runtime::ContextTransferRequirement> transfer_requirements;
    std::vector<FakePressureCheckpointImpact> replacement_impacts;
    std::vector<ninfer::runtime::CheckpointRef> private_replacement_candidates;
    std::uint32_t frontier = 0;
    bool publishes_private = false;
    bool publishes_shared  = false;
    bool needs_transfer    = false;
};

struct FakeSharedPrefixPublication {
    FakeSharedPrefixHandle handle;
    FakeSharedPrefixSummary summary;
};

struct FakeActiveCaptureResult {
    ninfer::runtime::ContextTransactionStatus status =
        ninfer::runtime::ContextTransactionStatus::Aborted;
    ResourceDelta resource_delta;
    ResourceDelta active_entitlement_delta;
    ResourceVector capacity_preparation_removed;
    bool capacity_preparation_committed = false;
    FakeContinuationSummary active_summary;
    std::optional<FakeSharedPrefixPublication> shared;
    std::vector<ninfer::runtime::ContextTransferObservation> transfer_observations;
    ninfer::runtime::ContextOperationCounts operations;
};

FakeContinuationSummary continuation_summary(std::uint64_t rebuild_work     = 1,
                                             std::uint32_t shortlist_digest = 0) {
    return FakeContinuationSummary{
        .endpoint =
            FakeCheckpointSummary{
                .ref = {.kind = ninfer::runtime::CheckpointKind::SessionEndpoint, .frontier = 16},
                .shortlist_key = {.digest = shortlist_digest, .frontier = 16},
                .required_kv   = {.main_pages = 2, .backend_pages = 1},
                .rebuild_work  = {.tokens = rebuild_work},
            },
    };
}

void add_resources(ResourceVector& destination, ResourceVector value) {
    destination.device.active_lanes += value.device.active_lanes;
    destination.device.state_slots += value.device.state_slots;
    destination.device.main_kv_pages += value.device.main_kv_pages;
    destination.device.backend_kv_pages += value.device.backend_kv_pages;
    destination.host.state_slots += value.host.state_slots;
    destination.host.kv_bytes += value.host.kv_bytes;
}

struct FakeMaterializationResult {
    ninfer::runtime::ContextTransactionStatus status =
        ninfer::runtime::ContextTransactionStatus::Aborted;
    std::optional<FakeStartResult> published;

    struct Source {
        ninfer::runtime::ClaimDisposition disposition = ninfer::runtime::ClaimDisposition::Retained;
        std::optional<FakeContinuationSummary> final_summary;
        ResourceDelta resource_delta;
    };

    std::optional<Source> source;

    struct SharedSource {
        ninfer::runtime::ClaimDisposition disposition = ninfer::runtime::ClaimDisposition::Retained;
        std::optional<FakeSharedPrefixSummary> final_summary;
        ResourceDelta resource_delta;
    };

    std::optional<SharedSource> shared_source;

    struct Victim {
        ninfer::runtime::ClaimDisposition disposition = ninfer::runtime::ClaimDisposition::Retained;
        std::optional<FakeContinuationSummary> final_summary;
        ResourceDelta resource_delta;
    };

    std::vector<Victim> victims;

    struct SharedVictim {
        ninfer::runtime::ClaimDisposition disposition = ninfer::runtime::ClaimDisposition::Retained;
        std::optional<FakeSharedPrefixSummary> final_summary;
        ResourceDelta resource_delta;
    };

    std::vector<SharedVictim> shared_victims;
    ResourceDelta resource_delta;
    std::vector<ninfer::runtime::ContextTransferObservation> transfer_observations;
    ninfer::runtime::ContextOperationCounts operations;
};

struct FakeReplicaTransitionOption {
    ninfer::runtime::ContextResourceClass resource = ninfer::runtime::ContextResourceClass::State;
    ninfer::runtime::CheckpointRef checkpoint;
    ResourceDelta effect;
    std::uint64_t transfer_bytes = 1;
    std::uint32_t page_count     = 0;
    ninfer::TransferWork transfer_work{.payload_bytes = 1, .copy_operations = 1};
    std::vector<FakeReplicaValueImpact> added_host_replica_impacts;
    bool shared_owner = false;

    friend bool operator==(const FakeReplicaTransitionOption&,
                           const FakeReplicaTransitionOption&) = default;
};

struct FakeReplicaTransitionResult {
    ninfer::runtime::ContextTransactionStatus status =
        ninfer::runtime::ContextTransactionStatus::Aborted;

    struct Owner {
        bool shared_owner = false;
        std::optional<FakeContinuationSummary> private_summary;
        std::optional<FakeSharedPrefixSummary> shared_summary;
        ResourceDelta resource_delta;
    };

    std::array<Owner, 2> owners;
    std::size_t owner_count = 0;
    ResourceDelta resource_delta;
    std::vector<ninfer::runtime::ContextTransferObservation> transfer_observations;
};

using FakeContextTransactionProgress =
    std::variant<ninfer::runtime::ContextTransactionInProgress, FakeMaterializationResult,
                 FakeActiveCaptureResult, FakeReplicaTransitionResult>;

struct FakeFinishResult {
    ConsumeStatus status                           = ConsumeStatus::InvariantMismatch;
    ninfer::runtime::FinishDisposition disposition = ninfer::runtime::FinishDisposition::Released;
    int timings                                    = 0;
    int speculative                                = 0;
    ResourceDelta resource_delta;
    FakeContinuationSummary summary;
    std::optional<FakeContinuationHandle> continuation;
};

struct FakeAbortResult {
    ConsumeStatus status = ConsumeStatus::InvariantMismatch;
    int timings          = 0;
    int speculative      = 0;
    ResourceDelta resource_delta;
};

struct FakeReleaseResult {
    ConsumeStatus status = ConsumeStatus::InvariantMismatch;
    ResourceDelta resource_delta;
};

struct FakeCommitRowResult {
    ninfer::runtime::CommitDisposition disposition = ninfer::runtime::CommitDisposition::Active;
    ResourceDelta resource_delta;
};

struct FakeCommitResult {
    std::array<FakeCommitRowResult, ninfer::kMaximumConcurrency> rows{};
    std::size_t row_count = 0;
};

struct FakeDiscardResult {
    ConsumeStatus status = ConsumeStatus::InvariantMismatch;
    std::array<ResourceDelta, ninfer::kMaximumConcurrency> resource_deltas{};
    std::size_t row_count = 0;
};

class FakeProgram {
public:
    std::optional<FakeAdmissionPlan> inspect_admission(
        const FakePreparedPrompt& prompt, const FakeRequestBasePlan& base, LaneId destination,
        const FakeContinuationHandle* source, const FakeSharedPrefixHandle* shared_source,
        std::optional<ninfer::runtime::CheckpointRef> checkpoint, bool must_retain_private_source) {
        if (shared_source != nullptr) {
            throw std::logic_error("fake shared-prefix admission is unsupported");
        }
        if ((source == nullptr) != !checkpoint.has_value()) {
            throw std::logic_error("fake source/checkpoint mismatch");
        }
        if (source != nullptr) {
            ++admission_source_inspections;
            last_must_retain_private_source = must_retain_private_source;
        }
        FakeAdmissionPlan plan;
        plan.value       = base.summary();
        plan.destination = destination;
        plan.key         = prompt.key;
        plan.root_work   = base.summary().service_work_quanta;
        const bool hit   = source != nullptr && source->valid && prompt.allow_reuse &&
                         source->key == prompt.key &&
                         checkpoint->kind == ninfer::runtime::CheckpointKind::SessionEndpoint &&
                         checkpoint->frontier == 16;
        if (source != nullptr && !hit) { return std::nullopt; }
        plan.source = hit;
        if (hit) {
            plan.value.reusable_prompt_tokens = 16;
            plan.value.service_work_quanta    = 1;
            plan.source_value                 = source->resources;
            if (must_retain_private_source) {
                plan.disposition = ninfer::runtime::ClaimDisposition::Retained;
                plan.resources   = base.root_demand();
            } else {
                plan.resources =
                    demand(base.root_demand().active_entitlement.device, source->resources.device);
            }
        } else {
            plan.value.reusable_prompt_tokens = 0;
            plan.resources                    = base.root_demand();
        }
        return plan;
    }

    [[nodiscard]] std::vector<FakePressureOption>
    inspect_pressure_options(const FakeContinuationHandle& continuation,
                             ResourceVector deficit) const {
        if (continuation.valid && demotable_device_state_key == continuation.key &&
            deficit.device.state_slots != 0 && continuation.resources.device.state_slots != 0 &&
            continuation.resources.host.state_slots == 0) {
            return {FakePressureOption{
                .id             = continuation.key,
                .effect         = {.removed = resources({.state_slots = 1}),
                                   .added   = resources({}, {.state_slots = 1})},
                .transfer_bytes = 1,
                .transfer_requirements =
                    {{.resource  = ninfer::runtime::ContextResourceClass::State,
                      .direction = ninfer::runtime::ContextTransferDirection::DeviceToHost,
                      .units     = 1,
                      .work      = {.payload_bytes = 1, .copy_operations = 1}}},
            }};
        }
        if (!continuation.valid || deficit.host.state_slots == 0 ||
            continuation.resources.host.state_slots == 0 ||
            continuation.resources.device.state_slots == 0) {
            return {};
        }
        const ninfer::runtime::CheckpointRef checkpoint = continuation_summary().endpoint->ref;
        return {FakePressureOption{
            .id                           = continuation.key,
            .effect                       = {.removed = resources({}, {.state_slots = 1})},
            .removed_host_replica_impacts = {FakeReplicaValueImpact{
                .checkpoint            = checkpoint,
                .fallback_rebuild_work = {.tokens = continuation.rebuild_work},
                .host_restore_requirements =
                    {{.resource  = ninfer::runtime::ContextResourceClass::State,
                      .direction = ninfer::runtime::ContextTransferDirection::HostToDevice,
                      .units     = 1,
                      .work      = {.payload_bytes = 1, .copy_operations = 1}}},
            }},
        }};
    }

    [[nodiscard]] std::vector<FakePressureOption>
    inspect_pressure_options(const FakeAdmissionPlan& admission,
                             const FakeContinuationHandle& continuation,
                             ResourceVector deficit) const {
        if (admission.source && pressure_alias_source_key == admission.key &&
            pressure_alias_owner_key == continuation.key) {
            return {};
        }
        return inspect_pressure_options(continuation, deficit);
    }

    [[nodiscard]] FakePressureOption
    inspect_eviction_option(const FakeContinuationHandle& continuation) const {
        if (!continuation.valid) { throw std::logic_error("fake eviction source is stale"); }
        const ninfer::runtime::CheckpointRef checkpoint = continuation_summary().endpoint->ref;
        return FakePressureOption{
            .id                  = continuation.key,
            .effect              = {.removed = continuation.resources},
            .checkpoint_impacts  = {FakePressureCheckpointImpact{
                 .checkpoint            = checkpoint,
                 .fallback_rebuild_work = {.tokens = continuation.rebuild_work},
                 .drops_checkpoint      = true,
            }},
            .evicts_continuation = true,
        };
    }

    [[nodiscard]] std::vector<FakePressureOption>
    inspect_shared_pressure_options(const FakeSharedPrefixHandle&, ResourceVector) const {
        return {};
    }

    [[nodiscard]] std::vector<FakePressureOption>
    inspect_shared_pressure_options(const FakeAdmissionPlan&, const FakeSharedPrefixHandle& shared,
                                    ResourceVector deficit) const {
        return inspect_shared_pressure_options(shared, deficit);
    }

    [[nodiscard]] FakePressureOption
    inspect_shared_eviction_option(const FakeSharedPrefixHandle&) const {
        const ninfer::runtime::CheckpointRef checkpoint = continuation_summary().endpoint->ref;
        return FakePressureOption{
            .id                  = std::numeric_limits<std::uint64_t>::max() - 1U,
            .checkpoint_impacts  = {FakePressureCheckpointImpact{
                 .checkpoint            = checkpoint,
                 .fallback_rebuild_work = {.tokens = 1},
                 .drops_checkpoint      = true,
            }},
            .evicts_continuation = true,
            .shared_owner        = true,
        };
    }

    [[nodiscard]] std::optional<MaterializationPressureEffect> inspect_combined_pressure_effect(
        const FakeAdmissionPlan& admission,
        std::span<const FakeContinuationHandle* const> pressure_owners,
        std::span<const FakePressureOption> pressure_options,
        std::span<const FakeSharedPrefixHandle* const> shared_pressure_owners,
        std::span<const FakePressureOption> shared_pressure_options) const {
        if (pressure_owners.size() != pressure_options.size() ||
            shared_pressure_owners.size() != shared_pressure_options.size() ||
            !shared_pressure_owners.empty()) {
            return std::nullopt;
        }
        MaterializationPressureEffect combined;
        for (std::size_t index = 0; index < pressure_options.size(); ++index) {
            if (pressure_owners[index] == nullptr || !pressure_owners[index]->valid ||
                pressure_options[index].id != pressure_owners[index]->key) {
                return std::nullopt;
            }
            if (!pressure_options[index].evicts_continuation && admission.source &&
                pressure_alias_source_key == admission.key &&
                pressure_alias_owner_key == pressure_owners[index]->key) {
                return std::nullopt;
            }
            ResourceDelta next;
            if (!ninfer::runtime::detail::add_resource_deltas(
                    combined.aggregate_delta, pressure_options[index].effect, next)) {
                return std::nullopt;
            }
            combined.aggregate_delta = next;
            if (pressure_options[index].evicts_continuation && admission.source &&
                pressure_alias_source_key == admission.key &&
                pressure_alias_owner_key == pressure_owners[index]->key) {
                combined.final_ownership_delta.removed  = pressure_alias_active_transfer;
                combined.final_ownership_delta.added    = pressure_alias_active_transfer;
                combined.active_entitlement_delta.added = pressure_alias_active_transfer;
            }
        }
        return combined;
    }

    [[nodiscard]] std::optional<FakeAdmissionPlan>
    compose_materialization(FakeAdmissionPlan&& plan,
                            std::span<const FakeContinuationHandle* const> pressure_owners,
                            std::span<const FakePressureOption> pressure_options,
                            std::span<const FakeSharedPrefixHandle* const> shared_pressure_owners,
                            std::span<const FakePressureOption> shared_pressure_options) const {
        if (pressure_owners.size() != pressure_options.size() ||
            shared_pressure_owners.size() != shared_pressure_options.size() ||
            !shared_pressure_owners.empty()) {
            throw std::logic_error("fake pressure composition is not row aligned");
        }
        last_composed_pressure.clear();
        const std::optional<MaterializationPressureEffect> combined =
            inspect_combined_pressure_effect(plan, pressure_owners, pressure_options,
                                             shared_pressure_owners, shared_pressure_options);
        if (!combined) { return std::nullopt; }
        for (std::size_t index = 0; index < pressure_options.size(); ++index) {
            if (pressure_owners[index] == nullptr || !pressure_owners[index]->valid ||
                pressure_options[index].id != pressure_owners[index]->key) {
                return std::nullopt;
            }
            last_composed_pressure.emplace_back(pressure_options[index].id,
                                                pressure_options[index].evicts_continuation);
        }
        if (!ninfer::runtime::detail::augment_demand(plan.resources, *combined)) {
            return std::nullopt;
        }
        return std::optional<FakeAdmissionPlan>(std::move(plan));
    }

    ninfer::runtime::PreflightStatus revalidate_materialization(
        const FakeAdmissionPlan& plan, const FakePreparedPrompt& prompt,
        const FakeContinuationHandle* source, const FakeSharedPrefixHandle* shared_source,
        std::span<const FakeContinuationHandle* const> victims,
        std::span<const FakeSharedPrefixHandle* const> shared_victims) const {
        if (shared_source != nullptr || !shared_victims.empty()) {
            return ninfer::runtime::PreflightStatus::InvariantFailure;
        }
        if (transaction_ || replica_transaction_) {
            return ninfer::runtime::PreflightStatus::StalePolicyState;
        }
        if (plan.key != prompt.key || plan.source != (source != nullptr)) {
            return ninfer::runtime::PreflightStatus::InvariantFailure;
        }
        if (source != nullptr && (!source->valid || source->key != plan.key)) {
            return ninfer::runtime::PreflightStatus::StalePolicyState;
        }
        for (const FakeContinuationHandle* victim : victims) {
            if (victim == nullptr || !victim->valid ||
                (source != nullptr && victim->key == source->key)) {
                return ninfer::runtime::PreflightStatus::InvariantFailure;
            }
        }
        return ninfer::runtime::PreflightStatus::Ready;
    }

    ninfer::runtime::ContextTransactionReserveStatus
    reserve_materialization(FakeAdmissionPlan&& plan, FakePreparedPrompt&& prompt,
                            const FakeContinuationHandle* source,
                            const FakeSharedPrefixHandle* shared_source,
                            std::span<const FakeContinuationHandle* const> victims,
                            std::span<const FakeSharedPrefixHandle* const> shared_victims,
                            ninfer::runtime::CancellationFlagView cancellation) {
        if (cancellation.requested()) {
            return ninfer::runtime::ContextTransactionReserveStatus::Aborted;
        }
        if (revalidate_materialization(plan, prompt, source, shared_source, victims,
                                       shared_victims) != ninfer::runtime::PreflightStatus::Ready) {
            throw std::logic_error("fake materialization reservation mismatch");
        }
        Transaction transaction;
        transaction.id                 = next_transaction_++;
        transaction.source_key         = source != nullptr ? source->key : 0;
        transaction.source_resources   = source != nullptr ? source->resources : ResourceVector{};
        transaction.has_source         = source != nullptr;
        transaction.source_disposition = plan.source_disposition();
        transaction.victim_count       = victims.size();
        transaction.plan.emplace(std::move(plan));
        transaction.prompt.emplace(std::move(prompt));
        for (std::size_t index = 0; index < victims.size(); ++index) {
            if (victims[index] == nullptr || !victims[index]->valid ||
                (source != nullptr && victims[index]->key == source->key)) {
                throw std::logic_error("fake materialization victim mismatch");
            }
            transaction.victim_keys[index]      = victims[index]->key;
            transaction.victim_resources[index] = victims[index]->resources;
        }
        transaction_.emplace(std::move(transaction));
        return ninfer::runtime::ContextTransactionReserveStatus::Reserved;
    }

    FakeMaterializationResult
    progress_materialization_transaction(ninfer::runtime::CancellationFlagView cancellation) {
        if (!transaction_ || transaction_->terminal) {
            throw std::logic_error("fake materialization progress mismatch");
        }
        FakeMaterializationResult result;
        result.victims.resize(transaction_->victim_count);
        const auto retain_unmodified_claims = [&]() {
            for (std::size_t index = 0; index < transaction_->victim_count; ++index) {
                if (transaction_->released[index]) { continue; }
                result.victims[index] = {
                    .disposition   = ninfer::runtime::ClaimDisposition::Retained,
                    .final_summary = continuation_summary(),
                };
            }
        };
        const auto retain_source = [&]() {
            if (!transaction_->has_source) { return; }
            result.source = FakeMaterializationResult::Source{
                .disposition   = ninfer::runtime::ClaimDisposition::Retained,
                .final_summary = continuation_summary(),
            };
        };
        if (cancellation.requested()) {
            retain_unmodified_claims();
            retain_source();
            transaction_->terminal = true;
            return result;
        }
        for (std::size_t index = 0; index < transaction_->victim_count; ++index) {
            transaction_->released[index] = true;
            result.victims[index]         = {
                        .disposition    = ninfer::runtime::ClaimDisposition::Evicted,
                        .resource_delta = {.removed = transaction_->victim_resources[index]},
            };
            add_resources(result.resource_delta.removed, transaction_->victim_resources[index]);
            ++release_count;
            last_released_key = transaction_->victim_keys[index];
            if (cancel_after_victim != nullptr) {
                cancel_after_victim->store(true, std::memory_order_release);
            }
            if (cancellation.requested()) {
                retain_unmodified_claims();
                retain_source();
                transaction_->terminal = true;
                return result;
            }
        }
        FakeAdmissionPlan plan(std::move(*transaction_->plan));
        FakePreparedPrompt prompt(std::move(*transaction_->prompt));
        result.status = ninfer::runtime::ContextTransactionStatus::Published;
        if (transaction_->has_source) {
            result.source = FakeMaterializationResult::Source{
                .disposition = transaction_->source_disposition,
            };
            if (transaction_->source_disposition == ninfer::runtime::ClaimDisposition::Retained) {
                result.source->final_summary = continuation_summary();
            }
        }
        result.resource_delta = {
            .removed = plan.resources.final_removed,
            .added   = plan.resources.final_added,
        };
        result.published.emplace(
            start_request(std::move(plan), std::move(prompt), transaction_->has_source));
        if (emit_state_d2h_observation) {
            result.transfer_observations.push_back(
                {.resource   = ninfer::runtime::ContextResourceClass::State,
                 .direction  = ninfer::runtime::ContextTransferDirection::DeviceToHost,
                 .units      = 1,
                 .work       = {.payload_bytes = 1, .copy_operations = 1},
                 .elapsed_ns = 1});
        }
        transaction_->terminal = true;
        return result;
    }

    void finalize_context_transaction() noexcept {
        if (transaction_ && transaction_->terminal) { transaction_.reset(); }
        if (replica_transaction_ && replica_transaction_->terminal) {
            replica_transaction_.reset();
        }
    }

    [[nodiscard]] bool has_context_transaction() const noexcept {
        return transaction_.has_value() || replica_transaction_.has_value();
    }

    [[nodiscard]] std::optional<ninfer::runtime::ContinuationCheckpointStats>
    checkpoint_continuation(const FakeContinuationHandle& continuation,
                            ninfer::runtime::ContinuationCheckpointWriter&,
                            std::size_t staging_bytes) const {
        if (!continuation.valid || staging_bytes == 0) { return std::nullopt; }
        return ninfer::runtime::ContinuationCheckpointStats{
            .frontier_tokens = 16, .restored_tokens = 16, .payload_bytes = 64};
    }

    [[nodiscard]] std::optional<FakeRestoredContinuation>
    restore_continuation(const ninfer::runtime::ContinuationCheckpointReader&,
                         std::size_t staging_bytes) const {
        if (staging_bytes == 0) { return std::nullopt; }
        const ResourceVector restored_resources =
            resources({.state_slots = 1, .main_kv_pages = 2, .backend_kv_pages = 1});
        return FakeRestoredContinuation{
            .handle = FakeContinuationHandle{99, restored_resources, 1},
            .summary = continuation_summary(),
            .resources = restored_resources,
            .stats = {.frontier_tokens = 16, .restored_tokens = 16, .payload_bytes = 64},
        };
    }

    [[nodiscard]] std::optional<FakeReplicaTransitionOption>
    inspect_replica_transition(const FakeContinuationHandle& owner,
                               ninfer::runtime::CheckpointRef checkpoint) const {
        ++replica_transition_inspections;
        if (!owner.valid || owner.resources.device.state_slots == 0 ||
            owner.resources.host.state_slots != 0 ||
            checkpoint != continuation_summary().endpoint->ref) {
            return std::nullopt;
        }
        return FakeReplicaTransitionOption{
            .checkpoint                 = checkpoint,
            .effect                     = {.added = resources({}, {.state_slots = 1})},
            .added_host_replica_impacts = {FakeReplicaValueImpact{
                .checkpoint            = checkpoint,
                .fallback_rebuild_work = {.tokens = owner.rebuild_work},
                .host_restore_requirements =
                    {{.resource  = ninfer::runtime::ContextResourceClass::State,
                      .direction = ninfer::runtime::ContextTransferDirection::HostToDevice,
                      .units     = 1,
                      .work      = {.payload_bytes = 1, .copy_operations = 1}}},
            }},
        };
    }

    [[nodiscard]] std::optional<FakeReplicaTransitionOption>
    inspect_replica_transition(const FakeSharedPrefixHandle&) const {
        ++replica_transition_inspections;
        return std::nullopt;
    }

    [[nodiscard]] ninfer::runtime::PreflightStatus
    revalidate_replica_transition(const FakeContinuationHandle* private_owner,
                                  const FakeSharedPrefixHandle* shared_owner,
                                  const FakeReplicaTransitionOption& option,
                                  const FakeContinuationHandle* private_replacement,
                                  const FakeSharedPrefixHandle* shared_replacement,
                                  const FakePressureOption* replacement) const {
        if (transaction_ || replica_transaction_) {
            return ninfer::runtime::PreflightStatus::StalePolicyState;
        }
        if (private_owner == nullptr || shared_owner != nullptr || shared_replacement != nullptr) {
            return ninfer::runtime::PreflightStatus::InvariantFailure;
        }
        const std::optional<FakeReplicaTransitionOption> expected =
            inspect_replica_transition(*private_owner, option.checkpoint);
        if (!expected || *expected != option) {
            return ninfer::runtime::PreflightStatus::StalePolicyState;
        }
        if ((private_replacement == nullptr) != (replacement == nullptr)) {
            return ninfer::runtime::PreflightStatus::InvariantFailure;
        }
        if (replacement != nullptr) {
            ++replica_replacement_revalidations;
            const std::vector<FakePressureOption> options =
                inspect_pressure_options(*private_replacement, option.effect.added);
            if (std::find(options.begin(), options.end(), *replacement) == options.end()) {
                return ninfer::runtime::PreflightStatus::StalePolicyState;
            }
        }
        return ninfer::runtime::PreflightStatus::Ready;
    }

    [[nodiscard]] ninfer::runtime::ContextTransactionReserveStatus
    reserve_prevalidated_replica_transition(const FakeContinuationHandle* private_owner,
                                            const FakeSharedPrefixHandle* shared_owner,
                                            FakeReplicaTransitionOption option,
                                            const FakeContinuationHandle* private_replacement,
                                            const FakeSharedPrefixHandle* shared_replacement,
                                            std::optional<FakePressureOption> replacement,
                                            ninfer::runtime::CancellationFlagView cancellation) {
        if (cancellation.requested()) {
            return ninfer::runtime::ContextTransactionReserveStatus::Aborted;
        }
        ReplicaTransaction transaction{
            .target             = private_owner,
            .replacement        = private_replacement,
            .option             = std::move(option),
            .replacement_option = std::move(replacement),
        };
        if (transaction.replacement != nullptr) {
            --transaction.replacement->resources.host.state_slots;
            transaction.replacement_committed = true;
        }
        replica_transaction_.emplace(std::move(transaction));
        return ninfer::runtime::ContextTransactionReserveStatus::Reserved;
    }

    [[nodiscard]] FakeReplicaTransitionResult
    progress_replica_transition_transaction(ninfer::runtime::CancellationFlagView cancellation) {
        if (!replica_transaction_ || replica_transaction_->terminal) {
            throw std::logic_error("fake replica-transition progress mismatch");
        }
        ReplicaTransaction& transaction = *replica_transaction_;
        FakeReplicaTransitionResult result;
        result.owner_count = transaction.replacement != nullptr ? 2U : 1U;
        result.owners[0]   = {
              .private_summary = continuation_summary(transaction.target->rebuild_work),
        };
        if (transaction.replacement != nullptr) {
            result.owners[1] = {
                .private_summary = continuation_summary(transaction.replacement->rebuild_work),
            };
            if (transaction.replacement_committed) {
                result.owners[1].resource_delta.removed =
                    transaction.replacement_option->effect.removed;
                result.resource_delta.removed = transaction.replacement_option->effect.removed;
            }
        }
        if (cancellation.requested()) {
            result.status        = ninfer::runtime::ContextTransactionStatus::Aborted;
            transaction.terminal = true;
            return result;
        }
        ++transaction.target->resources.host.state_slots;
        result.status = ninfer::runtime::ContextTransactionStatus::Published;
        result.owners[0].resource_delta.added = transaction.option.effect.added;
        result.resource_delta.added           = transaction.option.effect.added;
        result.transfer_observations.push_back(
            {.resource   = ninfer::runtime::ContextResourceClass::State,
             .direction  = ninfer::runtime::ContextTransferDirection::DeviceToHost,
             .units      = 1,
             .work       = transaction.option.transfer_work,
             .elapsed_ns = 1});
        last_replica_target_key = transaction.target->key;
        last_replica_victim_key =
            transaction.replacement != nullptr ? transaction.replacement->key : 0;
        transaction.terminal = true;
        return result;
    }

    [[nodiscard]] FakeContextTransactionProgress
    progress_context_transaction(ninfer::runtime::CancellationFlagView cancellation) {
        if (transaction_) {
            FakeMaterializationResult result = progress_materialization_transaction(cancellation);
            if (result.status == ninfer::runtime::ContextTransactionStatus::InProgress) {
                return ninfer::runtime::ContextTransactionInProgress{};
            }
            return result;
        }
        if (replica_transaction_) {
            FakeReplicaTransitionResult result =
                progress_replica_transition_transaction(cancellation);
            if (result.status == ninfer::runtime::ContextTransactionStatus::InProgress) {
                return ninfer::runtime::ContextTransactionInProgress{};
            }
            return result;
        }
        throw std::logic_error("fake context transaction is empty");
    }

    FakeStartResult start_request(FakeAdmissionPlan&& plan, FakePreparedPrompt&& prompt,
                                  bool has_source) {
        const std::uint32_t lane = plan.destination.value;
        if (lane >= active_.size() || active_[lane].occupied || plan.key != prompt.key ||
            plan.source != has_source) {
            throw std::logic_error("fake materialization contract mismatch");
        }
        active_[lane]          = Active{.occupied             = true,
                                        .key                  = prompt.key,
                                        .generation           = next_generation_++,
                                        .resources            = plan.resources.active_entitlement,
                                        .rebuild_work         = plan.root_work,
                                        .publish_continuation = plan.value.publish_continuation};
        last_start_lane        = lane;
        last_start_used_source = plan.source;
        return FakeStartResult{
            .sequence         = FakeSequenceHandle{LaneId{lane}, active_[lane].generation},
            .active_resources = active_[lane].resources,
        };
    }

    FakeFinishResult finish(FakeSequenceHandle sequence) noexcept {
        FakeFinishResult result;
        if (!valid(sequence)) { return result; }
        Active& active = active_[sequence.lane.value];
        result.status  = ConsumeStatus::Consumed;
        if (!active.publish_continuation) {
            result.disposition            = ninfer::runtime::FinishDisposition::Released;
            result.resource_delta.removed = active.resources;
            active                        = {};
            return result;
        }
        result.disposition            = ninfer::runtime::FinishDisposition::Catalogued;
        const bool host_only          = host_only_finish_key == active.key;
        const ResourceVector resident = resources(
            DeviceResources{
                .state_slots      = host_only ? 0U : 1U,
                .main_kv_pages    = std::min(2U, active.resources.device.main_kv_pages),
                .backend_kv_pages = std::min(1U, active.resources.device.backend_kv_pages),
            },
            HostResources{
                .state_slots = host_only ? 1U : std::min(1U, active.resources.host.state_slots),
            });
        result.resource_delta = {
            .removed = active.resources,
            .added   = resident,
        };
        result.summary =
            continuation_summary(active.rebuild_work, content_keyed_summaries ? active.key : 0U);
        result.continuation.emplace(active.key, resident, active.rebuild_work);
        active = {};
        return result;
    }

    FakeAbortResult abort(FakeSequenceHandle sequence) noexcept {
        FakeAbortResult result;
        if (!valid(sequence)) { return result; }
        Active& active                = active_[sequence.lane.value];
        result.status                 = ConsumeStatus::Consumed;
        result.resource_delta.removed = active.resources;
        active                        = {};
        return result;
    }

    FakeReleaseResult release_continuation(FakeContinuationHandle&& continuation) noexcept {
        FakeReleaseResult result;
        if (!continuation.valid) { return result; }
        result.status                 = ConsumeStatus::Consumed;
        result.resource_delta.removed = continuation.resources;
        continuation.valid            = false;
        ++release_count;
        last_released_key = continuation.key;
        return result;
    }

    [[nodiscard]] std::array<DeviceResources, 1U << ninfer::kMaximumConcurrency>
    project_protected_resources(std::span<const FakeProtectedPrivateOwner>,
                                std::span<const FakeProtectedSharedOwner>) const {
        return {};
    }

    std::uint32_t last_start_lane                           = ninfer::kMaximumConcurrency;
    bool last_start_used_source                             = false;
    std::uint32_t release_count                             = 0;
    std::uint32_t last_released_key                         = 0;
    std::uint32_t last_replica_target_key                   = 0;
    std::uint32_t last_replica_victim_key                   = 0;
    mutable std::uint32_t replica_transition_inspections    = 0;
    mutable std::uint32_t replica_replacement_revalidations = 0;
    mutable std::vector<std::pair<std::uint64_t, bool>> last_composed_pressure;
    std::optional<std::uint32_t> demotable_device_state_key;
    std::optional<std::uint32_t> host_only_finish_key;
    std::optional<std::uint32_t> pressure_alias_source_key;
    std::optional<std::uint32_t> pressure_alias_owner_key;
    ResourceVector pressure_alias_active_transfer;
    bool emit_state_d2h_observation            = false;
    bool content_keyed_summaries               = false;
    bool last_must_retain_private_source       = false;
    std::atomic<bool>* cancel_after_victim     = nullptr;
    std::uint64_t admission_source_inspections = 0;

private:
    struct Transaction {
        std::uint64_t id = 0;
        bool has_source  = false;
        ninfer::runtime::ClaimDisposition source_disposition =
            ninfer::runtime::ClaimDisposition::ConsumedToActive;
        std::uint32_t source_key = 0;
        ResourceVector source_resources;
        std::array<std::uint32_t, 2 * ninfer::kMaximumConcurrency> victim_keys{};
        std::array<ResourceVector, 2 * ninfer::kMaximumConcurrency> victim_resources{};
        std::array<bool, 2 * ninfer::kMaximumConcurrency> released{};
        std::size_t victim_count = 0;
        std::optional<FakeAdmissionPlan> plan;
        std::optional<FakePreparedPrompt> prompt;
        bool terminal = false;
    };

    struct Active {
        bool occupied            = false;
        std::uint32_t key        = 0;
        std::uint64_t generation = 0;
        ResourceVector resources;
        std::uint64_t rebuild_work = 0;
        bool publish_continuation  = true;
    };

    struct ReplicaTransaction {
        const FakeContinuationHandle* target      = nullptr;
        const FakeContinuationHandle* replacement = nullptr;
        FakeReplicaTransitionOption option;
        std::optional<FakePressureOption> replacement_option;
        bool replacement_committed = false;
        bool terminal              = false;
    };

    [[nodiscard]] bool valid(FakeSequenceHandle sequence) const noexcept {
        return sequence.lane.value < active_.size() && active_[sequence.lane.value].occupied &&
               active_[sequence.lane.value].generation == sequence.generation;
    }

    std::array<Active, ninfer::kMaximumConcurrency> active_{};
    std::uint64_t next_generation_ = 1;
    std::optional<Transaction> transaction_;
    std::optional<ReplicaTransaction> replica_transaction_;
    std::uint64_t next_transaction_ = 1;
};

struct FakePackage {
    using Program                    = FakeProgram;
    using PreparedPrompt             = FakePreparedPrompt;
    using RequestBasePlan            = FakeRequestBasePlan;
    using AdmissionPlan              = FakeAdmissionPlan;
    using SequenceHandle             = FakeSequenceHandle;
    using ContinuationHandle         = FakeContinuationHandle;
    using SharedPrefixHandle         = FakeSharedPrefixHandle;
    using CaptureOffer               = FakeCaptureOffer;
    using ProtectedPrivateOwner      = FakeProtectedPrivateOwner;
    using ProtectedSharedOwner       = FakeProtectedSharedOwner;
    using ContinuationSummary        = FakeContinuationSummary;
    using SharedPrefixSummary        = FakeSharedPrefixSummary;
    using CaptureAssessment          = FakeCaptureAssessment;
    using ActiveCaptureResult        = FakeActiveCaptureResult;
    using PressureOption             = FakePressureOption;
    using CacheSessionKey            = FakeCacheSessionKey;
    using MaterializationResult      = FakeMaterializationResult;
    using ContextTransactionProgress = FakeContextTransactionProgress;
    using ReplicaTransitionOption    = FakeReplicaTransitionOption;
    using ReplicaTransitionResult    = FakeReplicaTransitionResult;
    using StartResult                = FakeStartResult;
    using FinishResult               = FakeFinishResult;
    using AbortResult                = FakeAbortResult;
    using ReleaseResult              = FakeReleaseResult;
    using CommitResult               = FakeCommitResult;
    using DiscardResult              = FakeDiscardResult;
};

FakeRequestBasePlan make_base(std::uint64_t work = 10) {
    FakeRequestBasePlan base;
    base.resources                 = demand(DeviceResources{
                        .active_lanes = 1, .state_slots = 1, .main_kv_pages = 3, .backend_kv_pages = 2});
    base.value.service_work_quanta = work;
    return base;
}

void add_host_state_entitlement(FakeRequestBasePlan& base) {
    base.resources.active_entitlement.host.state_slots       = 1;
    base.resources.reservation_added.host.state_slots        = 1;
    base.resources.physical_peak_additional.host.state_slots = 1;
    base.resources.final_added.host.state_slots              = 1;
}

using FakeManager = ninfer::runtime::ResourceManager<FakePackage>;

FakeStartResult materialize_and_adopt(FakeManager& manager, FakeProgram& program,
                                      FakeManager::Choice&& choice, FakePreparedPrompt prompt) {
    std::atomic<bool> cancelled{false};
    const auto reserved =
        manager.reserve_materialization(program, std::move(choice), std::move(prompt),
                                        ninfer::runtime::CancellationFlagView{&cancelled});
    if (reserved != FakeManager::MaterializationReserveResult::Reserved) {
        throw std::logic_error("fake materialization was not reserved");
    }
    auto progress = manager.progress_context_transaction(
        program, ninfer::runtime::CancellationFlagView{&cancelled});
    if (!std::holds_alternative<FakeManager::MaterializationOutcome>(progress)) {
        throw std::logic_error("fake materialization returned the wrong transaction outcome");
    }
    auto outcome = std::get<FakeManager::MaterializationOutcome>(std::move(progress));
    if (outcome.status != ninfer::runtime::ContextTransactionStatus::Published ||
        !outcome.activation) {
        throw std::logic_error("fake materialization did not publish");
    }
    FakeStartResult result{
        .sequence         = outcome.activation->sequence(),
        .active_resources = outcome.activation->active_resources(),
    };
    manager.adopt(program, std::move(*outcome.activation));
    outcome.activation.reset();
    return result;
}

void test_global_catalog_lifecycle() {
    using Manager = ninfer::runtime::ResourceManager<FakePackage>;
    FakeProgram program;
    Manager manager(resources({2, 4, 12, 8}), 2, 4, 0, true, 0, test_cost_model());
    const FakeRequestBasePlan base = make_base();

    auto first = manager.inspect(program, FakePreparedPrompt{1}, base);
    expect(first.readiness == ninfer::runtime::Readiness::Ready && first.choice &&
               first.choice->destination() == LaneId{0},
           "first root was not assigned to the lowest free lane");
    auto first_active =
        materialize_and_adopt(manager, program, std::move(*first.choice), FakePreparedPrompt{1});
    (void)manager.finish(program, LaneId{0}, first_active.sequence);

    auto blocker = manager.inspect(program, FakePreparedPrompt{2}, base);
    auto blocker_active =
        materialize_and_adopt(manager, program, std::move(*blocker.choice), FakePreparedPrompt{2});
    expect(blocker_active.sequence.lane == LaneId{0}, "root blocker did not occupy lane zero");

    auto resumed = manager.inspect(program, FakePreparedPrompt{1}, base);
    expect(resumed.readiness == ninfer::runtime::Readiness::Ready && resumed.choice &&
               resumed.choice->destination() == LaneId{1} &&
               resumed.choice->summary().reusable_prompt_tokens == 16,
           "global continuation was not reusable on a different destination lane");
    auto resumed_active =
        materialize_and_adopt(manager, program, std::move(*resumed.choice), FakePreparedPrompt{1});
    expect(program.last_start_used_source && program.last_start_lane == 1,
           "materialization did not consume the global source into lane one");
    (void)manager.abort(program, LaneId{1}, resumed_active.sequence);
    (void)manager.abort(program, LaneId{0}, blocker_active.sequence);
    expect(manager.ledger().used() == ResourceVector{},
           "abort did not release active ownership and publication reservations");
}

void test_catalog_pressure_reserves_publication() {
    using Manager = ninfer::runtime::ResourceManager<FakePackage>;
    FakeProgram program;
    Manager manager(resources({1, 2, 6, 4}), 1, 2, 0, true, 0, test_cost_model());

    const auto publish_root = [&](std::uint32_t key, std::uint64_t work) {
        const FakeRequestBasePlan base = make_base(work);
        auto inspection                = manager.inspect(program, FakePreparedPrompt{key}, base);
        expect(inspection.readiness == ninfer::runtime::Readiness::Ready && inspection.choice,
               "root inspection failed under bounded catalog pressure");
        auto active = materialize_and_adopt(manager, program, std::move(*inspection.choice),
                                            FakePreparedPrompt{key});
        (void)manager.finish(program, LaneId{0}, active.sequence);
    };

    publish_root(1, 9);
    publish_root(2, 2);
    expect(manager.ledger().used() == resources({0, 2, 4, 2}),
           "two catalogued continuations did not fill the bounded state catalog");
    publish_root(3, 4);
    expect(program.release_count == 1 && program.last_released_key == 2 &&
               manager.ledger().used() == resources({0, 2, 4, 2}),
           "root materialization did not evict the cheapest whole entry and reuse its slot");
}

void test_root_only_mode_releases_finished_context() {
    FakeProgram program;
    FakeManager manager(resources({1, 1, 3, 2}), 1, 1, 0, false, 0, test_cost_model());
    FakeRequestBasePlan base        = make_base();
    base.cache.session_key          = FakeCacheSessionKey{7};
    base.value.publish_continuation = false;

    auto first = manager.inspect(program, FakePreparedPrompt{7}, base);
    expect(first.readiness == ninfer::runtime::Readiness::Ready && first.choice &&
               first.choice->summary().reusable_prompt_tokens == 0,
           "root-only mode did not admit the initial root plan");
    auto active =
        materialize_and_adopt(manager, program, std::move(*first.choice), FakePreparedPrompt{7});
    (void)manager.finish(program, LaneId{0}, active.sequence);
    expect(program.release_count == 0 && manager.ledger().used() == ResourceVector{} &&
               manager.catalog_state(0) == FakeManager::CatalogState::Vacant,
           "root-only mode did not release active state directly at finish");

    auto second = manager.inspect(program, FakePreparedPrompt{7}, base);
    expect(second.readiness == ninfer::runtime::Readiness::Ready && second.choice &&
               second.choice->summary().reusable_prompt_tokens == 0,
           "root-only mode exposed a private continuation to a later request");
    auto second_active =
        materialize_and_adopt(manager, program, std::move(*second.choice), FakePreparedPrompt{7});
    (void)manager.abort(program, LaneId{0}, second_active.sequence);
}

void test_materialization_abort_and_adoption() {
    FakeProgram program;
    FakeManager manager(resources({1, 2, 4, 2}), 1, 2, 0, true, 0, test_cost_model());

    FakeRequestBasePlan seed = make_base();
    seed.resources           = demand({1, 1, 2, 1});
    const auto publish_seed  = [&](std::uint32_t key, std::uint64_t work) {
        seed.value.service_work_quanta = work;
        auto inspection                = manager.inspect(program, FakePreparedPrompt{key}, seed);
        expect(inspection.readiness == ninfer::runtime::Readiness::Ready && inspection.choice,
                "transaction seed was not ready");
        auto active = materialize_and_adopt(manager, program, std::move(*inspection.choice),
                                             FakePreparedPrompt{key});
        (void)manager.finish(program, LaneId{0}, active.sequence);
    };
    publish_seed(1, 8);
    publish_seed(2, 2);

    std::atomic<bool> cancelled{false};
    program.cancel_after_victim       = &cancelled;
    const FakeRequestBasePlan request = make_base(9);
    auto inspection                   = manager.inspect(program, FakePreparedPrompt{1}, request);
    expect(inspection.readiness == ninfer::runtime::Readiness::Ready && inspection.choice &&
               inspection.choice->summary().reusable_prompt_tokens == 16,
           "source materialization under pressure was not selected");
    auto reserved = manager.reserve_materialization(
        program, std::move(*inspection.choice), FakePreparedPrompt{1},
        ninfer::runtime::CancellationFlagView{&cancelled});
    expect(reserved == FakeManager::MaterializationReserveResult::Reserved,
           "source materialization was not reserved");
    auto abort_progress = manager.progress_context_transaction(
        program, ninfer::runtime::CancellationFlagView{&cancelled});
    expect(std::holds_alternative<FakeManager::MaterializationOutcome>(abort_progress),
           "aborted materialization returned the wrong transaction outcome");
    auto aborted = std::get<FakeManager::MaterializationOutcome>(std::move(abort_progress));
    expect(aborted.status == ninfer::runtime::ContextTransactionStatus::Aborted &&
               !aborted.activation && program.release_count == 1 &&
               program.last_released_key == 2 && manager.ledger().used() == resources({0, 1, 2, 1}),
           "pre-publication abort did not retain committed victims and restore the source");

    program.cancel_after_victim = nullptr;
    cancelled.store(false, std::memory_order_release);
    auto retry = manager.inspect(program, FakePreparedPrompt{1}, request);
    expect(retry.readiness == ninfer::runtime::Readiness::Ready && retry.choice &&
               retry.choice->summary().reusable_prompt_tokens == 16,
           "aborted source was not reusable after its claim was restored");
    reserved =
        manager.reserve_materialization(program, std::move(*retry.choice), FakePreparedPrompt{1},
                                        ninfer::runtime::CancellationFlagView{&cancelled});
    expect(reserved == FakeManager::MaterializationReserveResult::Reserved,
           "retry materialization was not reserved");
    auto publish_progress = manager.progress_context_transaction(
        program, ninfer::runtime::CancellationFlagView{&cancelled});
    expect(std::holds_alternative<FakeManager::MaterializationOutcome>(publish_progress),
           "published materialization returned the wrong transaction outcome");
    auto published = std::get<FakeManager::MaterializationOutcome>(std::move(publish_progress));
    expect(published.status == ninfer::runtime::ContextTransactionStatus::Published &&
               published.activation && manager.ledger().used() == resources({0, 1, 2, 1}) &&
               manager.ledger().lane(LaneId{0}).state ==
                   ninfer::runtime::LogicalLaneState::Materializing,
           "Program publication changed the host ledger before activation adoption");
    const FakeSequenceHandle sequence = published.activation->sequence();
    manager.adopt(program, std::move(*published.activation));
    published.activation.reset();
    expect(manager.ledger().used() == resources({1, 1, 3, 2}),
           "published activation did not adopt the exact source-to-active delta");
    (void)manager.abort(program, LaneId{0}, sequence);
}

void test_retained_private_source_reference() {
    FakeProgram program;
    FakeManager manager(resources({2, 4, 12, 8}), 2, 4, 0, true, 0, test_cost_model());

    FakeRequestBasePlan seed = make_base();
    seed.cache.session_key   = FakeCacheSessionKey{42};
    auto initial             = manager.inspect(program, FakePreparedPrompt{42}, seed);
    expect(initial.readiness == ninfer::runtime::Readiness::Ready && initial.choice,
           "session seed was not admitted");
    auto seeded =
        materialize_and_adopt(manager, program, std::move(*initial.choice), FakePreparedPrompt{42});
    (void)manager.finish(program, LaneId{0}, seeded.sequence);

    FakeRequestBasePlan ephemeral        = make_base();
    ephemeral.cache.session_key          = FakeCacheSessionKey{42};
    ephemeral.cache.retention            = ninfer::runtime::RetentionClass::Disposable;
    ephemeral.cache.update_session_index = false;
    auto retained = manager.inspect(program, FakePreparedPrompt{42}, ephemeral);
    expect(retained.readiness == ninfer::runtime::Readiness::Ready && retained.choice &&
               retained.choice->summary().reusable_prompt_tokens == 16,
           "Disposable branch did not select the live private source");
    auto branch = materialize_and_adopt(manager, program, std::move(*retained.choice),
                                        FakePreparedPrompt{42});

    auto while_referenced = manager.inspect(program, FakePreparedPrompt{42}, seed);
    expect(while_referenced.readiness == ninfer::runtime::Readiness::Ready &&
               while_referenced.choice &&
               while_referenced.choice->summary().reusable_prompt_tokens == 0,
           "active retained source was exposed to a second private branch");

    (void)manager.abort(program, branch.sequence.lane, branch.sequence);
    auto reusable_again = manager.inspect(program, FakePreparedPrompt{42}, seed);
    expect(reusable_again.readiness == ninfer::runtime::Readiness::Ready && reusable_again.choice &&
               reusable_again.choice->summary().reusable_prompt_tokens == 16,
           "retained source did not become reusable after branch release");
}

void test_complete_eviction_closure_competes_with_mixed_closure() {
    FakeProgram program;
    program.host_only_finish_key       = 3;
    program.demotable_device_state_key = 1;
    FakeManager manager(resources({1, 2, 10, 6}, {.state_slots = 1}), 1, 3, 0, true, 0,
                        test_cost_model());

    const auto publish = [&](std::uint32_t key, ninfer::runtime::RetentionClass retention,
                             bool host_only) {
        FakeRequestBasePlan base = make_base();
        base.cache.retention     = retention;
        if (host_only) { add_host_state_entitlement(base); }
        auto inspection = manager.inspect(program, FakePreparedPrompt{key}, base);
        expect(inspection.readiness == ninfer::runtime::Readiness::Ready && inspection.choice,
               "pressure seed was not admitted");
        auto active = materialize_and_adopt(manager, program, std::move(*inspection.choice),
                                            FakePreparedPrompt{key});
        (void)manager.finish(program, LaneId{0}, active.sequence);
    };

    publish(3, ninfer::runtime::RetentionClass::LiveSession, true);
    publish(1, ninfer::runtime::RetentionClass::LiveSession, false);
    publish(2, ninfer::runtime::RetentionClass::RecentPrivate, false);

    auto inspection = manager.inspect(program, FakePreparedPrompt{4}, make_base());
    expect(inspection.readiness == ninfer::runtime::Readiness::Ready && inspection.choice,
           "root request was not closed under combined pressure");
    expect(
        program.last_composed_pressure == std::vector<std::pair<std::uint64_t, bool>>{{2, true}},
        "mixed closure displaced more valuable continuations than the complete eviction closure");
}

void test_generic_numeric_cost_prefers_disposable_eviction() {
    FakeProgram program;
    program.host_only_finish_key       = 1;
    program.demotable_device_state_key = 2;
    FakeManager manager(resources({1, 1, 8, 5}, {.state_slots = 2}), 1, 3, 0, true, 0,
                        test_cost_model());

    FakeRequestBasePlan live = make_base();
    live.cache.retention     = ninfer::runtime::RetentionClass::LiveSession;
    add_host_state_entitlement(live);
    auto live_inspection = manager.inspect(program, FakePreparedPrompt{1}, live);
    auto live_active = materialize_and_adopt(manager, program, std::move(*live_inspection.choice),
                                             FakePreparedPrompt{1});
    (void)manager.finish(program, LaneId{0}, live_active.sequence);

    FakeRequestBasePlan disposable = make_base();
    disposable.cache.retention     = ninfer::runtime::RetentionClass::Disposable;
    auto disposable_inspection     = manager.inspect(program, FakePreparedPrompt{2}, disposable);
    auto disposable_active         = materialize_and_adopt(
        manager, program, std::move(*disposable_inspection.choice), FakePreparedPrompt{2});
    (void)manager.finish(program, LaneId{0}, disposable_active.sequence);

    auto inspection = manager.inspect(program, FakePreparedPrompt{3}, make_base());
    expect(inspection.readiness == ninfer::runtime::Readiness::Ready && inspection.choice,
           "generic-cost pressure request was not admitted");
    expect(
        program.last_composed_pressure == std::vector<std::pair<std::uint64_t, bool>>{{2, true}},
        "generic numerical cost preferred transferring a Disposable checkpoint over evicting it");
}

void test_source_alias_filters_only_the_unsafe_pressure_action() {
    FakeProgram program;
    program.demotable_device_state_key     = 2;
    program.pressure_alias_source_key      = 1;
    program.pressure_alias_owner_key       = 2;
    program.pressure_alias_active_transfer = resources({.main_kv_pages = 1});
    FakeManager manager(resources({1, 2, 10, 6}), 1, 3, 0, true, 0, test_cost_model());

    FakeRequestBasePlan source = make_base();
    source.cache.session_key   = FakeCacheSessionKey{1};
    auto source_inspection     = manager.inspect(program, FakePreparedPrompt{1}, source);
    auto source_active         = materialize_and_adopt(
        manager, program, std::move(*source_inspection.choice), FakePreparedPrompt{1});
    (void)manager.finish(program, LaneId{0}, source_active.sequence);

    auto victim_inspection = manager.inspect(program, FakePreparedPrompt{2}, make_base());
    auto victim_active     = materialize_and_adopt(
        manager, program, std::move(*victim_inspection.choice), FakePreparedPrompt{2});
    (void)manager.finish(program, LaneId{0}, victim_active.sequence);

    FakeRequestBasePlan branch        = source;
    branch.cache.update_session_index = false;
    auto inspection                   = manager.inspect(program, FakePreparedPrompt{1}, branch);
    expect(inspection.readiness == ninfer::runtime::Readiness::Ready && inspection.choice &&
               inspection.choice->summary().reusable_prompt_tokens == 16 &&
               inspection.choice->active_entitlement().device.main_kv_pages == 4,
           "source alias discarded a feasible cache-hit candidate");
    expect(program.last_composed_pressure == std::vector<std::pair<std::uint64_t, bool>>{{2, true}},
           "source alias did not replace only the unsafe preserving action with exact eviction");
}

void test_value_positive_replica_replaces_lower_value_host_duplicate() {
    FakeProgram program;
    program.emit_state_d2h_observation = true;
    FakeManager manager(resources({1, 2, 6, 4}, {.state_slots = 1}), 1, 2, 0, true, 0,
                        test_cost_model());

    const auto publish = [&](std::uint32_t key, FakeRequestBasePlan base) {
        auto inspection = manager.inspect(program, FakePreparedPrompt{key}, base);
        expect(inspection.readiness == ninfer::runtime::Readiness::Ready && inspection.choice,
               "replica-transition seed was not admitted");
        auto active = materialize_and_adopt(manager, program, std::move(*inspection.choice),
                                            FakePreparedPrompt{key});
        (void)manager.finish(program, LaneId{0}, active.sequence);
    };

    FakeRequestBasePlan victim                                 = make_base(1);
    victim.resources.active_entitlement.host.state_slots       = 1;
    victim.resources.reservation_added.host.state_slots        = 1;
    victim.resources.physical_peak_additional.host.state_slots = 1;
    victim.resources.final_added.host.state_slots              = 1;
    publish(2, std::move(victim));
    publish(1, make_base(100));
    expect(manager.ledger().used() == resources({0, 2, 4, 2}, {.state_slots = 1}),
           "replica-transition seeds did not establish a full Host state pool");

    const auto reserved = manager.reserve_replica_transition(program);
    expect(reserved == FakeManager::ReplicaTransitionReserveResult::Reserved &&
               manager.has_replica_transition(),
           "value-positive Host backup replacement was not reserved");
    auto progress =
        manager.progress_context_transaction(program, ninfer::runtime::CancellationFlagView{});
    expect(std::holds_alternative<FakeManager::ReplicaTransitionOutcome>(progress),
           "replica transition returned the wrong transaction outcome");
    const auto outcome = std::get<FakeManager::ReplicaTransitionOutcome>(std::move(progress));
    expect(outcome.status == ninfer::runtime::ContextTransactionStatus::Published &&
               !manager.has_replica_transition() && !program.has_context_transaction() &&
               manager.ledger().used() == resources({0, 2, 4, 2}, {.state_slots = 1}) &&
               program.last_replica_target_key == 1 && program.last_replica_victim_key == 2 &&
               program.replica_replacement_revalidations == 1,
           "Host backup replacement did not atomically move unique occupancy");

    ninfer::RuntimeStats stats;
    manager.populate_runtime_stats(stats);
    expect(stats.private_checkpoint_degradations == 1,
           "Host backup replacement did not record the degraded private victim");
}

void test_ready_replica_transition_skips_dominated_replacements() {
    FakeProgram program;
    program.emit_state_d2h_observation = true;
    FakeManager manager(resources({1, 2, 6, 4}, {.state_slots = 2}), 1, 2, 0, true, 0,
                        test_cost_model());

    const auto publish = [&](std::uint32_t key, FakeRequestBasePlan base) {
        auto inspection = manager.inspect(program, FakePreparedPrompt{key}, base);
        expect(inspection.readiness == ninfer::runtime::Readiness::Ready && inspection.choice,
               "ready replica-transition seed was not admitted");
        auto active = materialize_and_adopt(manager, program, std::move(*inspection.choice),
                                            FakePreparedPrompt{key});
        (void)manager.finish(program, LaneId{0}, active.sequence);
    };

    FakeRequestBasePlan host_resident                                 = make_base(1);
    host_resident.resources.active_entitlement.host.state_slots       = 1;
    host_resident.resources.reservation_added.host.state_slots        = 1;
    host_resident.resources.physical_peak_additional.host.state_slots = 1;
    host_resident.resources.final_added.host.state_slots              = 1;
    publish(2, std::move(host_resident));
    publish(1, make_base(100));

    const auto reserved = manager.reserve_replica_transition(program);
    expect(reserved == FakeManager::ReplicaTransitionReserveResult::Reserved &&
               manager.has_replica_transition(),
           "ready Host backup was not reserved");
    auto progress =
        manager.progress_context_transaction(program, ninfer::runtime::CancellationFlagView{});
    expect(std::holds_alternative<FakeManager::ReplicaTransitionOutcome>(progress),
           "ready replica transition returned the wrong transaction outcome");
    const auto outcome = std::get<FakeManager::ReplicaTransitionOutcome>(std::move(progress));
    expect(outcome.status == ninfer::runtime::ContextTransactionStatus::Published &&
               program.last_replica_target_key == 1 && program.last_replica_victim_key == 0 &&
               program.replica_replacement_revalidations == 0 &&
               manager.ledger().used() == resources({0, 2, 4, 2}, {.state_slots = 2}),
           "ready Host backup evaluated or consumed a dominated replacement");
}

void test_clean_replica_policy_waits_for_resource_invalidation() {
    FakeProgram program;
    FakeManager manager(resources({1, 2, 6, 4}), 1, 2, 0, true, 0, test_cost_model());

    const auto publish = [&](std::uint32_t key) {
        auto inspection = manager.inspect(program, FakePreparedPrompt{key}, make_base());
        expect(inspection.readiness == ninfer::runtime::Readiness::Ready && inspection.choice,
               "replica-policy seed was not admitted");
        auto active = materialize_and_adopt(manager, program, std::move(*inspection.choice),
                                            FakePreparedPrompt{key});
        (void)manager.finish(program, LaneId{0}, active.sequence);
    };

    publish(1);
    expect(manager.replica_policy_pending(),
           "catalog publication did not invalidate replica policy");
    expect(manager.reserve_replica_transition(program) ==
               FakeManager::ReplicaTransitionReserveResult::Skipped,
           "zero-capacity Host policy unexpectedly reserved a transition");
    const std::uint32_t clean_inspections = program.replica_transition_inspections;
    expect(!manager.replica_policy_pending() && clean_inspections != 0,
           "replica policy did not become clean after a complete pass");
    expect(manager.reserve_replica_transition(program) ==
                   FakeManager::ReplicaTransitionReserveResult::Skipped &&
               program.replica_transition_inspections == clean_inspections,
           "clean replica policy rescanned the catalog");

    publish(2);
    expect(manager.replica_policy_pending(),
           "second catalog publication did not re-arm replica policy");
    expect(manager.reserve_replica_transition(program) ==
                   FakeManager::ReplicaTransitionReserveResult::Skipped &&
               program.replica_transition_inspections > clean_inspections,
           "resource invalidation did not trigger one new replica-policy pass");
}

void test_anonymous_content_prefix_rolls_in_place() {
    FakeProgram program;
    program.content_keyed_summaries = true;
    FakeManager manager(resources({1, 1, 3, 2}), 1, 1, 0, true, 0, test_cost_model());

    FakeRequestBasePlan request        = make_base(100);
    request.shortlist_digest           = 7;
    request.cache.update_session_index = false;
    auto seed                          = manager.inspect(program, FakePreparedPrompt{7}, request);
    expect(seed.readiness == ninfer::runtime::Readiness::Ready && seed.choice,
           "anonymous content-prefix seed was not admitted");
    auto active =
        materialize_and_adopt(manager, program, std::move(*seed.choice), FakePreparedPrompt{7});
    (void)manager.finish(program, LaneId{0}, active.sequence);

    program.admission_source_inspections    = 0;
    program.last_must_retain_private_source = true;
    auto first_append = manager.inspect(program, FakePreparedPrompt{7}, request);
    expect(first_append.readiness == ninfer::runtime::Readiness::Ready && first_append.choice &&
               first_append.choice->summary().reusable_prompt_tokens == 16 &&
               program.admission_source_inspections == 1 &&
               !program.last_must_retain_private_source,
           "anonymous append did not consume the content-matched source in the only private slot");
    active = materialize_and_adopt(manager, program, std::move(*first_append.choice),
                                   FakePreparedPrompt{7});
    (void)manager.finish(program, LaneId{0}, active.sequence);

    program.admission_source_inspections    = 0;
    program.last_must_retain_private_source = true;
    auto second_append = manager.inspect(program, FakePreparedPrompt{7}, request);
    expect(second_append.readiness == ninfer::runtime::Readiness::Ready && second_append.choice &&
               second_append.choice->summary().reusable_prompt_tokens == 16 &&
               program.admission_source_inspections == 1 &&
               !program.last_must_retain_private_source,
           "anonymous content source was not republished for the next append");
    active = materialize_and_adopt(manager, program, std::move(*second_append.choice),
                                   FakePreparedPrompt{7});
    (void)manager.abort(program, LaneId{0}, active.sequence);
}

void test_content_prefix_shortlist_bounds_exact_inspection() {
    FakeProgram program;
    program.content_keyed_summaries = true;
    FakeManager manager(resources({1, 8, 20, 12}), 1, 5, 0, true, 0, test_cost_model());

    for (std::uint32_t key = 1; key <= 4; ++key) {
        FakeRequestBasePlan base = make_base(100 + key);
        base.shortlist_digest    = key;
        auto inspection          = manager.inspect(program, FakePreparedPrompt{key}, base);
        expect(inspection.readiness == ninfer::runtime::Readiness::Ready && inspection.choice,
               "content-index seed was not admitted");
        auto active = materialize_and_adopt(manager, program, std::move(*inspection.choice),
                                            FakePreparedPrompt{key});
        (void)manager.finish(program, LaneId{0}, active.sequence);
    }

    FakeRequestBasePlan exact            = make_base(200);
    exact.shortlist_digest               = 4;
    program.admission_source_inspections = 0;
    auto hit                             = manager.inspect(program, FakePreparedPrompt{4}, exact);
    expect(hit.readiness == ninfer::runtime::Readiness::Ready && hit.choice &&
               hit.choice->summary().reusable_prompt_tokens == 16 &&
               program.admission_source_inspections == 1,
           "unrelated catalog entries reached private exact inspection");

    program.admission_source_inspections = 0;
    auto collision                       = manager.inspect(program, FakePreparedPrompt{99}, exact);
    expect(collision.readiness == ninfer::runtime::Readiness::Ready && collision.choice &&
               collision.choice->summary().reusable_prompt_tokens == 0 &&
               program.admission_source_inspections == 1,
           "shortlist digest collision bypassed exact prefix verification");

    FakeProgram session_program;
    session_program.content_keyed_summaries = true;
    FakeManager session_manager(resources({1, 4, 10, 6}), 1, 2, 0, true, 0, test_cost_model());
    FakeRequestBasePlan session_seed = make_base(100);
    session_seed.shortlist_digest    = 77;
    session_seed.cache.session_key   = FakeCacheSessionKey{11};
    auto seeded = session_manager.inspect(session_program, FakePreparedPrompt{77}, session_seed);
    auto active = materialize_and_adopt(session_manager, session_program, std::move(*seeded.choice),
                                        FakePreparedPrompt{77});
    (void)session_manager.finish(session_program, LaneId{0}, active.sequence);

    FakeRequestBasePlan other_session = make_base(200);
    other_session.shortlist_digest    = 77;
    other_session.cache.session_key   = FakeCacheSessionKey{12};
    auto retained = session_manager.inspect(session_program, FakePreparedPrompt{77}, other_session);
    expect(retained.readiness == ninfer::runtime::Readiness::Ready && retained.choice &&
               retained.choice->summary().reusable_prompt_tokens == 16 &&
               session_program.last_must_retain_private_source,
           "content hit across session identities did not retain the original continuation");

    retained.choice.reset();
    FakeRequestBasePlan anonymous                   = make_base(200);
    anonymous.shortlist_digest                      = 77;
    session_program.last_must_retain_private_source = false;
    auto unnamed = session_manager.inspect(session_program, FakePreparedPrompt{77}, anonymous);
    expect(unnamed.readiness == ninfer::runtime::Readiness::Ready && unnamed.choice &&
               unnamed.choice->summary().reusable_prompt_tokens == 16 &&
               session_program.last_must_retain_private_source,
           "anonymous branch did not retain its content-matched named source");
}

void test_session_checkpoint_tag_and_restore_ledger() {
    class Writer final : public ninfer::runtime::ContinuationCheckpointWriter {
    public:
        bool write_file(std::string_view, std::uint64_t, std::uint64_t,
                        std::span<const std::byte>) override {
            return true;
        }
    } writer;
    class Reader final : public ninfer::runtime::ContinuationCheckpointReader {
    public:
        std::optional<std::uint64_t> file_size(std::string_view) const override {
            return std::nullopt;
        }
        bool read_file(std::string_view, std::uint64_t,
                       std::span<std::byte>) const override {
            return false;
        }
    } reader;

    FakeProgram source_program;
    FakeManager source(resources({1, 4, 10, 6}), 1, 2, 0, true, 0, test_cost_model());
    FakeRequestBasePlan seed = make_base();
    seed.cache.session_key   = FakeCacheSessionKey{77};
    auto inspected = source.inspect(source_program, FakePreparedPrompt{77}, seed, "resp_1");
    auto active = materialize_and_adopt(source, source_program, std::move(*inspected.choice),
                                        FakePreparedPrompt{77});
    (void)source.finish(source_program, LaneId{0}, active.sequence);
    expect(!source.checkpoint_session(source_program, FakeCacheSessionKey{77}, "resp_wrong", writer,
                                      1024),
           "checkpoint accepted a stale response tag");
    const auto saved = source.checkpoint_session(source_program, FakeCacheSessionKey{77}, "resp_1",
                                                 writer, 1024);
    expect(saved && saved->restored_tokens == 16,
           "checkpoint did not export the exact tagged session endpoint");

    FakeProgram restored_program;
    FakeManager restored(resources({1, 4, 10, 6}), 1, 2, 0, true, 0, test_cost_model());
    const auto restored_stats = restored.restore_session_checkpoint(
        restored_program, FakeCacheSessionKey{88}, "resp_2", reader, 1024);
    expect(restored_stats && restored_stats->frontier_tokens == 16 &&
               restored.ledger().used() == resources({0, 1, 2, 1}),
           "restored continuation was not reserved as inactive catalog ownership");
    expect(restored.checkpoint_session(restored_program, FakeCacheSessionKey{88}, "resp_2", writer,
                                       1024)
               .has_value() &&
               !restored.checkpoint_session(restored_program, FakeCacheSessionKey{88}, "resp_1",
                                            writer, 1024),
           "restored session did not retain its exact response checkpoint tag");
}

} // namespace

int main() {
    test_global_catalog_lifecycle();
    test_catalog_pressure_reserves_publication();
    test_root_only_mode_releases_finished_context();
    test_materialization_abort_and_adoption();
    test_retained_private_source_reference();
    test_complete_eviction_closure_competes_with_mixed_closure();
    test_generic_numeric_cost_prefers_disposable_eviction();
    test_source_alias_filters_only_the_unsafe_pressure_action();
    test_value_positive_replica_replaces_lower_value_host_duplicate();
    test_ready_replica_transition_skips_dominated_replacements();
    test_clean_replica_policy_waits_for_resource_invalidation();
    test_anonymous_content_prefix_rolls_in_place();
    test_content_prefix_shortlist_bounds_exact_inspection();
    test_session_checkpoint_tag_and_restore_ledger();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
