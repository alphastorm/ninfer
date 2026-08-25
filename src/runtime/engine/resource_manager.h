#pragma once

#include "ninfer/types.h"
#include "runtime/contract/types.h"
#include "runtime/engine/admission_policy.h"
#include "runtime/engine/context_cost.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ninfer::runtime {

inline constexpr std::uint32_t kInvalidCatalogSlot = std::numeric_limits<std::uint32_t>::max();

enum class LogicalLaneState : std::uint8_t {
    Free,
    Materializing,
    Active,
};

struct LogicalLaneSnapshot {
    LogicalLaneState state = LogicalLaneState::Free;
    ResourceVector resources;
};

[[nodiscard]] constexpr bool resources_fit(ResourceVector value, ResourceVector capacity) noexcept {
    return value.device.active_lanes <= capacity.device.active_lanes &&
           value.device.state_slots <= capacity.device.state_slots &&
           value.device.main_kv_pages <= capacity.device.main_kv_pages &&
           value.device.backend_kv_pages <= capacity.device.backend_kv_pages &&
           value.host.state_slots <= capacity.host.state_slots &&
           value.host.kv_bytes <= capacity.host.kv_bytes;
}

[[nodiscard]] constexpr bool continuation_within_active(ResourceVector continuation,
                                                        ResourceVector active) noexcept {
    return continuation.device.active_lanes == 0 &&
           continuation.device.state_slots <= active.device.state_slots &&
           continuation.device.main_kv_pages <= active.device.main_kv_pages &&
           continuation.device.backend_kv_pages <= active.device.backend_kv_pages &&
           continuation.host.state_slots <= active.host.state_slots &&
           continuation.host.kv_bytes <= active.host.kv_bytes;
}

namespace detail {

[[nodiscard]] constexpr bool add_resources(ResourceVector left, ResourceVector right,
                                           ResourceVector& out) noexcept {
    const std::uint64_t active =
        static_cast<std::uint64_t>(left.device.active_lanes) + right.device.active_lanes;
    const std::uint64_t state =
        static_cast<std::uint64_t>(left.device.state_slots) + right.device.state_slots;
    const std::uint64_t main =
        static_cast<std::uint64_t>(left.device.main_kv_pages) + right.device.main_kv_pages;
    const std::uint64_t backend =
        static_cast<std::uint64_t>(left.device.backend_kv_pages) + right.device.backend_kv_pages;
    const std::uint64_t host_state =
        static_cast<std::uint64_t>(left.host.state_slots) + right.host.state_slots;
    if (active > std::numeric_limits<std::uint32_t>::max() ||
        state > std::numeric_limits<std::uint32_t>::max() ||
        main > std::numeric_limits<std::uint32_t>::max() ||
        backend > std::numeric_limits<std::uint32_t>::max() ||
        host_state > std::numeric_limits<std::uint32_t>::max() ||
        right.host.kv_bytes > std::numeric_limits<std::size_t>::max() - left.host.kv_bytes) {
        return false;
    }
    out = ResourceVector{
        .device =
            {
                .active_lanes     = static_cast<std::uint32_t>(active),
                .state_slots      = static_cast<std::uint32_t>(state),
                .main_kv_pages    = static_cast<std::uint32_t>(main),
                .backend_kv_pages = static_cast<std::uint32_t>(backend),
            },
        .host =
            {
                .state_slots = static_cast<std::uint32_t>(host_state),
                .kv_bytes    = left.host.kv_bytes + right.host.kv_bytes,
            },
    };
    return true;
}

[[nodiscard]] constexpr bool subtract_resources(ResourceVector value, ResourceVector removed,
                                                ResourceVector& out) noexcept {
    if (removed.device.active_lanes > value.device.active_lanes ||
        removed.device.state_slots > value.device.state_slots ||
        removed.device.main_kv_pages > value.device.main_kv_pages ||
        removed.device.backend_kv_pages > value.device.backend_kv_pages ||
        removed.host.state_slots > value.host.state_slots ||
        removed.host.kv_bytes > value.host.kv_bytes) {
        return false;
    }
    out = ResourceVector{
        .device =
            {
                .active_lanes     = value.device.active_lanes - removed.device.active_lanes,
                .state_slots      = value.device.state_slots - removed.device.state_slots,
                .main_kv_pages    = value.device.main_kv_pages - removed.device.main_kv_pages,
                .backend_kv_pages = value.device.backend_kv_pages - removed.device.backend_kv_pages,
            },
        .host =
            {
                .state_slots = value.host.state_slots - removed.host.state_slots,
                .kv_bytes    = value.host.kv_bytes - removed.host.kv_bytes,
            },
    };
    return true;
}

[[nodiscard]] constexpr bool transition_fits(ResourceVector used, ResourceVector removed,
                                             ResourceVector added,
                                             ResourceVector capacity) noexcept {
    ResourceVector remainder;
    if (!subtract_resources(used, removed, remainder)) { return false; }
    ResourceVector total;
    return add_resources(remainder, added, total) && resources_fit(total, capacity);
}

[[nodiscard]] constexpr ResourceVector
positive_resource_difference(ResourceVector value, ResourceVector removed) noexcept {
    const auto positive_u32 = [](std::uint32_t left, std::uint32_t right) {
        return left > right ? left - right : 0U;
    };
    return ResourceVector{
        .device =
            {
                .active_lanes =
                    positive_u32(value.device.active_lanes, removed.device.active_lanes),
                .state_slots = positive_u32(value.device.state_slots, removed.device.state_slots),
                .main_kv_pages =
                    positive_u32(value.device.main_kv_pages, removed.device.main_kv_pages),
                .backend_kv_pages =
                    positive_u32(value.device.backend_kv_pages, removed.device.backend_kv_pages),
            },
        .host =
            {
                .state_slots = positive_u32(value.host.state_slots, removed.host.state_slots),
                .kv_bytes    = value.host.kv_bytes > removed.host.kv_bytes
                                   ? value.host.kv_bytes - removed.host.kv_bytes
                                   : 0U,
            },
    };
}

[[nodiscard]] constexpr bool valid_demand(const ResourceDemand& demand, ResourceVector source,
                                          ClaimDisposition source_disposition) noexcept {
    if (demand.active_entitlement.device.active_lanes != 1 || source.device.active_lanes != 0) {
        return false;
    }
    ResourceVector unused;
    if (!subtract_resources(demand.final_added, demand.active_entitlement, unused)) {
        return false;
    }
    if (source_disposition == ClaimDisposition::Retained) { return true; }
    return source_disposition == ClaimDisposition::ConsumedToActive &&
           subtract_resources(demand.final_removed, source, unused);
}

[[nodiscard]] inline bool augment_demand(ResourceDemand& demand,
                                         const ResourceDelta& effect) noexcept {
    ResourceVector next;
    if (!add_resources(demand.reservation_credit, effect.removed, next)) { return false; }
    demand.reservation_credit = next;
    if (!add_resources(demand.reservation_added, effect.added, next)) { return false; }
    demand.reservation_added = next;
    if (!add_resources(demand.physical_peak_additional, effect.added, next)) { return false; }
    demand.physical_peak_additional = positive_resource_difference(next, effect.removed);
    if (!add_resources(demand.final_removed, effect.removed, next)) { return false; }
    demand.final_removed = next;
    if (!add_resources(demand.final_added, effect.added, next)) { return false; }
    demand.final_added = next;
    return true;
}

[[nodiscard]] inline bool augment_demand(ResourceDemand& demand,
                                         const MaterializationPressureEffect& effect) noexcept {
    if (effect.final_ownership_delta.removed != effect.final_ownership_delta.added ||
        effect.final_ownership_delta.removed.device.active_lanes != 0 ||
        effect.active_entitlement_delta.removed.device.active_lanes != 0 ||
        effect.active_entitlement_delta.added.device.active_lanes != 0) {
        return false;
    }
    ResourceDemand next_demand = demand;
    if (!augment_demand(next_demand, effect.aggregate_delta)) { return false; }
    ResourceVector next;
    if (!add_resources(next_demand.final_removed, effect.final_ownership_delta.removed, next)) {
        return false;
    }
    next_demand.final_removed = next;
    if (!add_resources(next_demand.final_added, effect.final_ownership_delta.added, next)) {
        return false;
    }
    next_demand.final_added = next;
    ResourceVector remaining;
    ResourceVector active;
    if (!subtract_resources(next_demand.active_entitlement, effect.active_entitlement_delta.removed,
                            remaining) ||
        !add_resources(remaining, effect.active_entitlement_delta.added, active) ||
        active.device.active_lanes != 1) {
        return false;
    }
    next_demand.active_entitlement = active;
    demand                         = next_demand;
    return true;
}

[[nodiscard]] constexpr bool resource_delta_within(const ResourceDelta& value,
                                                   const ResourceDelta& limit) noexcept {
    return resources_fit(value.removed, limit.removed) && resources_fit(value.added, limit.added);
}

[[nodiscard]] constexpr bool add_resource_deltas(ResourceDelta left, ResourceDelta right,
                                                 ResourceDelta& out) noexcept {
    return add_resources(left.removed, right.removed, out.removed) &&
           add_resources(left.added, right.added, out.added);
}

[[nodiscard]] inline bool demand_fits(ResourceVector used, const ResourceDemand& demand,
                                      ResourceVector capacity) noexcept {
    return transition_fits(used, demand.reservation_credit, demand.reservation_added, capacity) &&
           transition_fits(used, demand.final_removed, demand.final_added, capacity);
}

} // namespace detail

class ResourceLedger {
public:
    ResourceLedger(ResourceVector capacity, std::uint32_t lane_count)
        : capacity_(capacity), lane_count_(lane_count) {
        if (lane_count == 0 || lane_count > kMaximumConcurrency ||
            capacity.device.active_lanes != lane_count) {
            throw std::invalid_argument("resource ledger capacity is invalid");
        }
    }

    [[nodiscard]] ResourceVector capacity() const noexcept { return capacity_; }

    [[nodiscard]] ResourceVector used() const noexcept { return used_; }

    [[nodiscard]] const LogicalLaneSnapshot& lane(LaneId id) const noexcept {
        static const LogicalLaneSnapshot invalid;
        return id.value < lane_count_ ? lanes_[id.value] : invalid;
    }

    [[nodiscard]] bool complete_materialization(LaneId lane, const ResourceDelta& delta) noexcept {
        if (lane.value >= lane_count_ ||
            lanes_[lane.value].state != LogicalLaneState::Materializing || !transaction_lane_ ||
            *transaction_lane_ != lane ||
            transaction_kind_ != ContextTransactionKind::Materialization ||
            lanes_[lane.value].resources.device.active_lanes != 1 ||
            delta.added.device.active_lanes != 1 || delta.removed.device.active_lanes != 0) {
            return false;
        }
        ResourceVector next;
        if (!apply_delta(used_, delta, next)) { return false; }
        lanes_[lane.value].state = LogicalLaneState::Active;
        used_                    = next;
        transaction_lane_.reset();
        transaction_kind_.reset();
        return true;
    }

    [[nodiscard]] bool abort_materialization(LaneId lane, const ResourceDelta& delta) noexcept {
        if (lane.value >= lane_count_ ||
            lanes_[lane.value].state != LogicalLaneState::Materializing || !transaction_lane_ ||
            *transaction_lane_ != lane ||
            transaction_kind_ != ContextTransactionKind::Materialization ||
            delta.added.device.active_lanes != 0 || delta.removed.device.active_lanes != 0) {
            return false;
        }
        ResourceVector next;
        if (!apply_delta(used_, delta, next)) { return false; }
        lanes_[lane.value] = {};
        used_              = next;
        transaction_lane_.reset();
        transaction_kind_.reset();
        return true;
    }

    [[nodiscard]] bool can_reserve_materialization(LaneId lane,
                                                   const ResourceDemand& demand) const noexcept {
        if (lane.value >= lane_count_ || lanes_[lane.value].state != LogicalLaneState::Free ||
            demand.active_entitlement.device.active_lanes != 1 || transaction_kind_) {
            return false;
        }
        return detail::transition_fits(used_, demand.reservation_credit, demand.reservation_added,
                                       capacity_) &&
               detail::transition_fits(used_, demand.final_removed, demand.final_added, capacity_);
    }

    [[nodiscard]] bool reserve_materialization(LaneId lane, const ResourceDemand& demand) noexcept {
        if (!can_reserve_materialization(lane, demand)) { return false; }
        lanes_[lane.value] = {LogicalLaneState::Materializing, demand.active_entitlement};
        transaction_lane_  = lane;
        transaction_kind_  = ContextTransactionKind::Materialization;
        return true;
    }

    [[nodiscard]] bool cancel_materialization(LaneId lane) noexcept {
        if (lane.value >= lane_count_ ||
            lanes_[lane.value].state != LogicalLaneState::Materializing || !transaction_lane_ ||
            *transaction_lane_ != lane ||
            transaction_kind_ != ContextTransactionKind::Materialization) {
            return false;
        }
        lanes_[lane.value] = {};
        transaction_lane_.reset();
        transaction_kind_.reset();
        return true;
    }

    [[nodiscard]] bool
    can_reserve_active_capture(LaneId lane, const ResourceDemand& demand,
                               const ResourceDelta& active_entitlement_delta) const noexcept {
        ResourceVector next_active;
        if (lane.value >= lane_count_ || lanes_[lane.value].state != LogicalLaneState::Active ||
            transaction_kind_ || demand.active_entitlement != ResourceVector{} ||
            demand.reservation_added.device.active_lanes != 0 ||
            demand.reservation_credit.device.active_lanes != 0 ||
            demand.final_removed.device.active_lanes != 0 ||
            demand.final_added.device.active_lanes != 0 ||
            !detail::transition_fits(used_, demand.reservation_credit, demand.reservation_added,
                                     capacity_) ||
            !detail::transition_fits(used_, demand.final_removed, demand.final_added, capacity_) ||
            !apply_delta(lanes_[lane.value].resources, active_entitlement_delta, next_active) ||
            next_active.device.active_lanes != 1) {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool
    reserve_active_capture(LaneId lane, const ResourceDemand& demand,
                           const ResourceDelta& active_entitlement_delta) noexcept {
        if (!can_reserve_active_capture(lane, demand, active_entitlement_delta)) { return false; }
        transaction_lane_ = lane;
        transaction_kind_ = ContextTransactionKind::ActiveCapture;
        return true;
    }

    [[nodiscard]] bool complete_active_capture(LaneId lane, const ResourceDelta& aggregate_delta,
                                               const ResourceDelta& active_delta) noexcept {
        if (lane.value >= lane_count_ || lanes_[lane.value].state != LogicalLaneState::Active ||
            !transaction_lane_ || *transaction_lane_ != lane ||
            transaction_kind_ != ContextTransactionKind::ActiveCapture ||
            aggregate_delta.removed.device.active_lanes != 0 ||
            aggregate_delta.added.device.active_lanes != 0 ||
            active_delta.removed.device.active_lanes != 0 ||
            active_delta.added.device.active_lanes != 0) {
            return false;
        }
        ResourceVector aggregate;
        ResourceVector active;
        if (!apply_delta(used_, aggregate_delta, aggregate) ||
            !apply_delta(lanes_[lane.value].resources, active_delta, active) ||
            active.device.active_lanes != 1) {
            return false;
        }
        used_                        = aggregate;
        lanes_[lane.value].resources = active;
        transaction_lane_.reset();
        transaction_kind_.reset();
        return true;
    }

    [[nodiscard]] bool cancel_active_capture(LaneId lane) noexcept {
        if (lane.value >= lane_count_ || !transaction_lane_ || *transaction_lane_ != lane ||
            transaction_kind_ != ContextTransactionKind::ActiveCapture ||
            lanes_[lane.value].state != LogicalLaneState::Active) {
            return false;
        }
        transaction_lane_.reset();
        transaction_kind_.reset();
        return true;
    }

    [[nodiscard]] bool can_reserve_replica_transition(const ResourceDemand& demand) const noexcept {
        if (transaction_kind_ || demand.active_entitlement != ResourceVector{} ||
            demand.reservation_credit.device.active_lanes != 0 ||
            demand.reservation_added.device.active_lanes != 0 ||
            demand.final_removed.device.active_lanes != 0 ||
            demand.final_added.device.active_lanes != 0 ||
            !detail::transition_fits(used_, demand.reservation_credit, demand.reservation_added,
                                     capacity_) ||
            !detail::transition_fits(used_, demand.final_removed, demand.final_added, capacity_)) {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool reserve_replica_transition(const ResourceDemand& demand) noexcept {
        if (!can_reserve_replica_transition(demand)) { return false; }
        transaction_kind_ = ContextTransactionKind::ReplicaTransition;
        return true;
    }

    [[nodiscard]] bool complete_replica_transition(const ResourceDelta& delta) noexcept {
        if (transaction_kind_ != ContextTransactionKind::ReplicaTransition || transaction_lane_ ||
            delta.removed.device.active_lanes != 0 || delta.added.device.active_lanes != 0) {
            return false;
        }
        ResourceVector next;
        if (!apply_delta(used_, delta, next)) { return false; }
        used_ = next;
        transaction_kind_.reset();
        return true;
    }

    [[nodiscard]] bool cancel_replica_transition() noexcept {
        if (transaction_kind_ != ContextTransactionKind::ReplicaTransition || transaction_lane_) {
            return false;
        }
        transaction_kind_.reset();
        return true;
    }

    [[nodiscard]] bool complete_active(LaneId id, ResourceVector active,
                                       const ResourceDelta& delta) noexcept {
        if (id.value >= lane_count_ || lanes_[id.value].state != LogicalLaneState::Active ||
            lanes_[id.value].resources != active || delta.removed.device.active_lanes != 1 ||
            delta.added.device.active_lanes != 0) {
            return false;
        }
        ResourceVector next;
        if (!apply_delta(used_, delta, next)) { return false; }
        lanes_[id.value] = {};
        used_            = next;
        return true;
    }

    [[nodiscard]] bool release_inactive(ResourceVector resources) noexcept {
        if (resources.device.active_lanes != 0 || transaction_kind_) { return false; }
        ResourceVector next;
        if (!detail::subtract_resources(used_, resources, next)) { return false; }
        used_ = next;
        return true;
    }

    void clear() noexcept {
        for (std::uint32_t lane = 0; lane < lane_count_; ++lane) { lanes_[lane] = {}; }
        used_ = {};
        transaction_lane_.reset();
        transaction_kind_.reset();
    }

private:
    [[nodiscard]] bool apply_delta(ResourceVector current, const ResourceDelta& delta,
                                   ResourceVector& out) const noexcept {
        ResourceVector remaining;
        return detail::subtract_resources(current, delta.removed, remaining) &&
               detail::add_resources(remaining, delta.added, out) && resources_fit(out, capacity_);
    }

    ResourceVector capacity_;
    ResourceVector used_;
    std::uint32_t lane_count_ = 0;
    std::array<LogicalLaneSnapshot, kMaximumConcurrency> lanes_{};
    std::optional<LaneId> transaction_lane_;
    std::optional<ContextTransactionKind> transaction_kind_;
};

struct ResourceCandidateDescriptor {
    ResourceDemand demand;
    ResourceVector source_resources;
    PrefillWork remaining_prefill_work;
    std::vector<ContextTransferRequirement> transfer_requirements;
    std::uint32_t source_slot           = kInvalidCatalogSlot;
    std::uint32_t shared_source_slot    = kInvalidCatalogSlot;
    std::uint32_t reused_prompt_tokens  = 0;
    std::uint64_t service_work_quanta   = 0;
    CheckpointKind checkpoint_kind      = CheckpointKind::SessionEndpoint;
    ClaimDisposition source_disposition = ClaimDisposition::ConsumedToActive;
    bool publication_slot_available     = false;
};

// Deterministic semantic ordering used only when two numerical estimates are exactly equal.
struct CostTieBreak {
    std::uint32_t dropped_shared_stable    = 0;
    std::uint32_t dropped_live_session     = 0;
    std::uint32_t dropped_recent_private   = 0;
    std::uint32_t evicted_continuations    = 0;
    std::uint32_t dropped_checkpoints      = 0;
    std::uint64_t remaining_text_prefill   = 0;
    std::uint64_t remaining_vision_prefill = 0;
    std::uint32_t transferred_state_images = 0;
    std::uint64_t transferred_bytes        = 0;
    std::uint32_t copy_operations          = 0;
    std::uint32_t reused_prompt_tokens     = 0;
};

struct CostEstimate {
    std::uint64_t nanoseconds = 0;
    CostTieBreak tie_break;
};

struct RetentionObservation {
    RetentionClass retention_class     = RetentionClass::RecentPrivate;
    std::uint64_t exact_eligible_count = 0;
    std::uint64_t selected_hit_count   = 0;
    std::uint64_t last_hit_epoch       = 0;
};

struct PolicyObservationKey {
    bool shared            = false;
    std::uint32_t slot     = kInvalidCatalogSlot;
    std::uint64_t owner_id = 0;
    std::uint64_t revision = 0;
    CheckpointRef checkpoint;

    [[nodiscard]] friend constexpr bool operator==(const PolicyObservationKey&,
                                                   const PolicyObservationKey&) noexcept = default;
};

struct CheckpointObservation {
    CheckpointRef checkpoint;
    RetentionObservation observation;
};

namespace detail {

[[nodiscard]] constexpr ResourceVector resource_deficit(ResourceVector value,
                                                        ResourceVector capacity) noexcept {
    return ResourceVector{
        .device =
            {
                .active_lanes  = value.device.active_lanes > capacity.device.active_lanes
                                     ? value.device.active_lanes - capacity.device.active_lanes
                                     : 0,
                .state_slots   = value.device.state_slots > capacity.device.state_slots
                                     ? value.device.state_slots - capacity.device.state_slots
                                     : 0,
                .main_kv_pages = value.device.main_kv_pages > capacity.device.main_kv_pages
                                     ? value.device.main_kv_pages - capacity.device.main_kv_pages
                                     : 0,
                .backend_kv_pages =
                    value.device.backend_kv_pages > capacity.device.backend_kv_pages
                        ? value.device.backend_kv_pages - capacity.device.backend_kv_pages
                        : 0,
            },
        .host =
            {
                .state_slots = value.host.state_slots > capacity.host.state_slots
                                   ? value.host.state_slots - capacity.host.state_slots
                                   : 0,
                .kv_bytes    = value.host.kv_bytes > capacity.host.kv_bytes
                                   ? value.host.kv_bytes - capacity.host.kv_bytes
                                   : 0,
            },
    };
}

[[nodiscard]] constexpr ResourceVector max_resources(ResourceVector left,
                                                     ResourceVector right) noexcept {
    return ResourceVector{
        .device =
            {
                .active_lanes  = std::max(left.device.active_lanes, right.device.active_lanes),
                .state_slots   = std::max(left.device.state_slots, right.device.state_slots),
                .main_kv_pages = std::max(left.device.main_kv_pages, right.device.main_kv_pages),
                .backend_kv_pages =
                    std::max(left.device.backend_kv_pages, right.device.backend_kv_pages),
            },
        .host =
            {
                .state_slots = std::max(left.host.state_slots, right.host.state_slots),
                .kv_bytes    = std::max(left.host.kv_bytes, right.host.kv_bytes),
            },
    };
}

[[nodiscard]] inline ResourceVector demand_deficit(ResourceVector used,
                                                   const ResourceDemand& demand,
                                                   ResourceVector capacity) noexcept {
    ResourceVector reserved_base;
    ResourceVector reserved_total;
    ResourceVector final_base;
    ResourceVector final_total;
    if (!subtract_resources(used, demand.reservation_credit, reserved_base) ||
        !add_resources(reserved_base, demand.reservation_added, reserved_total) ||
        !subtract_resources(used, demand.final_removed, final_base) ||
        !add_resources(final_base, demand.final_added, final_total)) {
        return ResourceVector{
            .device =
                {
                    .active_lanes     = std::numeric_limits<std::uint32_t>::max(),
                    .state_slots      = std::numeric_limits<std::uint32_t>::max(),
                    .main_kv_pages    = std::numeric_limits<std::uint32_t>::max(),
                    .backend_kv_pages = std::numeric_limits<std::uint32_t>::max(),
                },
            .host =
                {
                    .state_slots = std::numeric_limits<std::uint32_t>::max(),
                    .kv_bytes    = std::numeric_limits<std::size_t>::max(),
                },
        };
    }
    return max_resources(resource_deficit(reserved_total, capacity),
                         resource_deficit(final_total, capacity));
}

[[nodiscard]] constexpr std::uint32_t deficit_dimension_count(ResourceVector value) noexcept {
    return static_cast<std::uint32_t>(value.device.active_lanes != 0) +
           static_cast<std::uint32_t>(value.device.state_slots != 0) +
           static_cast<std::uint32_t>(value.device.main_kv_pages != 0) +
           static_cast<std::uint32_t>(value.device.backend_kv_pages != 0) +
           static_cast<std::uint32_t>(value.host.state_slots != 0) +
           static_cast<std::uint32_t>(value.host.kv_bytes != 0);
}

[[nodiscard]] constexpr auto deficit_key(ResourceVector value) noexcept {
    return std::tuple{value.device.state_slots,      value.device.main_kv_pages,
                      value.device.backend_kv_pages, value.host.state_slots,
                      value.host.kv_bytes,           value.device.active_lanes};
}

} // namespace detail

template <class Package>
class ResourceManager {
public:
    using Program             = typename Package::Program;
    using PreparedPrompt      = typename Package::PreparedPrompt;
    using RequestBasePlan     = typename Package::RequestBasePlan;
    using AdmissionPlan       = typename Package::AdmissionPlan;
    using SequenceHandle      = typename Package::SequenceHandle;
    using ContinuationHandle  = typename Package::ContinuationHandle;
    using SharedPrefixHandle  = typename Package::SharedPrefixHandle;
    using CaptureOffer        = typename Package::CaptureOffer;
    using ContinuationSummary = typename Package::ContinuationSummary;
    using SharedPrefixSummary = typename Package::SharedPrefixSummary;
    using PrefixShortlistKey =
        std::remove_cvref_t<decltype(std::declval<SharedPrefixSummary>().checkpoint.shortlist_key)>;
    using CaptureAssessment                 = typename Package::CaptureAssessment;
    using ProgramActiveCaptureResult        = typename Package::ActiveCaptureResult;
    using CacheSessionKey                   = typename Package::CacheSessionKey;
    using ProgramContextTransactionProgress = typename Package::ContextTransactionProgress;
    using ProgramMaterializationResult      = typename Package::MaterializationResult;
    using ReplicaTransitionOption           = typename Package::ReplicaTransitionOption;
    using ProgramReplicaTransitionResult    = typename Package::ReplicaTransitionResult;
    using StartResult                       = typename Package::StartResult;
    using FinishResult                      = typename Package::FinishResult;
    using AbortResult                       = typename Package::AbortResult;
    using ReleaseResult                     = typename Package::ReleaseResult;

    enum class CatalogState : std::uint8_t {
        Vacant,
        Catalogued,
        Claimed,
        ReservedForActive,
    };

    enum class SharedCatalogState : std::uint8_t {
        Vacant,
        ReservedCapture,
        Claimed,
        Catalogued,
    };

    class Choice {
    public:
        Choice(Choice&&) noexcept        = default;
        Choice& operator=(Choice&&)      = delete;
        Choice(const Choice&)            = delete;
        Choice& operator=(const Choice&) = delete;

        [[nodiscard]] const RequestPlanSummary& summary() const noexcept {
            return plan_->summary();
        }

        [[nodiscard]] LaneId destination() const noexcept { return destination_; }

        [[nodiscard]] const ResourceVector& active_entitlement() const noexcept {
            return demand_.active_entitlement;
        }

        [[nodiscard]] bool needs_transfer() const noexcept { return needs_transfer_; }

        [[nodiscard]] bool temporal_eligible() const noexcept { return temporal_eligible_; }

        [[nodiscard]] const ProtectedHeadResourceProjection& projection() const noexcept {
            return projection_;
        }

    private:
        Choice(LaneId destination, AdmissionPlan&& plan, std::uint32_t catalog_capacity,
               std::optional<CacheSessionKey> session, RetentionClass retention,
               bool update_session_index)
            : destination_(destination) {
            plan_.emplace(std::move(plan));
            session_              = std::move(session);
            retention_            = retention;
            update_session_index_ = update_session_index;
            evictions_.reserve(catalog_capacity);
            eviction_ids_.reserve(catalog_capacity);
            eviction_revisions_.reserve(catalog_capacity);
            pressure_options_.reserve(catalog_capacity);
            eligible_observations_.reserve(catalog_capacity);
        }

        LaneId destination_{};
        std::optional<AdmissionPlan> plan_;
        ResourceDemand demand_;
        ResourceVector source_resources_;
        ClaimDisposition source_disposition_  = ClaimDisposition::ConsumedToActive;
        std::uint32_t source_slot_            = kInvalidCatalogSlot;
        std::uint64_t source_id_              = 0;
        std::uint64_t source_revision_        = 0;
        std::uint32_t shared_source_slot_     = kInvalidCatalogSlot;
        std::uint64_t shared_source_id_       = 0;
        std::uint64_t shared_source_revision_ = 0;
        std::uint32_t publication_slot_       = kInvalidCatalogSlot;
        std::vector<std::uint32_t> evictions_;
        std::vector<std::uint64_t> eviction_ids_;
        std::vector<std::uint64_t> eviction_revisions_;
        std::vector<typename Package::PressureOption> pressure_options_;
        std::vector<std::uint32_t> shared_evictions_;
        std::vector<std::uint64_t> shared_eviction_ids_;
        std::vector<std::uint64_t> shared_eviction_revisions_;
        std::vector<typename Package::PressureOption> shared_pressure_options_;
        std::vector<PolicyObservationKey> eligible_observations_;
        std::optional<PolicyObservationKey> selected_observation_;
        std::optional<CacheSessionKey> session_;
        RetentionClass retention_                   = RetentionClass::RecentPrivate;
        bool update_session_index_                  = true;
        bool needs_transfer_                        = false;
        bool temporal_eligible_                     = true;
        std::uint64_t predicted_materialization_ns_ = 0;
        ProtectedHeadResourceProjection projection_;

        friend class ResourceManager;
    };

    class PublishedActivation {
    public:
        PublishedActivation(PublishedActivation&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), result_(std::move(other.result_)),
              destination_(other.destination_), demand_(other.demand_),
              terminal_delta_(other.terminal_delta_), source_slot_(other.source_slot_),
              source_disposition_(other.source_disposition_),
              shared_source_slot_(other.shared_source_slot_),
              publication_slot_(other.publication_slot_), continuation_id_(other.continuation_id_) {
        }

        PublishedActivation& operator=(PublishedActivation&&)      = delete;
        PublishedActivation(const PublishedActivation&)            = delete;
        PublishedActivation& operator=(const PublishedActivation&) = delete;

        [[nodiscard]] const SequenceHandle& sequence() const {
            if (!result_) { throw std::logic_error("published activation is empty"); }
            return result_->sequence;
        }

        [[nodiscard]] ResourceVector active_resources() const {
            if (!result_) { throw std::logic_error("published activation is empty"); }
            return result_->active_resources;
        }

    private:
        PublishedActivation(ResourceManager& owner, StartResult&& result, LaneId destination,
                            ResourceDemand demand, ResourceDelta terminal_delta,
                            std::uint32_t source_slot, ClaimDisposition source_disposition,
                            std::uint32_t shared_source_slot, std::uint32_t publication_slot,
                            std::uint64_t continuation_id)
            : owner_(&owner), result_(std::move(result)), destination_(destination),
              demand_(demand), terminal_delta_(terminal_delta), source_slot_(source_slot),
              source_disposition_(source_disposition), shared_source_slot_(shared_source_slot),
              publication_slot_(publication_slot), continuation_id_(continuation_id) {}

        ResourceManager* owner_ = nullptr;
        std::optional<StartResult> result_;
        LaneId destination_{};
        ResourceDemand demand_;
        ResourceDelta terminal_delta_;
        std::uint32_t source_slot_           = kInvalidCatalogSlot;
        ClaimDisposition source_disposition_ = ClaimDisposition::ConsumedToActive;
        std::uint32_t shared_source_slot_    = kInvalidCatalogSlot;
        std::uint32_t publication_slot_      = kInvalidCatalogSlot;
        std::uint64_t continuation_id_       = 0;

        friend class ResourceManager;
    };

    struct MaterializationOutcome {
        ContextTransactionStatus status = ContextTransactionStatus::Aborted;
        std::optional<PublishedActivation> activation;
    };

    enum class MaterializationReserveResult : std::uint8_t {
        Reserved,
        Stale,
        Aborted,
    };

    enum class ActiveCaptureReserveResult : std::uint8_t {
        Reserved,
        Skipped,
    };

    struct ActiveCaptureOutcome {
        ContextTransactionStatus status = ContextTransactionStatus::Aborted;
    };

    enum class ReplicaTransitionReserveResult : std::uint8_t {
        Reserved,
        Skipped,
    };

    struct ReplicaTransitionOutcome {
        ContextTransactionStatus status = ContextTransactionStatus::Aborted;
    };

    using ContextTransactionOutcome =
        std::variant<ContextTransactionInProgress, MaterializationOutcome, ActiveCaptureOutcome,
                     ReplicaTransitionOutcome>;

    struct Inspection {
        Readiness readiness = Readiness::TemporarilyBlocked;
        std::optional<Choice> choice;
    };

    ResourceManager(ResourceVector capacity, std::uint32_t lane_count,
                    std::uint32_t private_catalog_capacity, std::uint32_t shared_catalog_capacity,
                    bool cache_enabled, std::uint32_t max_long_anchors, ContextCostModel cost_model)
        : ledger_(capacity, lane_count), lane_count_(lane_count),
          catalog_count_(private_catalog_capacity), shared_catalog_count_(shared_catalog_capacity),
          cache_enabled_(cache_enabled), catalog_(private_catalog_capacity),
          shared_catalog_(shared_catalog_capacity), session_index_(private_catalog_capacity),
          prefix_index_(checked_prefix_index_capacity(private_catalog_capacity,
                                                      shared_catalog_capacity, max_long_anchors)),
          max_long_anchors_(max_long_anchors), cost_model_(std::move(cost_model)) {
        if (private_catalog_capacity < lane_count) {
            throw std::invalid_argument(
                "private continuation capacity does not cover active lanes");
        }
        const std::size_t anchor_capacity = max_long_anchors_;
        if (anchor_capacity > std::numeric_limits<std::size_t>::max() - 3U) {
            throw std::overflow_error("private observation capacity overflowed");
        }
        // migrate_private_observations temporarily parks one displaced observation while it
        // rewrites a published entry in place. Reserve that scratch cell at construction so
        // terminal adoption never grows a control container.
        const std::size_t observation_capacity = 3U + anchor_capacity;
        for (CatalogEntry& entry : catalog_) {
            entry.summary.long_anchors.reserve(anchor_capacity);
            entry.observations.reserve(observation_capacity);
        }
    }

    [[nodiscard]] ProtectedHeadResourceProjection
    protected_head_projection(Program& program) const {
        return build_protected_projection(program, nullptr);
    }

    [[nodiscard]] Inspection inspect(Program& program, const PreparedPrompt& prompt,
                                     const RequestBasePlan& base) {
        if (!std::holds_alternative<std::monostate>(context_transaction_) ||
            program.has_context_transaction()) {
            return {.readiness = Readiness::TemporarilyBlocked};
        }
        std::optional<LaneId> destination;
        for (std::uint32_t lane = 0; lane < lane_count_; ++lane) {
            if (ledger_.lane(LaneId{lane}).state == LogicalLaneState::Free) {
                destination = LaneId{lane};
                break;
            }
        }
        if (!resources_fit(base.root_demand().active_entitlement, ledger_.capacity())) {
            return {.readiness = Readiness::PermanentlyInfeasible};
        }
        if (!destination) { return {.readiness = Readiness::TemporarilyBlocked}; }
        rebuild_prefix_indices();

        std::vector<std::optional<AdmissionPlan>> plans;
        std::vector<ResourceCandidateDescriptor> candidates;
        std::vector<std::uint32_t> residents;
        std::vector<std::optional<PolicyObservationKey>> candidate_observations;
        const std::size_t candidate_capacity = 1U + prefix_index_.size();
        plans.reserve(candidate_capacity);
        candidates.reserve(candidate_capacity);
        candidate_observations.reserve(candidate_capacity);
        residents.reserve(catalog_count_);
        const auto attach_cost_inputs = [](ResourceCandidateDescriptor& descriptor,
                                           const AdmissionPlan& plan) {
            descriptor.remaining_prefill_work = plan.remaining_prefill_work();
            const auto requirements           = plan.transfer_requirements();
            descriptor.transfer_requirements.assign(requirements.begin(), requirements.end());
        };

        bool vacant_catalog_slot = false;
        for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
            const CatalogEntry& entry = catalog_[slot];
            if (entry.state == CatalogState::Vacant) {
                vacant_catalog_slot = true;
                continue;
            }
            if (entry.state != CatalogState::Catalogued || !entry.handle ||
                entry.active_references != 0) {
                continue;
            }
            residents.push_back(slot);
        }

        std::optional<AdmissionPlan> root = program.inspect_admission(
            prompt, base, *destination, nullptr, nullptr, std::nullopt, false);
        if (!root) { throw std::logic_error("Program rejected its root admission assessment"); }
        plans.emplace_back(std::move(*root));
        candidates.push_back(ResourceCandidateDescriptor{
            .demand                     = plans.back()->demand(),
            .service_work_quanta        = plans.back()->summary().service_work_quanta,
            .publication_slot_available = vacant_catalog_slot,
        });
        attach_cost_inputs(candidates.back(), *plans.back());
        candidate_observations.emplace_back();

        if (cache_enabled_) {
            for (const PrefixIndexEntry& index : prefix_index_) {
                if (!valid_prefix_index_entry(index)) { continue; }
                const std::optional<PrefixShortlistKey> incoming =
                    base.prefix_shortlist_key(index.key.frontier);
                if (!incoming || *incoming != index.key) { continue; }

                if (!index.shared) {
                    const CatalogEntry& entry = catalog_[index.slot];
                    if (entry.active_references != 0) { continue; }
                    // Content selects the source; session identity only protects ownership of an
                    // existing named lineage. An anonymous source has no SessionIndex owner to
                    // preserve, even when the incoming request does not update SessionIndex.
                    const bool must_retain_private_source =
                        entry.session && (!base.context_cache().session_key ||
                                          *entry.session != *base.context_cache().session_key ||
                                          !base.context_cache().update_session_index);
                    std::optional<AdmissionPlan> assessment = program.inspect_admission(
                        prompt, base, *destination, &*entry.handle, nullptr, index.checkpoint,
                        must_retain_private_source);
                    if (!assessment) { continue; }
                    if (assessment->summary().reusable_prompt_tokens == 0 ||
                        (must_retain_private_source &&
                         assessment->source_disposition() != ClaimDisposition::Retained)) {
                        throw std::logic_error("Program returned an invalid private assessment");
                    }
                    plans.emplace_back(std::move(*assessment));
                    const RequestPlanSummary& summary = plans.back()->summary();
                    candidates.push_back(ResourceCandidateDescriptor{
                        .demand                     = plans.back()->demand(),
                        .source_resources           = plans.back()->source_resources(),
                        .source_slot                = index.slot,
                        .reused_prompt_tokens       = summary.reusable_prompt_tokens,
                        .service_work_quanta        = summary.service_work_quanta,
                        .checkpoint_kind            = index.checkpoint.kind,
                        .source_disposition         = plans.back()->source_disposition(),
                        .publication_slot_available = plans.back()->source_disposition() ==
                                                          ClaimDisposition::ConsumedToActive ||
                                                      vacant_catalog_slot,
                    });
                    attach_cost_inputs(candidates.back(), *plans.back());
                    candidate_observations.emplace_back(PolicyObservationKey{
                        .shared     = false,
                        .slot       = index.slot,
                        .owner_id   = entry.id,
                        .revision   = entry.revision,
                        .checkpoint = index.checkpoint,
                    });
                    continue;
                }

                const SharedCatalogEntry& entry         = shared_catalog_[index.slot];
                std::optional<AdmissionPlan> assessment = program.inspect_admission(
                    prompt, base, *destination, nullptr, &*entry.handle, index.checkpoint, false);
                if (!assessment) { continue; }
                if (assessment->summary().reusable_prompt_tokens == 0 ||
                    assessment->source_disposition() != ClaimDisposition::Retained ||
                    assessment->source_resources() != ResourceVector{}) {
                    throw std::logic_error("Program returned an invalid shared assessment");
                }
                plans.emplace_back(std::move(*assessment));
                const RequestPlanSummary& summary = plans.back()->summary();
                candidates.push_back(ResourceCandidateDescriptor{
                    .demand                     = plans.back()->demand(),
                    .shared_source_slot         = index.slot,
                    .reused_prompt_tokens       = summary.reusable_prompt_tokens,
                    .service_work_quanta        = summary.service_work_quanta,
                    .checkpoint_kind            = CheckpointKind::SharedStablePrefix,
                    .source_disposition         = ClaimDisposition::Retained,
                    .publication_slot_available = vacant_catalog_slot,
                });
                attach_cost_inputs(candidates.back(), *plans.back());
                candidate_observations.emplace_back(PolicyObservationKey{
                    .shared     = true,
                    .slot       = index.slot,
                    .owner_id   = entry.id,
                    .revision   = entry.revision,
                    .checkpoint = index.checkpoint,
                });
            }
        }

        struct ClosedCandidate {
            bool found                  = false;
            std::size_t candidate_index = 0;
            ResourceDemand demand;
            std::vector<std::uint32_t> pressure_slots;
            std::vector<typename Package::PressureOption> pressure_options;
            std::vector<std::uint32_t> shared_pressure_slots;
            std::vector<typename Package::PressureOption> shared_pressure_options;
            CostEstimate cost;
            std::uint64_t transfer_bytes    = 0;
            std::uint32_t logical_evictions = 0;
        };

        const auto inspect_combined_pressure_effect =
            [&](const AdmissionPlan& admission, const std::vector<std::uint32_t>& private_slots,
                const std::vector<typename Package::PressureOption>& private_options,
                const std::vector<std::uint32_t>& shared_slots,
                const std::vector<typename Package::PressureOption>& shared_options)
            -> std::optional<MaterializationPressureEffect> {
            if (private_slots.size() != private_options.size() ||
                shared_slots.size() != shared_options.size()) {
                throw std::logic_error("pressure selection is not row aligned");
            }
            std::vector<const ContinuationHandle*> private_handles;
            private_handles.reserve(private_slots.size());
            for (const std::uint32_t slot : private_slots) {
                if (slot >= catalog_count_ || !catalog_[slot].handle) { return std::nullopt; }
                private_handles.push_back(&*catalog_[slot].handle);
            }
            std::vector<const SharedPrefixHandle*> shared_handles;
            shared_handles.reserve(shared_slots.size());
            for (const std::uint32_t slot : shared_slots) {
                if (slot >= shared_catalog_count_ || !shared_catalog_[slot].handle) {
                    return std::nullopt;
                }
                shared_handles.push_back(&*shared_catalog_[slot].handle);
            }
            return program.inspect_combined_pressure_effect(
                admission, private_handles, private_options, shared_handles, shared_options);
        };

        const auto close_candidate = [&](std::size_t candidate_index,
                                         bool allow_preserving) -> ClosedCandidate {
            ClosedCandidate closed;
            closed.candidate_index = candidate_index;
            closed.demand          = candidates[candidate_index].demand;
            closed.cost            = candidate_base_cost(candidates[candidate_index]);
            closed.pressure_slots.reserve(catalog_count_);
            closed.pressure_options.reserve(catalog_count_);
            closed.shared_pressure_slots.reserve(shared_catalog_count_);
            closed.shared_pressure_options.reserve(shared_catalog_count_);

            const std::uint64_t maximum_steps =
                static_cast<std::uint64_t>(catalog_count_) + shared_catalog_count_;
            for (std::uint64_t step = 0; step <= maximum_steps; ++step) {
                bool supplies_publication = candidates[candidate_index].publication_slot_available;
                for (const auto& option : closed.pressure_options) {
                    supplies_publication = supplies_publication || option.evicts_continuation;
                }
                if (supplies_publication &&
                    detail::valid_demand(closed.demand,
                                         candidates[candidate_index].source_resources,
                                         candidates[candidate_index].source_disposition) &&
                    detail::demand_fits(ledger_.used(), closed.demand, ledger_.capacity())) {
                    closed.found = true;
                    return closed;
                }
                if (step == maximum_steps) { break; }

                const ResourceVector deficit =
                    detail::demand_deficit(ledger_.used(), closed.demand, ledger_.capacity());
                std::optional<bool> best_shared;
                std::optional<std::uint32_t> best_slot;
                std::optional<typename Package::PressureOption> best_option;
                CostEstimate best_cost;
                ResourceVector best_deficit;
                ResourceDemand best_demand;
                std::uint64_t best_hit_epoch = 0;
                std::uint64_t best_owner_id  = 0;
                const auto consider_option   = [&](bool shared, std::uint32_t slot,
                                                 std::uint64_t hit_epoch, std::uint64_t owner_id,
                                                 typename Package::PressureOption option,
                                                 const CatalogEntry* private_entry,
                                                 const SharedCatalogEntry* shared_entry) {
                    if (option.shared_owner != shared) {
                        throw std::logic_error("Program pressure owner kind is invalid");
                    }
                    std::vector<std::uint32_t> next_private_slots = closed.pressure_slots;
                    std::vector<typename Package::PressureOption> next_private_options =
                        closed.pressure_options;
                    std::vector<std::uint32_t> next_shared_slots = closed.shared_pressure_slots;
                    std::vector<typename Package::PressureOption> next_shared_options =
                        closed.shared_pressure_options;
                    if (shared) {
                        next_shared_slots.push_back(slot);
                        next_shared_options.push_back(option);
                    } else {
                        next_private_slots.push_back(slot);
                        next_private_options.push_back(option);
                    }
                    const std::optional<MaterializationPressureEffect> combined =
                        inspect_combined_pressure_effect(*plans[candidate_index],
                                                           next_private_slots, next_private_options,
                                                           next_shared_slots, next_shared_options);
                    if (!combined) { return; }
                    ResourceDemand next_demand = candidates[candidate_index].demand;
                    if (!detail::augment_demand(next_demand, *combined)) { return; }
                    const ResourceVector next_deficit =
                        detail::demand_deficit(ledger_.used(), next_demand, ledger_.capacity());
                    const bool supplies_cell =
                        !supplies_publication && !shared && option.evicts_continuation;
                    if (!supplies_cell && next_deficit == deficit &&
                        (allow_preserving || !option.evicts_continuation)) {
                        return;
                    }
                    const CostEstimate option_cost =
                        pressure_cost(option, private_entry, shared_entry);
                    const auto option_key = std::tuple{
                        detail::deficit_dimension_count(next_deficit),
                        detail::deficit_key(next_deficit),
                        option.evicts_continuation ? 1U : 0U,
                        hit_epoch,
                        owner_id,
                        option.id,
                        shared ? 1U : 0U,
                        slot,
                    };
                    const auto best_key = best_slot
                                              ? std::tuple{
                                                    detail::deficit_dimension_count(best_deficit),
                                                    detail::deficit_key(best_deficit),
                                                    best_option->evicts_continuation ? 1U : 0U,
                                                    best_hit_epoch,
                                                    best_owner_id,
                                                    best_option->id,
                                                    *best_shared ? 1U : 0U,
                                                    *best_slot,
                                                }
                                              : option_key;
                    const int cost_order = best_slot ? compare_cost(option_cost, best_cost) : -1;
                    if (!best_slot || cost_order < 0 ||
                        (cost_order == 0 && option_key < best_key)) {
                        best_shared    = shared;
                        best_slot      = slot;
                        best_option    = std::move(option);
                        best_cost      = option_cost;
                        best_deficit   = next_deficit;
                        best_demand    = next_demand;
                        best_hit_epoch = hit_epoch;
                        best_owner_id  = owner_id;
                    }
                };
                for (const std::uint32_t resident : residents) {
                    if (resident == candidates[candidate_index].source_slot ||
                        std::find(closed.pressure_slots.begin(), closed.pressure_slots.end(),
                                  resident) != closed.pressure_slots.end()) {
                        continue;
                    }
                    const CatalogEntry& entry = catalog_[resident];
                    if (allow_preserving) {
                        std::vector<typename Package::PressureOption> preserving =
                            program.inspect_pressure_options(*plans[candidate_index], *entry.handle,
                                                             deficit);
                        for (typename Package::PressureOption& option : preserving) {
                            if (option.evicts_continuation) {
                                throw std::logic_error(
                                    "Program preserving option evicted its owner");
                            }
                            consider_option(false, resident, newest_hit_epoch(entry), entry.id,
                                            std::move(option), &entry, nullptr);
                        }
                    }
                    typename Package::PressureOption eviction =
                        program.inspect_eviction_option(*entry.handle);
                    if (!eviction.evicts_continuation) {
                        throw std::logic_error("Program eviction option retained its owner");
                    }
                    consider_option(false, resident, newest_hit_epoch(entry), entry.id,
                                    std::move(eviction), &entry, nullptr);
                }
                for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
                    if (slot == candidates[candidate_index].shared_source_slot ||
                        std::find(closed.shared_pressure_slots.begin(),
                                  closed.shared_pressure_slots.end(),
                                  slot) != closed.shared_pressure_slots.end()) {
                        continue;
                    }
                    const SharedCatalogEntry& entry = shared_catalog_[slot];
                    if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                        entry.transaction_pins != 0 || entry.summary.active_references != 0) {
                        continue;
                    }
                    if (allow_preserving) {
                        std::vector<typename Package::PressureOption> preserving =
                            program.inspect_shared_pressure_options(*plans[candidate_index],
                                                                    *entry.handle, deficit);
                        for (typename Package::PressureOption& option : preserving) {
                            if (option.evicts_continuation) {
                                throw std::logic_error(
                                    "Program shared preserving option evicted its owner");
                            }
                            consider_option(true, slot, entry.observation.last_hit_epoch, entry.id,
                                            std::move(option), nullptr, &entry);
                        }
                    }
                    typename Package::PressureOption eviction =
                        program.inspect_shared_eviction_option(*entry.handle);
                    if (!eviction.evicts_continuation || !eviction.shared_owner) {
                        throw std::logic_error("Program shared eviction option is invalid");
                    }
                    consider_option(true, slot, entry.observation.last_hit_epoch, entry.id,
                                    std::move(eviction), nullptr, &entry);
                }
                if (!best_slot || !best_option || !best_shared) { break; }
                closed.demand = best_demand;
                if (*best_shared) {
                    closed.shared_pressure_slots.push_back(*best_slot);
                    closed.shared_pressure_options.push_back(*best_option);
                } else {
                    closed.pressure_slots.push_back(*best_slot);
                    closed.pressure_options.push_back(*best_option);
                }
                add_cost(closed.cost, best_cost);
                closed.transfer_bytes =
                    saturating_add(closed.transfer_bytes, best_option->transfer_bytes);
                if (best_option->evicts_continuation) { ++closed.logical_evictions; }
            }
            return closed;
        };

        const auto better_closed = [&](const ClosedCandidate& candidate,
                                       const ClosedCandidate& current) {
            const ResourceCandidateDescriptor& candidate_desc =
                candidates[candidate.candidate_index];
            const ResourceCandidateDescriptor& current_desc = candidates[current.candidate_index];
            const int cost_order = compare_cost(candidate.cost, current.cost);
            if (cost_order != 0) { return cost_order < 0; }
            const auto candidate_key = std::tuple{
                candidate.logical_evictions,
                candidate.transfer_bytes,
                candidate.pressure_slots.size() + candidate.shared_pressure_slots.size(),
                std::numeric_limits<std::uint32_t>::max() - candidate_desc.reused_prompt_tokens,
                candidate_desc.service_work_quanta,
                candidate_desc.checkpoint_kind,
                candidate.candidate_index};
            const auto current_key = std::tuple{
                current.logical_evictions,
                current.transfer_bytes,
                current.pressure_slots.size() + current.shared_pressure_slots.size(),
                std::numeric_limits<std::uint32_t>::max() - current_desc.reused_prompt_tokens,
                current_desc.service_work_quanta,
                current_desc.checkpoint_kind,
                current.candidate_index};
            return candidate_key < current_key;
        };
        std::vector<ClosedCandidate> closed_candidates;
        closed_candidates.reserve(candidates.size() * 2U);
        for (std::size_t candidate_index = 0; candidate_index < candidates.size();
             ++candidate_index) {
            ClosedCandidate mixed = close_candidate(candidate_index, true);
            if (mixed.found) { closed_candidates.push_back(std::move(mixed)); }
            ClosedCandidate eviction_only = close_candidate(candidate_index, false);
            if (eviction_only.found) { closed_candidates.push_back(std::move(eviction_only)); }
        }
        std::sort(closed_candidates.begin(), closed_candidates.end(), better_closed);

        std::optional<ClosedCandidate> selected;
        std::optional<ProtectedHeadResourceProjection> selected_projection;
        for (ClosedCandidate& closed : closed_candidates) {
            const ResourceCandidateDescriptor& descriptor = candidates[closed.candidate_index];
            const ProjectedActivation hypothetical{
                .destination        = *destination,
                .active_resources   = closed.demand.active_entitlement.device,
                .source_slot        = descriptor.source_slot,
                .source_disposition = descriptor.source_disposition,
                .shared_source_slot = descriptor.shared_source_slot,
            };
            ProtectedHeadResourceProjection projection =
                build_protected_projection(program, &hypothetical);
            if (!device_state_headroom_preserved(projection,
                                                 ledger_.used().device.active_lanes + 1U)) {
                continue;
            }
            selected.emplace(std::move(closed));
            selected_projection.emplace(std::move(projection));
            break;
        }
        if (!selected) { return {.readiness = Readiness::TemporarilyBlocked}; }

        const std::size_t index = selected->candidate_index;
        std::vector<const ContinuationHandle*> pressure_handles;
        pressure_handles.reserve(selected->pressure_slots.size());
        for (const std::uint32_t slot : selected->pressure_slots) {
            pressure_handles.push_back(&*catalog_[slot].handle);
        }
        std::vector<const SharedPrefixHandle*> shared_pressure_handles;
        shared_pressure_handles.reserve(selected->shared_pressure_slots.size());
        for (const std::uint32_t slot : selected->shared_pressure_slots) {
            shared_pressure_handles.push_back(&*shared_catalog_[slot].handle);
        }
        std::optional<AdmissionPlan> composed = program.compose_materialization(
            std::move(*plans[index]), pressure_handles, selected->pressure_options,
            shared_pressure_handles, selected->shared_pressure_options);
        plans[index].reset();
        if (!composed) { return {.readiness = Readiness::TemporarilyBlocked}; }
        if (composed->demand() != selected->demand ||
            composed->source_resources() != candidates[index].source_resources ||
            composed->source_disposition() != candidates[index].source_disposition) {
            throw std::logic_error("Program changed candidate ownership while composing pressure");
        }
        const ResourceVector source_resources     = composed->source_resources();
        const ClaimDisposition source_disposition = composed->source_disposition();
        if (!detail::valid_demand(composed->demand(), source_resources, source_disposition)) {
            return {.readiness = Readiness::TemporarilyBlocked};
        }
        const auto& cache = base.context_cache();
        Choice choice(*destination, std::move(*composed), catalog_count_, cache.session_key,
                      cache.retention, cache.update_session_index);
        choice.demand_                       = selected->demand;
        choice.source_resources_             = source_resources;
        choice.source_disposition_           = source_disposition;
        choice.source_slot_                  = candidates[index].source_slot;
        choice.shared_source_slot_           = candidates[index].shared_source_slot;
        choice.needs_transfer_               = selected->transfer_bytes != 0;
        choice.predicted_materialization_ns_ = selected->cost.nanoseconds;
        choice.needs_transfer_    = choice.needs_transfer_ || choice.plan_->needs_transfer();
        choice.temporal_eligible_ = !choice.needs_transfer_ && choice.plan_->temporal_eligible();
        for (const auto& observation : candidate_observations) {
            if (observation && std::find(choice.eligible_observations_.begin(),
                                         choice.eligible_observations_.end(),
                                         *observation) == choice.eligible_observations_.end()) {
                choice.eligible_observations_.push_back(*observation);
            }
        }
        choice.selected_observation_ = candidate_observations[index];
        if (choice.shared_source_slot_ != kInvalidCatalogSlot) {
            const SharedCatalogEntry& source = shared_catalog_[choice.shared_source_slot_];
            choice.shared_source_id_         = source.id;
            choice.shared_source_revision_   = source.revision;
        }
        if (choice.source_slot_ != kInvalidCatalogSlot &&
            choice.source_disposition_ == ClaimDisposition::ConsumedToActive) {
            const CatalogEntry& source = catalog_[choice.source_slot_];
            choice.source_id_          = source.id;
            choice.source_revision_    = source.revision;
            choice.publication_slot_   = choice.source_slot_;
        } else {
            if (choice.source_slot_ != kInvalidCatalogSlot) {
                const CatalogEntry& source = catalog_[choice.source_slot_];
                choice.source_id_          = source.id;
                choice.source_revision_    = source.revision;
            }
            for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
                if (catalog_[slot].state == CatalogState::Vacant) {
                    choice.publication_slot_ = slot;
                    break;
                }
            }
            if (choice.publication_slot_ == kInvalidCatalogSlot) {
                const auto victim = std::find_if(
                    selected->pressure_options.begin(), selected->pressure_options.end(),
                    [](const auto& option) { return option.evicts_continuation; });
                if (victim == selected->pressure_options.end()) {
                    throw std::logic_error("closed root plan has no publication descriptor");
                }
                choice.publication_slot_ = selected->pressure_slots[static_cast<std::size_t>(
                    victim - selected->pressure_options.begin())];
            }
        }
        for (std::size_t victim = 0; victim < selected->pressure_slots.size(); ++victim) {
            const std::uint32_t slot = selected->pressure_slots[victim];
            choice.evictions_.push_back(slot);
            choice.eviction_ids_.push_back(catalog_[slot].id);
            choice.eviction_revisions_.push_back(catalog_[slot].revision);
            choice.pressure_options_.push_back(selected->pressure_options[victim]);
        }
        for (std::size_t victim = 0; victim < selected->shared_pressure_slots.size(); ++victim) {
            const std::uint32_t slot = selected->shared_pressure_slots[victim];
            choice.shared_evictions_.push_back(slot);
            choice.shared_eviction_ids_.push_back(shared_catalog_[slot].id);
            choice.shared_eviction_revisions_.push_back(shared_catalog_[slot].revision);
            choice.shared_pressure_options_.push_back(selected->shared_pressure_options[victim]);
        }
        choice.projection_ = std::move(*selected_projection);
        return {.readiness = choice.needs_transfer_ ? Readiness::NeedsTransfer : Readiness::Ready,
                .choice    = std::optional<Choice>(std::move(choice))};
    }

    [[nodiscard]] MaterializationReserveResult
    reserve_materialization(Program& program, Choice&& choice, PreparedPrompt&& prompt,
                            CancellationFlagView cancellation) {
        if (!std::holds_alternative<std::monostate>(context_transaction_)) {
            throw std::logic_error("ResourceManager already owns a materialization");
        }
        validate_choice(choice);
        if (cancellation.requested()) { return MaterializationReserveResult::Aborted; }
        const bool publish_continuation = choice.summary().publish_continuation;

        std::vector<const ContinuationHandle*> victim_handles;
        victim_handles.reserve(choice.evictions_.size());
        for (const std::uint32_t slot : choice.evictions_) {
            victim_handles.push_back(&*catalog_[slot].handle);
        }
        std::vector<const SharedPrefixHandle*> shared_victim_handles;
        shared_victim_handles.reserve(choice.shared_evictions_.size());
        for (const std::uint32_t slot : choice.shared_evictions_) {
            shared_victim_handles.push_back(&*shared_catalog_[slot].handle);
        }
        const ContinuationHandle* source_handle = choice.source_slot_ == kInvalidCatalogSlot
                                                      ? nullptr
                                                      : &*catalog_[choice.source_slot_].handle;
        const SharedPrefixHandle* shared_source_handle =
            choice.shared_source_slot_ == kInvalidCatalogSlot
                ? nullptr
                : &*shared_catalog_[choice.shared_source_slot_].handle;
        const PreflightStatus preflight = program.revalidate_materialization(
            *choice.plan_, prompt, source_handle, shared_source_handle, victim_handles,
            shared_victim_handles);
        if (preflight == PreflightStatus::StalePolicyState) {
            return MaterializationReserveResult::Stale;
        }
        if (preflight != PreflightStatus::Ready) {
            throw std::logic_error("Program rejected a structurally valid materialization plan");
        }
        if (!ledger_.can_reserve_materialization(choice.destination_, choice.demand_)) {
            throw std::logic_error("materialization lane reservation changed after inspection");
        }

        const auto mark_claimed = [](CatalogEntry& entry) noexcept {
            entry.state = CatalogState::Claimed;
        };
        if (choice.shared_source_slot_ != kInvalidCatalogSlot) {
            if (shared_catalog_[choice.shared_source_slot_].transaction_pins ==
                std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error("shared-prefix transaction pin overflow");
            }
        }
        const runtime::ContextTransactionReserveStatus reserved = program.reserve_materialization(
            std::move(*choice.plan_), std::move(prompt), source_handle, shared_source_handle,
            victim_handles, shared_victim_handles, cancellation);
        choice.plan_.reset();
        if (reserved == runtime::ContextTransactionReserveStatus::Aborted) {
            return MaterializationReserveResult::Aborted;
        }

        // Program now owns every physical/control reservation and all throwing backing storage.
        // The single worker makes the following ledger/catalog adoption an invariant-preserving,
        // allocation-free commit; a mismatch after this point is Engine-wide failure.
        if (!ledger_.reserve_materialization(choice.destination_, choice.demand_)) {
            throw std::logic_error("materialization lane reservation changed after inspection");
        }
        if (choice.source_slot_ != kInvalidCatalogSlot) {
            mark_claimed(catalog_[choice.source_slot_]);
        }
        if (choice.shared_source_slot_ != kInvalidCatalogSlot) {
            ++shared_catalog_[choice.shared_source_slot_].transaction_pins;
        }
        for (const std::uint32_t slot : choice.evictions_) { mark_claimed(catalog_[slot]); }
        for (const std::uint32_t slot : choice.shared_evictions_) {
            shared_catalog_[slot].state = SharedCatalogState::Claimed;
        }

        MaterializationRecord record;
        record.destination                  = choice.destination_;
        record.demand                       = choice.demand_;
        record.source_slot                  = choice.source_slot_;
        record.source_id                    = choice.source_id_;
        record.source_disposition           = choice.source_disposition_;
        record.shared_source_slot           = choice.shared_source_slot_;
        record.shared_source_id             = choice.shared_source_id_;
        record.publication_slot             = choice.publication_slot_;
        record.evictions                    = std::move(choice.evictions_);
        record.eviction_ids                 = std::move(choice.eviction_ids_);
        record.pressure_options             = std::move(choice.pressure_options_);
        record.shared_evictions             = std::move(choice.shared_evictions_);
        record.shared_eviction_ids          = std::move(choice.shared_eviction_ids_);
        record.shared_pressure_options      = std::move(choice.shared_pressure_options_);
        record.eligible_observations        = std::move(choice.eligible_observations_);
        record.selected_observation         = std::move(choice.selected_observation_);
        record.session                      = std::move(choice.session_);
        record.retention                    = choice.retention_;
        record.update_session_index         = choice.update_session_index_;
        record.predicted_materialization_ns = choice.predicted_materialization_ns_;
        record.publish_continuation         = publish_continuation;
        static_assert(std::is_nothrow_move_constructible_v<MaterializationRecord>);
        context_transaction_.template emplace<MaterializationRecord>(std::move(record));
        return MaterializationReserveResult::Reserved;
    }

private:
    [[nodiscard]] MaterializationOutcome
    adopt_materialization_progress(Program& program, ProgramMaterializationResult&& result) {
        MaterializationRecord* record_ptr =
            std::get_if<MaterializationRecord>(&context_transaction_);
        if (record_ptr == nullptr || !program.has_context_transaction()) {
            throw std::logic_error("ResourceManager has no progressable materialization");
        }
        MaterializationRecord& record = *record_ptr;
        if (result.victims.size() != record.evictions.size() ||
            record.pressure_options.size() != record.evictions.size()) {
            throw std::logic_error("Program returned an incomplete victim acknowledgement");
        }
        if (result.status == ContextTransactionStatus::Published) {
            if (!result.published ||
                result.published->active_resources != record.demand.active_entitlement ||
                result.resource_delta.removed != record.demand.final_removed ||
                result.resource_delta.added != record.demand.final_added) {
                throw std::logic_error("Program publication violated the selected resource delta");
            }
            adopt_retention_observations(record);
            context_stats_.last_predicted_materialization_ns = record.predicted_materialization_ns;
        }
        ResourceDelta acknowledged_owner_delta;
        const auto accumulate_owner_delta = [&](const ResourceDelta& delta) {
            ResourceDelta next;
            if (!detail::add_resource_deltas(acknowledged_owner_delta, delta, next)) {
                throw std::logic_error("materialization owner delta overflowed");
            }
            acknowledged_owner_delta = next;
        };
        const auto validate_pressure_acknowledgement = [&](const auto& victim, const auto& option) {
            const bool valid_eviction =
                option.evicts_continuation && ((victim.disposition == ClaimDisposition::Evicted &&
                                                victim.resource_delta.added == ResourceVector{}) ||
                                               (victim.disposition == ClaimDisposition::Retained &&
                                                victim.resource_delta == ResourceDelta{}));
            const bool valid_transition =
                !option.evicts_continuation &&
                detail::resource_delta_within(victim.resource_delta, option.effect) &&
                (result.status != ContextTransactionStatus::Published ||
                 victim.resource_delta == option.effect);
            if (!valid_eviction && !valid_transition) {
                throw std::logic_error(
                    "pressure claim returned an invalid committed resource delta");
            }
            accumulate_owner_delta(victim.resource_delta);
        };
        if (result.shared_victims.size() != record.shared_evictions.size() ||
            record.shared_pressure_options.size() != record.shared_evictions.size()) {
            throw std::logic_error("Program returned an incomplete shared-victim acknowledgement");
        }
        for (std::size_t index = 0; index < record.shared_evictions.size(); ++index) {
            const std::uint32_t slot  = record.shared_evictions[index];
            SharedCatalogEntry& entry = shared_catalog_[slot];
            if (entry.state != SharedCatalogState::Claimed || !entry.handle ||
                entry.id != record.shared_eviction_ids[index]) {
                throw std::logic_error(
                    "claimed shared materialization victim changed before adoption");
            }
            const auto& victim = result.shared_victims[index];
            const auto& option = record.shared_pressure_options[index];
            validate_pressure_acknowledgement(victim, option);
            if (victim.disposition == ClaimDisposition::Evicted) {
                if (!option.evicts_continuation || !option.shared_owner || victim.final_summary) {
                    throw std::logic_error(
                        "shared pressure claim returned an invalid eviction adoption");
                }
                saturating_increment(context_stats_.shared_checkpoint_evictions);
                clear_shared_catalog_entry(entry);
            } else if (victim.disposition == ClaimDisposition::Retained) {
                if (!victim.final_summary || !valid_shared_prefix_summary(*victim.final_summary) ||
                    (result.status == ContextTransactionStatus::Published &&
                     option.evicts_continuation)) {
                    throw std::logic_error(
                        "shared pressure claim returned an invalid retained adoption");
                }
                const bool changed = victim.resource_delta != ResourceDelta{};
                if (changed) {
                    saturating_increment(context_stats_.shared_checkpoint_degradations);
                    if (victim.resource_delta == option.effect) {
                        context_stats_.partial_spill_pages = saturating_add(
                            context_stats_.partial_spill_pages, pressure_spill_pages(option));
                    }
                }
                entry.state = SharedCatalogState::Catalogued;
                if (entry.summary != *victim.final_summary || changed) {
                    entry.summary = *victim.final_summary;
                    advance_revision(entry.revision);
                }
            } else {
                throw std::logic_error("shared pressure claim returned an invalid disposition");
            }
        }
        const auto restore_catalogued = [&](CatalogEntry& entry, std::uint32_t slot,
                                            const ContinuationSummary& summary,
                                            bool physical_change) {
            migrate_private_observations(entry, summary, entry.retention);
            entry.state = CatalogState::Catalogued;
            if (!summaries_equal(entry.summary, summary) || physical_change) {
                assign_continuation_summary(entry.summary, summary);
                advance_revision(entry.revision);
            }
            if (entry.session) {
                update_session_revision_if_equals(*entry.session, slot, entry.id, entry.revision);
            }
        };
        for (std::size_t index = 0; index < record.evictions.size(); ++index) {
            const std::uint32_t slot = record.evictions[index];
            CatalogEntry& entry      = catalog_[slot];
            if (entry.state != CatalogState::Claimed || !entry.handle ||
                entry.id != record.eviction_ids[index]) {
                throw std::logic_error("claimed materialization victim changed before adoption");
            }
            const auto& victim = result.victims[index];
            const auto& option = record.pressure_options[index];
            validate_pressure_acknowledgement(victim, option);
            if (victim.disposition == ClaimDisposition::Evicted) {
                if (!option.evicts_continuation || victim.final_summary) {
                    throw std::logic_error("pressure claim returned an invalid eviction adoption");
                }
                saturating_increment(context_stats_.private_checkpoint_evictions);
                clear_catalog_slot(slot);
            } else if (victim.disposition == ClaimDisposition::Retained) {
                if (!victim.final_summary || !valid_continuation_summary(*victim.final_summary) ||
                    (result.status == ContextTransactionStatus::Published &&
                     option.evicts_continuation)) {
                    throw std::logic_error("pressure claim returned an invalid retained adoption");
                }
                const bool changed = victim.resource_delta != ResourceDelta{};
                if (changed) {
                    saturating_increment(context_stats_.private_checkpoint_degradations);
                    if (victim.resource_delta == option.effect) {
                        context_stats_.partial_spill_pages = saturating_add(
                            context_stats_.partial_spill_pages, pressure_spill_pages(option));
                    }
                }
                restore_catalogued(entry, slot, *victim.final_summary, changed);
            } else {
                throw std::logic_error("pressure claim returned an invalid disposition");
            }
        }

        if ((record.source_slot != kInvalidCatalogSlot) != result.source.has_value()) {
            throw std::logic_error("Program returned an incomplete source acknowledgement");
        }
        if (result.source) {
            accumulate_owner_delta(result.source->resource_delta);
            CatalogEntry& source = catalog_[record.source_slot];
            const ClaimDisposition expected_disposition =
                result.status == ContextTransactionStatus::Aborted ? ClaimDisposition::Retained
                                                                   : record.source_disposition;
            if (source.state != CatalogState::Claimed || !source.handle ||
                source.id != record.source_id ||
                result.source->disposition != expected_disposition) {
                throw std::logic_error("materialization source acknowledgement is stale");
            }
            if (expected_disposition == ClaimDisposition::Retained) {
                if (!result.source->final_summary ||
                    !valid_continuation_summary(*result.source->final_summary)) {
                    throw std::logic_error("retained source returned an invalid summary");
                }
                restore_catalogued(source, record.source_slot, *result.source->final_summary,
                                   result.source->resource_delta != ResourceDelta{});
            } else if (expected_disposition != ClaimDisposition::ConsumedToActive ||
                       result.source->final_summary) {
                throw std::logic_error("consumed source acknowledgement is invalid");
            }
        }

        if ((record.shared_source_slot != kInvalidCatalogSlot) !=
            result.shared_source.has_value()) {
            throw std::logic_error("Program returned an incomplete shared-source acknowledgement");
        }
        if (result.shared_source) {
            SharedCatalogEntry& source  = shared_catalog_[record.shared_source_slot];
            const auto& acknowledgement = *result.shared_source;
            if (source.state != SharedCatalogState::Catalogued || !source.handle ||
                source.id != record.shared_source_id || source.transaction_pins == 0 ||
                acknowledgement.disposition != ClaimDisposition::Retained ||
                !acknowledgement.final_summary ||
                !valid_shared_prefix_summary(*acknowledgement.final_summary) ||
                acknowledgement.final_summary->checkpoint.ref != source.summary.checkpoint.ref) {
                throw std::logic_error("materialization shared-source acknowledgement is stale");
            }
            const std::uint64_t expected_references =
                static_cast<std::uint64_t>(source.summary.active_references) +
                (result.status == ContextTransactionStatus::Published ? 1U : 0U);
            if (expected_references > std::numeric_limits<std::uint32_t>::max() ||
                acknowledgement.final_summary->active_references != expected_references) {
                throw std::logic_error("materialization shared-source reference count diverged");
            }
            accumulate_owner_delta(acknowledgement.resource_delta);
            const bool changed =
                acknowledgement.resource_delta != ResourceDelta{} ||
                source.summary.checkpoint != acknowledgement.final_summary->checkpoint;
            source.summary = *acknowledgement.final_summary;
            if (changed) { advance_revision(source.revision); }
        }

        if (result.status == ContextTransactionStatus::Aborted) {
            if (result.published || acknowledged_owner_delta != result.resource_delta ||
                !ledger_.abort_materialization(record.destination, result.resource_delta)) {
                throw std::logic_error("Program returned an invalid aborted materialization");
            }
            if (record.shared_source_slot != kInvalidCatalogSlot) {
                SharedCatalogEntry& source = shared_catalog_[record.shared_source_slot];
                if (source.state != SharedCatalogState::Catalogued || !source.handle ||
                    source.id != record.shared_source_id || source.transaction_pins == 0) {
                    throw std::logic_error("aborted shared source pin is stale");
                }
                --source.transaction_pins;
            }
            observe_transfers(result);
            observe_operations(result);
            context_transaction_.template emplace<std::monostate>();
            program.finalize_context_transaction();
            return {.status = ContextTransactionStatus::Aborted};
        }
        if (result.status != ContextTransactionStatus::Published || !result.published) {
            throw std::logic_error("Program returned an invalid terminal materialization");
        }
        StartResult started = std::move(*result.published);
        result.published.reset();
        const std::uint64_t lineage_id =
            record.source_slot != kInvalidCatalogSlot &&
                    record.source_disposition == ClaimDisposition::ConsumedToActive
                ? record.source_id
                : next_continuation_id_++;
        MaterializationOutcome outcome{.status = ContextTransactionStatus::Published};
        observe_transfers(result);
        observe_operations(result);
        outcome.activation.emplace(PublishedActivation(
            *this, std::move(started), record.destination, record.demand, result.resource_delta,
            record.source_slot, record.source_disposition, record.shared_source_slot,
            record.publication_slot, lineage_id));
        return outcome;
    }

public:
    void adopt(Program& program, PublishedActivation&& activation) {
        MaterializationRecord* materialization =
            std::get_if<MaterializationRecord>(&context_transaction_);
        if (activation.owner_ != this || !activation.result_ || materialization == nullptr ||
            !program.has_context_transaction() || activation.destination_.value >= lane_count_ ||
            active_[activation.destination_.value].occupied ||
            activation.publication_slot_ >= catalog_count_ ||
            materialization->destination != activation.destination_ ||
            materialization->source_slot != activation.source_slot_ ||
            materialization->shared_source_slot != activation.shared_source_slot_ ||
            materialization->publication_slot != activation.publication_slot_) {
            throw std::logic_error("published activation token is stale");
        }
        const StartResult& result = *activation.result_;
        if (result.active_resources != activation.demand_.active_entitlement ||
            activation.terminal_delta_.removed != activation.demand_.final_removed ||
            activation.terminal_delta_.added != activation.demand_.final_added) {
            throw std::logic_error("published activation token has an invalid resource delta");
        }

        CatalogEntry& publication = catalog_[activation.publication_slot_];
        const bool consumes_source =
            activation.source_slot_ != kInvalidCatalogSlot &&
            activation.source_disposition_ == ClaimDisposition::ConsumedToActive;
        const bool retains_source = activation.source_slot_ != kInvalidCatalogSlot &&
                                    activation.source_disposition_ == ClaimDisposition::Retained;
        CatalogEntry* retained_source =
            retains_source ? &catalog_[activation.source_slot_] : nullptr;
        SharedCatalogEntry* shared_source = activation.shared_source_slot_ != kInvalidCatalogSlot
                                                ? &shared_catalog_[activation.shared_source_slot_]
                                                : nullptr;
        const CatalogState expected =
            consumes_source ? CatalogState::Claimed : CatalogState::Vacant;
        const bool source_has_handle = consumes_source;
        if (publication.state != expected || publication.handle.has_value() != source_has_handle ||
            (consumes_source && (activation.source_slot_ != activation.publication_slot_ ||
                                 publication.id != activation.continuation_id_)) ||
            (retains_source &&
             (retained_source == nullptr || retained_source->state != CatalogState::Catalogued ||
              !retained_source->handle || retained_source->id != materialization->source_id ||
              retained_source->active_references == std::numeric_limits<std::uint32_t>::max())) ||
            (shared_source != nullptr &&
             (shared_source->state != SharedCatalogState::Catalogued || !shared_source->handle ||
              shared_source->id != materialization->shared_source_id ||
              shared_source->transaction_pins == 0 ||
              shared_source->summary.active_references == 0 ||
              (shared_source->active_owner_mask & (1U << activation.destination_.value)) != 0)) ||
            !ledger_.complete_materialization(activation.destination_,
                                              activation.terminal_delta_)) {
            throw std::logic_error("published activation could not be adopted by the ledger");
        }

        publication.handle.reset();
        publication.state = CatalogState::ReservedForActive;
        publication.id    = activation.continuation_id_;
        publication.summary.endpoint.reset();
        publication.summary.rewrite.reset();
        publication.summary.long_anchors.clear();
        publication.active_references = 0;
        publication.session =
            materialization->update_session_index ? materialization->session : std::nullopt;
        publication.retention = materialization->retention;
        if (++publication.revision == 0) { ++publication.revision; }
        if (retained_source != nullptr) { ++retained_source->active_references; }
        if (shared_source != nullptr) {
            --shared_source->transaction_pins;
            shared_source->active_owner_mask |= 1U << activation.destination_.value;
        }
        active_[activation.destination_.value] = ActiveEntry{
            .occupied         = true,
            .publication_slot = activation.publication_slot_,
            .continuation_id  = activation.continuation_id_,
            .resources        = result.active_resources,
            .session   = materialization->update_session_index ? std::move(materialization->session)
                                                               : std::nullopt,
            .retention = materialization->retention,
            .update_session_index = materialization->update_session_index,
            .publish_continuation = materialization->publish_continuation,
            .retained_source_slot = retains_source ? activation.source_slot_ : kInvalidCatalogSlot,
            .retained_source_id   = retains_source ? materialization->source_id : 0,
        };
        if (consumes_source && publication.session &&
            active_[activation.destination_.value].update_session_index) {
            update_session_revision_if_equals(*publication.session, activation.publication_slot_,
                                              publication.id, publication.revision);
        }
        activation.result_.reset();
        activation.owner_ = nullptr;
        context_transaction_.template emplace<std::monostate>();
        program.finalize_context_transaction();
    }

    [[nodiscard]] ActiveCaptureReserveResult
    reserve_active_capture(Program& program, LaneId lane, CaptureOffer&& offer,
                           bool permit_transfer, CancellationFlagView cancellation) {
        if (!std::holds_alternative<std::monostate>(context_transaction_) ||
            program.has_context_transaction()) {
            throw std::logic_error("ResourceManager already owns a context transaction");
        }
        (void)require_active(lane);
        rebuild_prefix_indices();

        CaptureAssessment assessment =
            program.inspect_capture(offer, nullptr, nullptr, std::nullopt);
        const SharedPrefixHandle* exact_shared = nullptr;
        if (assessment.publishes_shared) {
            std::vector<std::uint32_t> candidate_slots;
            candidate_slots.reserve(shared_catalog_count_);
            for (const PrefixIndexEntry& index : prefix_index_) {
                if (index.shared && valid_prefix_index_entry(index) &&
                    index.key == assessment.shortlist_key) {
                    candidate_slots.push_back(index.slot);
                }
            }
            for (const std::uint32_t slot : candidate_slots) {
                const SharedCatalogEntry& entry = shared_catalog_[slot];
                if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                    entry.summary.checkpoint.shortlist_key != assessment.shortlist_key) {
                    continue;
                }
                if (program.shared_capture_matches(offer, *entry.handle)) {
                    exact_shared = &*entry.handle;
                    assessment =
                        program.inspect_capture(offer, exact_shared, nullptr, std::nullopt);
                    break;
                }
            }
        }

        if (!assessment.publishes_private && !assessment.publishes_shared) {
            program.skip_capture(std::move(offer));
            return ActiveCaptureReserveResult::Skipped;
        }

        std::vector<std::optional<CheckpointRef>> private_replacements;
        if (assessment.private_replacement_candidates.empty()) {
            private_replacements.emplace_back();
        } else {
            private_replacements.reserve(assessment.private_replacement_candidates.size());
            for (const CheckpointRef checkpoint : assessment.private_replacement_candidates) {
                private_replacements.emplace_back(checkpoint);
            }
        }

        std::vector<std::uint32_t> shared_publication_slots;
        if (!assessment.publishes_shared) {
            shared_publication_slots.push_back(kInvalidCatalogSlot);
        } else {
            for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
                if (shared_catalog_[slot].state == SharedCatalogState::Vacant) {
                    shared_publication_slots.push_back(slot);
                    break;
                }
            }
            if (shared_publication_slots.empty()) {
                for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
                    const SharedCatalogEntry& entry = shared_catalog_[slot];
                    if (entry.state == SharedCatalogState::Catalogued && entry.handle &&
                        entry.transaction_pins == 0 && entry.summary.active_references == 0) {
                        shared_publication_slots.push_back(slot);
                    }
                }
            }
        }
        if (shared_publication_slots.empty()) {
            program.skip_capture(std::move(offer));
            return ActiveCaptureReserveResult::Skipped;
        }

        const ActiveEntry& active = active_[lane.value];
        if (active.publication_slot >= catalog_count_) {
            throw std::logic_error("active capture has no private lineage descriptor");
        }
        const CatalogEntry& private_entry = catalog_[active.publication_slot];
        const ProtectedHeadResourceProjection current_projection =
            build_protected_projection(program, nullptr);
        const std::uint64_t current_protected_state_slots =
            projected_device_state_slots(current_projection);

        struct CaptureSelection {
            CaptureAssessment assessment;
            std::optional<CheckpointRef> private_replacement;
            std::uint32_t publication_slot = kInvalidCatalogSlot;
            std::uint64_t margin_ns        = 0;
            CostEstimate cost;
        };

        std::optional<CaptureSelection> selected;
        for (const std::optional<CheckpointRef> private_replacement : private_replacements) {
            for (const std::uint32_t publication_slot : shared_publication_slots) {
                const SharedCatalogEntry* shared_victim = nullptr;
                const SharedPrefixHandle* replacement   = nullptr;
                if (publication_slot != kInvalidCatalogSlot &&
                    shared_catalog_[publication_slot].state == SharedCatalogState::Catalogued) {
                    shared_victim = &shared_catalog_[publication_slot];
                    replacement   = &*shared_victim->handle;
                }
                CaptureAssessment candidate =
                    program.inspect_capture(offer, exact_shared, replacement, private_replacement);
                if ((!permit_transfer && candidate.needs_transfer) ||
                    (!candidate.publishes_private && !candidate.publishes_shared) ||
                    (!candidate.private_replacement_candidates.empty() && !private_replacement) ||
                    !ledger_.can_reserve_active_capture(lane, candidate.demand,
                                                        candidate.active_entitlement_delta) ||
                    !active_capture_state_headroom_preserved(current_protected_state_slots,
                                                             candidate)) {
                    continue;
                }

                CostEstimate saving;
                if (candidate.publishes_private) {
                    RetentionObservation observation{
                        .retention_class = active.retention,
                    };
                    add_cost(saving, weighted_cost(prefill_cost(candidate.protected_rebuild_work),
                                                   retention_probability_q16(observation)));
                }
                if (candidate.publishes_shared) {
                    RetentionObservation observation{
                        .retention_class = RetentionClass::SharedStable,
                    };
                    add_cost(saving, weighted_cost(prefill_cost(candidate.protected_rebuild_work),
                                                   retention_probability_q16(observation)));
                }

                CostEstimate cost = transfer_cost(candidate.transfer_requirements);
                add_cost(cost, checkpoint_impacts_cost(candidate.replacement_impacts,
                                                       &private_entry, nullptr));
                if (shared_victim != nullptr) {
                    typename Package::PressureOption eviction =
                        program.inspect_shared_eviction_option(*shared_victim->handle);
                    if (!eviction.evicts_continuation || !eviction.shared_owner) {
                        throw std::logic_error("Program shared capture victim is not evictable");
                    }
                    add_cost(cost, pressure_cost(eviction, nullptr, shared_victim));
                }

                const bool retain =
                    candidate.publishes_shared || active.retention != RetentionClass::Disposable;
                if (!retain) { continue; }

                std::uint64_t margin = 0;
                if (saving.nanoseconds > cost.nanoseconds) {
                    margin = saving.nanoseconds - cost.nanoseconds;
                }

                // Rolling private captures are the current request's retention observation and
                // must be allowed to advance.  A full shared catalog likewise must be able to
                // replace a prefix that has never produced a hit; otherwise its first entry owns a
                // one-slot catalog forever.  Once a shared prefix has demonstrated reuse, preserve
                // it unless the numerical value model prefers the new prefix.
                if (shared_victim != nullptr &&
                    shared_victim->observation.selected_hit_count != 0 && margin == 0) {
                    continue;
                }

                CaptureSelection choice{
                    .assessment          = std::move(candidate),
                    .private_replacement = private_replacement,
                    .publication_slot    = publication_slot,
                    .margin_ns           = margin,
                    .cost                = cost,
                };
                const auto tie_key = [&](const CaptureSelection& value) {
                    const CheckpointRef checkpoint =
                        value.private_replacement.value_or(CheckpointRef{});
                    const std::uint64_t victim_epoch =
                        value.publication_slot != kInvalidCatalogSlot &&
                                shared_catalog_[value.publication_slot].state ==
                                    SharedCatalogState::Catalogued
                            ? shared_catalog_[value.publication_slot].observation.last_hit_epoch
                            : 0;
                    return std::tuple{
                        std::numeric_limits<std::uint64_t>::max() - value.margin_ns,
                        tie_break_key(value.cost.tie_break),
                        victim_epoch,
                        checkpoint.kind,
                        checkpoint.frontier,
                        checkpoint.ordinal,
                        value.publication_slot,
                    };
                };
                if (!selected || tie_key(choice) < tie_key(*selected)) {
                    selected.emplace(std::move(choice));
                }
            }
        }
        if (!selected) {
            program.skip_capture(std::move(offer));
            return ActiveCaptureReserveResult::Skipped;
        }

        assessment                            = std::move(selected->assessment);
        const std::uint32_t publication_slot  = selected->publication_slot;
        const SharedPrefixHandle* replacement = nullptr;
        std::uint64_t replacement_id          = 0;
        std::uint64_t replacement_revision    = 0;
        if (publication_slot != kInvalidCatalogSlot &&
            shared_catalog_[publication_slot].state == SharedCatalogState::Catalogued) {
            const SharedCatalogEntry& victim = shared_catalog_[publication_slot];
            replacement                      = &*victim.handle;
            replacement_id                   = victim.id;
            replacement_revision             = victim.revision;
        }

        const runtime::ContextTransactionReserveStatus reserved =
            program.reserve_active_capture(std::move(offer), exact_shared, replacement,
                                           selected->private_replacement, cancellation);
        if (reserved == runtime::ContextTransactionReserveStatus::Aborted) {
            return ActiveCaptureReserveResult::Skipped;
        }
        if (!ledger_.reserve_active_capture(lane, assessment.demand,
                                            assessment.active_entitlement_delta)) {
            throw std::logic_error("active-capture reservation changed after inspection");
        }
        if (publication_slot != kInvalidCatalogSlot) {
            shared_catalog_[publication_slot].state = SharedCatalogState::ReservedCapture;
        }

        static_assert(std::is_nothrow_move_constructible_v<ActiveCaptureRecord>);
        context_transaction_.template emplace<ActiveCaptureRecord>(ActiveCaptureRecord{
            .lane                 = lane,
            .assessment           = std::move(assessment),
            .publication_slot     = publication_slot,
            .replacement_id       = replacement_id,
            .replacement_revision = replacement_revision,
        });
        return ActiveCaptureReserveResult::Reserved;
    }

private:
    [[nodiscard]] ActiveCaptureOutcome
    adopt_active_capture_progress(Program& program, ProgramActiveCaptureResult&& result) {
        ActiveCaptureRecord* record_ptr = std::get_if<ActiveCaptureRecord>(&context_transaction_);
        if (record_ptr == nullptr || !program.has_context_transaction()) {
            throw std::logic_error("ResourceManager has no progressable active capture");
        }
        ActiveCaptureRecord& record = *record_ptr;
        if (result.status == ContextTransactionStatus::Aborted) {
            const bool preparation_committed = result.capacity_preparation_committed;
            const ResourceDelta expected_delta{
                .removed = result.capacity_preparation_removed,
            };
            if ((preparation_committed && record.replacement_id == 0) ||
                (preparation_committed && result.capacity_preparation_removed !=
                                              record.assessment.capacity_preparation_removed) ||
                (!preparation_committed &&
                 result.capacity_preparation_removed != ResourceVector{}) ||
                result.resource_delta != expected_delta ||
                result.active_entitlement_delta != ResourceDelta{} || result.shared ||
                (preparation_committed ? !ledger_.complete_active_capture(
                                             record.lane, result.resource_delta, ResourceDelta{})
                                       : !ledger_.cancel_active_capture(record.lane))) {
                throw std::logic_error("aborted active capture violated its reservation");
            }
            if (record.publication_slot != kInvalidCatalogSlot) {
                SharedCatalogEntry& publication = shared_catalog_[record.publication_slot];
                if (record.replacement_id != 0) {
                    if (publication.state != SharedCatalogState::ReservedCapture ||
                        publication.id != record.replacement_id ||
                        publication.revision != record.replacement_revision) {
                        throw std::logic_error(
                            "aborted shared replacement changed before adoption");
                    }
                    if (preparation_committed) {
                        clear_shared_catalog_entry(publication);
                    } else {
                        publication.state = SharedCatalogState::Catalogued;
                    }
                } else {
                    clear_shared_catalog_entry(publication);
                }
            }
            observe_transfers(result);
            observe_operations(result);
            context_transaction_.template emplace<std::monostate>();
            program.finalize_context_transaction();
            return {.status = ContextTransactionStatus::Aborted};
        }
        if (result.status != ContextTransactionStatus::Published ||
            result.resource_delta.removed != record.assessment.demand.final_removed ||
            result.resource_delta.added != record.assessment.demand.final_added ||
            result.active_entitlement_delta != record.assessment.active_entitlement_delta ||
            result.capacity_preparation_removed != record.assessment.capacity_preparation_removed ||
            result.capacity_preparation_committed != (record.replacement_id != 0) ||
            result.shared.has_value() != record.assessment.publishes_shared) {
            throw std::logic_error("active capture publication violated its selected plan");
        }
        if (record.assessment.publishes_private &&
            !valid_continuation_summary(result.active_summary)) {
            throw std::logic_error("active capture returned an invalid private summary");
        }
        if (result.shared) {
            if (record.publication_slot >= shared_catalog_count_ ||
                !valid_shared_prefix_summary(result.shared->summary) ||
                result.shared->summary.active_references != 1) {
                throw std::logic_error("active capture returned an invalid shared publication");
            }
        }
        if (!ledger_.complete_active_capture(record.lane, result.resource_delta,
                                             result.active_entitlement_delta)) {
            throw std::logic_error("active capture could not be adopted by the ledger");
        }
        active_[record.lane.value].resources = ledger_.lane(record.lane).resources;
        if (record.assessment.publishes_private) {
            CatalogEntry& active_publication =
                catalog_[active_[record.lane.value].publication_slot];
            if (result.active_summary.long_anchors.size() >
                active_publication.summary.long_anchors.capacity()) {
                throw std::logic_error("active capture summary exceeds its reserved backing");
            }
            active_publication.summary.endpoint          = result.active_summary.endpoint;
            active_publication.summary.rewrite           = result.active_summary.rewrite;
            active_publication.summary.active_references = result.active_summary.active_references;
            active_publication.summary.long_anchors.clear();
            for (const auto& anchor : result.active_summary.long_anchors) {
                active_publication.summary.long_anchors.push_back(anchor);
            }
            migrate_private_observations(active_publication, result.active_summary,
                                         active_[record.lane.value].retention);
        }
        if (result.shared) {
            SharedCatalogEntry& publication = shared_catalog_[record.publication_slot];
            if (publication.state != SharedCatalogState::ReservedCapture ||
                (record.replacement_id != 0 &&
                 (publication.id != record.replacement_id ||
                  publication.revision != record.replacement_revision)) ||
                (record.replacement_id == 0 && publication.handle)) {
                throw std::logic_error("active capture shared descriptor changed before adoption");
            }
            publication.handle.reset();
            publication.state   = SharedCatalogState::Catalogued;
            publication.id      = next_shared_prefix_id_++;
            publication.summary = result.shared->summary;
            publication.handle.emplace(std::move(result.shared->handle));
            publication.observation =
                RetentionObservation{.retention_class = RetentionClass::SharedStable};
            publication.active_owner_mask = 1U << record.lane.value;
            if (++publication.revision == 0) { ++publication.revision; }
        }
        observe_transfers(result);
        observe_operations(result);
        context_transaction_.template emplace<std::monostate>();
        program.finalize_context_transaction();
        return {.status = ContextTransactionStatus::Published};
    }

public:
    [[nodiscard]] ReplicaTransitionReserveResult reserve_replica_transition(Program& program) {
        if (!std::holds_alternative<std::monostate>(context_transaction_) ||
            program.has_context_transaction()) {
            throw std::logic_error("ResourceManager already owns a context transaction");
        }
        if (!replica_policy_pending()) { return ReplicaTransitionReserveResult::Skipped; }
        const std::uint64_t policy_generation = replica_policy_generation_;
        evaluated_replica_policy_generation_  = policy_generation;

        struct Candidate {
            std::uint64_t policy_generation = 0;
            bool shared                     = false;
            std::uint32_t slot              = kInvalidCatalogSlot;
            std::uint64_t owner_id          = 0;
            std::uint64_t revision          = 0;
            ReplicaTransitionOption option;
            bool replacement_shared            = false;
            std::uint32_t replacement_slot     = kInvalidCatalogSlot;
            std::uint64_t replacement_owner_id = 0;
            std::uint64_t replacement_revision = 0;
            std::optional<typename Package::PressureOption> replacement;
            ResourceDemand demand;
            std::uint64_t backup_transfer_ns = 0;
            std::uint64_t margin_ns          = 0;
            std::uint32_t retention_priority = 0;
            std::uint64_t last_hit_epoch     = 0;
        };

        std::optional<Candidate> selected;
        const auto consider = [&](Candidate candidate) {
            const auto key =
                std::tuple{std::numeric_limits<std::uint64_t>::max() - candidate.margin_ns,
                           std::numeric_limits<std::uint32_t>::max() - candidate.retention_priority,
                           std::numeric_limits<std::uint64_t>::max() - candidate.last_hit_epoch,
                           candidate.shared,
                           candidate.owner_id,
                           candidate.option.checkpoint.kind,
                           candidate.option.checkpoint.frontier,
                           candidate.option.checkpoint.ordinal,
                           candidate.slot,
                           candidate.replacement.has_value(),
                           candidate.replacement_shared,
                           candidate.replacement_owner_id,
                           candidate.replacement_slot};
            if (!selected) {
                selected.emplace(std::move(candidate));
                return;
            }
            const auto current_key =
                std::tuple{std::numeric_limits<std::uint64_t>::max() - selected->margin_ns,
                           std::numeric_limits<std::uint32_t>::max() - selected->retention_priority,
                           std::numeric_limits<std::uint64_t>::max() - selected->last_hit_epoch,
                           selected->shared,
                           selected->owner_id,
                           selected->option.checkpoint.kind,
                           selected->option.checkpoint.frontier,
                           selected->option.checkpoint.ordinal,
                           selected->slot,
                           selected->replacement.has_value(),
                           selected->replacement_shared,
                           selected->replacement_owner_id,
                           selected->replacement_slot};
            if (key < current_key) { selected.emplace(std::move(candidate)); }
        };

        const auto pure_host_release = [](const auto& option,
                                          const ReplicaTransitionOption& target) {
            if (option.evicts_continuation || option.dropped_checkpoint ||
                option.effect.added != ResourceVector{} ||
                option.effect.removed.device != DeviceResources{} ||
                option.removed_host_replica_impacts.empty()) {
                return false;
            }
            return target.resource == ContextResourceClass::State
                       ? option.effect.removed.host.state_slots != 0 &&
                             option.effect.removed.host.kv_bytes == 0
                       : option.effect.removed.host.state_slots == 0 &&
                             option.effect.removed.host.kv_bytes != 0;
        };
        const auto evaluate_target = [&](Candidate target, const CatalogEntry* private_entry,
                                         const SharedCatalogEntry* shared_entry,
                                         const ContinuationHandle* private_owner,
                                         const SharedPrefixHandle* shared_owner) {
            target.backup_transfer_ns       = price_replica_option(target.option);
            target.policy_generation        = policy_generation;
            const CostEstimate target_value = replica_value_cost(
                target.option.added_host_replica_impacts, private_entry, shared_entry);
            if (target_value.nanoseconds <= target.backup_transfer_ns) { return; }
            target.demand = ResourceDemand{
                .reservation_added        = target.option.effect.added,
                .physical_peak_additional = target.option.effect.added,
                .final_added              = target.option.effect.added,
            };
            const auto try_candidate = [&](Candidate candidate,
                                           const ContinuationHandle* private_replacement,
                                           const SharedPrefixHandle* shared_replacement,
                                           std::uint64_t replacement_loss) -> bool {
                if (replacement_loss >
                    std::numeric_limits<std::uint64_t>::max() - candidate.backup_transfer_ns) {
                    return false;
                }
                const std::uint64_t cost = candidate.backup_transfer_ns + replacement_loss;
                if (target_value.nanoseconds <= cost ||
                    !detail::demand_fits(ledger_.used(), candidate.demand, ledger_.capacity())) {
                    return false;
                }
                const PreflightStatus preflight = program.revalidate_replica_transition(
                    private_owner, shared_owner, candidate.option, private_replacement,
                    shared_replacement, candidate.replacement ? &*candidate.replacement : nullptr);
                if (preflight == PreflightStatus::InvariantFailure) {
                    throw std::logic_error("Program rejected a valid replica-transition shape");
                }
                if (preflight != PreflightStatus::Ready) { return false; }
                candidate.margin_ns = target_value.nanoseconds - cost;
                consider(std::move(candidate));
                return true;
            };

            if (try_candidate(target, nullptr, nullptr, 0)) { return; }
            const ResourceVector deficit = target.option.effect.added;
            for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
                const CatalogEntry& entry = catalog_[slot];
                if (entry.state != CatalogState::Catalogued || !entry.handle ||
                    entry.active_references != 0) {
                    continue;
                }
                std::vector<typename Package::PressureOption> options =
                    program.inspect_pressure_options(*entry.handle, deficit);
                for (typename Package::PressureOption& option : options) {
                    if (!pure_host_release(option, target.option)) { continue; }
                    const CostEstimate loss =
                        replica_value_cost(option.removed_host_replica_impacts, &entry, nullptr);
                    Candidate candidate            = target;
                    candidate.replacement_shared   = false;
                    candidate.replacement_slot     = slot;
                    candidate.replacement_owner_id = entry.id;
                    candidate.replacement_revision = entry.revision;
                    candidate.replacement          = option;
                    if (!detail::augment_demand(candidate.demand, option.effect)) { continue; }
                    try_candidate(std::move(candidate), &*entry.handle, nullptr, loss.nanoseconds);
                }
            }
            for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
                const SharedCatalogEntry& entry = shared_catalog_[slot];
                if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                    entry.transaction_pins != 0 || entry.summary.active_references != 0) {
                    continue;
                }
                std::vector<typename Package::PressureOption> options =
                    program.inspect_shared_pressure_options(*entry.handle, deficit);
                for (typename Package::PressureOption& option : options) {
                    if (!pure_host_release(option, target.option)) { continue; }
                    const CostEstimate loss =
                        replica_value_cost(option.removed_host_replica_impacts, nullptr, &entry);
                    Candidate candidate            = target;
                    candidate.replacement_shared   = true;
                    candidate.replacement_slot     = slot;
                    candidate.replacement_owner_id = entry.id;
                    candidate.replacement_revision = entry.revision;
                    candidate.replacement          = option;
                    if (!detail::augment_demand(candidate.demand, option.effect)) { continue; }
                    try_candidate(std::move(candidate), nullptr, &*entry.handle, loss.nanoseconds);
                }
            }
        };

        for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
            const CatalogEntry& entry = catalog_[slot];
            if (entry.state != CatalogState::Catalogued || !entry.handle ||
                entry.active_references != 0) {
                continue;
            }
            for (const CheckpointObservation& checkpoint : entry.observations) {
                if (!contains_checkpoint(entry.summary, checkpoint.checkpoint)) { continue; }
                std::optional<ReplicaTransitionOption> option =
                    program.inspect_replica_transition(*entry.handle, checkpoint.checkpoint);
                if (!option || option->shared_owner || option->effect.removed != ResourceVector{}) {
                    continue;
                }
                evaluate_target(
                    Candidate{
                        .shared             = false,
                        .slot               = slot,
                        .owner_id           = entry.id,
                        .revision           = entry.revision,
                        .option             = std::move(*option),
                        .retention_priority = eviction_priority(entry.retention),
                        .last_hit_epoch     = checkpoint.observation.last_hit_epoch,
                    },
                    &entry, nullptr, &*entry.handle, nullptr);
            }
        }
        for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
            const SharedCatalogEntry& entry = shared_catalog_[slot];
            if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                entry.transaction_pins != 0 || entry.summary.active_references != 0) {
                continue;
            }
            std::optional<ReplicaTransitionOption> option =
                program.inspect_replica_transition(*entry.handle);
            if (!option || !option->shared_owner || option->effect.removed != ResourceVector{}) {
                continue;
            }
            evaluate_target(
                Candidate{
                    .shared             = true,
                    .slot               = slot,
                    .owner_id           = entry.id,
                    .revision           = entry.revision,
                    .option             = std::move(*option),
                    .retention_priority = eviction_priority(RetentionClass::SharedStable),
                    .last_hit_epoch     = entry.observation.last_hit_epoch,
                },
                nullptr, &entry, nullptr, &*entry.handle);
        }
        if (!selected) { return ReplicaTransitionReserveResult::Skipped; }
        if (selected->policy_generation != replica_policy_generation_) {
            return ReplicaTransitionReserveResult::Skipped;
        }

        ContinuationHandle* private_owner =
            selected->shared ? nullptr : &*catalog_[selected->slot].handle;
        SharedPrefixHandle* shared_owner =
            selected->shared ? &*shared_catalog_[selected->slot].handle : nullptr;
        ContinuationHandle* private_replacement = nullptr;
        SharedPrefixHandle* shared_replacement  = nullptr;
        if (selected->replacement) {
            if (selected->replacement_shared) {
                shared_replacement = &*shared_catalog_[selected->replacement_slot].handle;
            } else {
                private_replacement = &*catalog_[selected->replacement_slot].handle;
            }
        }
        if (!ledger_.can_reserve_replica_transition(selected->demand)) {
            return ReplicaTransitionReserveResult::Skipped;
        }
        if (selected->shared) {
            const SharedCatalogEntry& entry = shared_catalog_[selected->slot];
            if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                entry.id != selected->owner_id || entry.revision != selected->revision ||
                entry.transaction_pins != 0 || entry.summary.active_references != 0) {
                return ReplicaTransitionReserveResult::Skipped;
            }
        } else {
            const CatalogEntry& entry = catalog_[selected->slot];
            if (entry.state != CatalogState::Catalogued || !entry.handle ||
                entry.id != selected->owner_id || entry.revision != selected->revision ||
                entry.active_references != 0) {
                return ReplicaTransitionReserveResult::Skipped;
            }
        }
        const bool same_owner = selected->replacement &&
                                selected->replacement_shared == selected->shared &&
                                selected->replacement_slot == selected->slot;
        if (selected->replacement && !same_owner) {
            if (selected->replacement_shared) {
                const SharedCatalogEntry& entry = shared_catalog_[selected->replacement_slot];
                if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                    entry.id != selected->replacement_owner_id ||
                    entry.revision != selected->replacement_revision ||
                    entry.transaction_pins != 0 || entry.summary.active_references != 0) {
                    return ReplicaTransitionReserveResult::Skipped;
                }
            } else {
                const CatalogEntry& entry = catalog_[selected->replacement_slot];
                if (entry.state != CatalogState::Catalogued || !entry.handle ||
                    entry.id != selected->replacement_owner_id ||
                    entry.revision != selected->replacement_revision ||
                    entry.active_references != 0) {
                    return ReplicaTransitionReserveResult::Skipped;
                }
            }
        }

        const runtime::ContextTransactionReserveStatus reserved =
            program.reserve_prevalidated_replica_transition(
                private_owner, shared_owner, selected->option, private_replacement,
                shared_replacement, selected->replacement, CancellationFlagView{});
        if (reserved == runtime::ContextTransactionReserveStatus::Aborted) {
            return ReplicaTransitionReserveResult::Skipped;
        }

        static_assert(std::is_nothrow_move_constructible_v<ReplicaTransitionRecord>);
        ReplicaTransitionRecord record{
            .shared               = selected->shared,
            .slot                 = selected->slot,
            .owner_id             = selected->owner_id,
            .revision             = selected->revision,
            .option               = std::move(selected->option),
            .replacement_shared   = selected->replacement_shared,
            .replacement_slot     = selected->replacement_slot,
            .replacement_owner_id = selected->replacement_owner_id,
            .replacement_revision = selected->replacement_revision,
            .replacement          = std::move(selected->replacement),
            .demand               = selected->demand,
        };

        // Program owns the prepared transition before ResourceManager adopts its logical claims.
        // Adoption below is allocation-free and cannot be rolled back independently.
        if (!ledger_.reserve_replica_transition(record.demand)) {
            throw std::logic_error("replica-transition reservation changed after inspection");
        }
        if (record.shared) {
            shared_catalog_[record.slot].state = SharedCatalogState::Claimed;
        } else {
            catalog_[record.slot].state = CatalogState::Claimed;
        }
        if (record.replacement && !same_owner) {
            if (record.replacement_shared) {
                shared_catalog_[record.replacement_slot].state = SharedCatalogState::Claimed;
            } else {
                catalog_[record.replacement_slot].state = CatalogState::Claimed;
            }
        }
        context_transaction_.template emplace<ReplicaTransitionRecord>(std::move(record));
        return ReplicaTransitionReserveResult::Reserved;
    }

private:
    [[nodiscard]] ReplicaTransitionOutcome
    adopt_replica_transition_progress(Program& program, ProgramReplicaTransitionResult&& result) {
        ReplicaTransitionRecord* record_ptr =
            std::get_if<ReplicaTransitionRecord>(&context_transaction_);
        if (record_ptr == nullptr || !program.has_context_transaction()) {
            throw std::logic_error("ResourceManager has no progressable replica transition");
        }
        ReplicaTransitionRecord& record = *record_ptr;
        const bool target_committed     = result.resource_delta.added == record.option.effect.added;
        const bool target_unmodified    = result.resource_delta.added == ResourceVector{};
        const ResourceVector replacement_removed =
            record.replacement ? record.replacement->effect.removed : ResourceVector{};
        const bool replacement_committed  = result.resource_delta.removed == replacement_removed;
        const bool replacement_unmodified = result.resource_delta.removed == ResourceVector{};
        const bool same_owner = record.replacement && record.replacement_shared == record.shared &&
                                record.replacement_slot == record.slot;
        const std::size_t expected_owner_count = record.replacement && !same_owner ? 2U : 1U;
        if ((result.status != ContextTransactionStatus::Published &&
             result.status != ContextTransactionStatus::Aborted) ||
            (!target_committed && !target_unmodified) ||
            (!replacement_committed && !replacement_unmodified) ||
            (target_committed && record.replacement && !replacement_committed) ||
            (result.status == ContextTransactionStatus::Published &&
             (!target_committed || (record.replacement && !replacement_committed))) ||
            result.owner_count != expected_owner_count) {
            throw std::logic_error("replica transition violated its selected resource delta");
        }
        ResourceDelta expected_target_delta;
        if (target_committed) { expected_target_delta.added = record.option.effect.added; }
        ResourceDelta expected_replacement_delta;
        if (record.replacement && replacement_committed) {
            expected_replacement_delta.removed = record.replacement->effect.removed;
        }
        if (same_owner) {
            if (!detail::add_resource_deltas(expected_target_delta, expected_replacement_delta,
                                             expected_target_delta)) {
                throw std::logic_error("replica-transition owner delta overflowed");
            }
        }
        const auto validate_owner = [&](const auto& owner, bool shared, std::uint32_t slot,
                                        std::uint64_t owner_id, std::uint64_t revision,
                                        const ResourceDelta& expected_delta) {
            if (owner.shared_owner != shared || owner.resource_delta != expected_delta ||
                owner.private_summary.has_value() == shared ||
                owner.shared_summary.has_value() != shared) {
                throw std::logic_error("replica-transition owner acknowledgement is invalid");
            }
            if (shared) {
                const SharedCatalogEntry& entry = shared_catalog_[slot];
                if (entry.state != SharedCatalogState::Claimed || !entry.handle ||
                    entry.id != owner_id || entry.revision != revision ||
                    !valid_shared_prefix_summary(*owner.shared_summary)) {
                    throw std::logic_error("replica-transition shared adoption is stale");
                }
                return;
            }
            const CatalogEntry& entry = catalog_[slot];
            if (entry.state != CatalogState::Claimed || !entry.handle || entry.id != owner_id ||
                entry.revision != revision || !valid_continuation_summary(*owner.private_summary)) {
                throw std::logic_error("replica-transition private adoption is stale");
            }
        };
        validate_owner(result.owners[0], record.shared, record.slot, record.owner_id,
                       record.revision, expected_target_delta);
        if (record.replacement && !same_owner) {
            validate_owner(result.owners[1], record.replacement_shared, record.replacement_slot,
                           record.replacement_owner_id, record.replacement_revision,
                           expected_replacement_delta);
        }
        if (!ledger_.complete_replica_transition(result.resource_delta)) {
            throw std::logic_error("replica transition could not be adopted by the ledger");
        }

        const auto adopt_owner = [&](const auto& owner, bool shared, std::uint32_t slot) {
            if (shared) {
                SharedCatalogEntry& entry = shared_catalog_[slot];
                if (entry.summary != *owner.shared_summary ||
                    owner.resource_delta != ResourceDelta{}) {
                    entry.summary = *owner.shared_summary;
                    advance_revision(entry.revision);
                }
                entry.state = SharedCatalogState::Catalogued;
                return;
            }
            CatalogEntry& entry = catalog_[slot];
            migrate_private_observations(entry, *owner.private_summary, entry.retention);
            if (!summaries_equal(entry.summary, *owner.private_summary) ||
                owner.resource_delta != ResourceDelta{}) {
                assign_continuation_summary(entry.summary, *owner.private_summary);
                advance_revision(entry.revision);
                if (entry.session) {
                    update_session_revision_if_equals(*entry.session, slot, entry.id,
                                                      entry.revision);
                }
            }
            entry.state = CatalogState::Catalogued;
        };
        adopt_owner(result.owners[0], record.shared, record.slot);
        if (record.replacement && !same_owner) {
            adopt_owner(result.owners[1], record.replacement_shared, record.replacement_slot);
        }
        if (record.replacement && replacement_committed) {
            if (record.replacement_shared) {
                saturating_increment(context_stats_.shared_checkpoint_degradations);
            } else {
                saturating_increment(context_stats_.private_checkpoint_degradations);
            }
        }
        const ContextTransactionStatus status = result.status;
        observe_transfers(result);
        context_transaction_.template emplace<std::monostate>();
        program.finalize_context_transaction();
        return {.status = status};
    }

public:
    [[nodiscard]] ContextTransactionOutcome
    progress_context_transaction(Program& program, CancellationFlagView cancellation) {
        if (std::holds_alternative<std::monostate>(context_transaction_) ||
            !program.has_context_transaction()) {
            throw std::logic_error("ResourceManager has no progressable context transaction");
        }
        ProgramContextTransactionProgress progress =
            program.progress_context_transaction(cancellation);
        ContextTransactionOutcome outcome = std::visit(
            [&](auto&& result) -> ContextTransactionOutcome {
                using Result = std::decay_t<decltype(result)>;
                if constexpr (std::is_same_v<Result, ContextTransactionInProgress>) {
                    return ContextTransactionInProgress{};
                } else if constexpr (std::is_same_v<Result, ProgramMaterializationResult>) {
                    if (!std::holds_alternative<MaterializationRecord>(context_transaction_)) {
                        throw std::logic_error(
                            "Program materialization result does not match ResourceManager");
                    }
                    return adopt_materialization_progress(program, std::move(result));
                } else if constexpr (std::is_same_v<Result, ProgramActiveCaptureResult>) {
                    if (!std::holds_alternative<ActiveCaptureRecord>(context_transaction_)) {
                        throw std::logic_error(
                            "Program active-capture result does not match ResourceManager");
                    }
                    return adopt_active_capture_progress(program, std::move(result));
                } else {
                    static_assert(std::is_same_v<Result, ProgramReplicaTransitionResult>);
                    if (!std::holds_alternative<ReplicaTransitionRecord>(context_transaction_)) {
                        throw std::logic_error(
                            "Program replica-transition result does not match ResourceManager");
                    }
                    return adopt_replica_transition_progress(program, std::move(result));
                }
            },
            std::move(progress));
        if (!std::holds_alternative<ContextTransactionInProgress>(outcome)) {
            invalidate_replica_policy();
        }
        return outcome;
    }

    [[nodiscard]] FinishResult finish(Program& program, LaneId lane, SequenceHandle sequence) {
        const ResourceVector active = require_active(lane);
        ActiveEntry& active_entry   = active_[lane.value];
        FinishResult result         = program.finish(sequence);
        if (result.status != ConsumeStatus::Consumed || result.resource_delta.removed != active ||
            result.resource_delta.added.device.active_lanes != 0) {
            throw std::logic_error("Runtime finish did not consume the active entitlement");
        }
        CatalogEntry& publication = catalog_[active_entry.publication_slot];
        if (!cache_enabled_ || !active_entry.publish_continuation) {
            if (result.disposition != FinishDisposition::Released || result.continuation ||
                result.resource_delta.added != ResourceVector{} ||
                !release_retained_source(active_entry) ||
                !ledger_.complete_active(lane, active, result.resource_delta)) {
                throw std::logic_error("released finish violated the active ledger");
            }
            release_shared_active_references(lane);
            clear_catalog_slot(active_entry.publication_slot);
            active_entry = {};
            invalidate_replica_policy();
            return result;
        }
        const bool valid_summary = valid_continuation_summary(result.summary);
        if (result.disposition != FinishDisposition::Catalogued || !result.continuation ||
            !valid_summary || !continuation_within_active(result.resource_delta.added, active) ||
            publication.state != CatalogState::ReservedForActive ||
            publication.id != active_entry.continuation_id) {
            if (result.continuation) {
                (void)program.release_continuation(std::move(*result.continuation));
                result.continuation.reset();
            }
            throw std::logic_error("Runtime continuation publication violated the ledger");
        }
        release_shared_active_references(lane);
        if (!release_retained_source(active_entry) ||
            !ledger_.complete_active(lane, active, result.resource_delta)) {
            (void)program.release_continuation(std::move(*result.continuation));
            result.continuation.reset();
            throw std::logic_error("continuation occupancy could not be adopted by the ledger");
        }
        std::optional<RetentionObservation> inherited_session_endpoint;
        if (active_entry.session && active_entry.update_session_index) {
            const std::optional<SessionIndexEntry> previous =
                session_binding(*active_entry.session);
            if (previous && previous->slot < catalog_count_ &&
                (previous->slot != active_entry.publication_slot ||
                 previous->owner_id != active_entry.continuation_id)) {
                const CatalogEntry& prior = catalog_[previous->slot];
                if (const RetentionObservation* observation = find_kind_observation(
                        prior.observations, CheckpointKind::SessionEndpoint)) {
                    inherited_session_endpoint = *observation;
                }
            }
        }
        publication.state = CatalogState::Catalogued;
        migrate_private_observations(publication, result.summary, active_entry.retention,
                                     inherited_session_endpoint ? &*inherited_session_endpoint
                                                                : nullptr);
        assign_continuation_summary(publication.summary, result.summary);
        publication.handle.emplace(std::move(*result.continuation));
        publication.session   = active_entry.session;
        publication.retention = active_entry.retention;
        result.continuation.reset();
        advance_revision(publication.revision);
        if (publication.session && active_entry.update_session_index) {
            const std::optional<SessionIndexEntry> previous =
                exchange_session(*publication.session, active_entry.publication_slot,
                                 publication.id, publication.revision);
            if (previous) {
                release_replaced_session(program, *previous, active_entry.publication_slot,
                                         publication.id);
            }
        }
        active_entry = {};
        invalidate_replica_policy();
        return result;
    }

    [[nodiscard]] AbortResult abort(Program& program, LaneId lane, SequenceHandle sequence) {
        const ResourceVector active = require_active(lane);
        AbortResult result          = program.abort(sequence);
        if (result.status != ConsumeStatus::Consumed ||
            result.resource_delta != ResourceDelta{.removed = active}) {
            throw std::logic_error("Runtime abort violated the resource ledger");
        }
        release_shared_active_references(lane);
        if (!release_retained_source(active_[lane.value]) ||
            !ledger_.complete_active(lane, active, result.resource_delta)) {
            throw std::logic_error("Runtime abort violated the resource ledger");
        }
        clear_catalog_slot(active_[lane.value].publication_slot);
        active_[lane.value] = {};
        invalidate_replica_policy();
        return result;
    }

    void apply_commit(std::span<const LaneId> lanes, const typename Package::CommitResult& result) {
        if (lanes.size() != result.row_count) {
            throw std::logic_error("commit result membership is not row aligned");
        }
        for (std::size_t row = 0; row < lanes.size(); ++row) {
            const ResourceVector active = require_active(lanes[row]);
            if (result.rows[row].disposition == CommitDisposition::CancelledReleased &&
                result.rows[row].resource_delta != ResourceDelta{.removed = active}) {
                throw std::logic_error("cancelled commit did not release its active entitlement");
            }
        }
        bool released = false;
        for (std::size_t row = 0; row < lanes.size(); ++row) {
            if (result.rows[row].disposition != CommitDisposition::CancelledReleased) { continue; }
            if (!release_cancelled_lane(lanes[row])) {
                throw std::logic_error("cancelled commit ownership could not be released");
            }
            released = true;
        }
        if (released) { invalidate_replica_policy(); }
    }

    void apply_discard(std::span<const LaneId> lanes,
                       const typename Package::DiscardResult& result) {
        if (lanes.size() != result.row_count || result.status != ConsumeStatus::Consumed) {
            throw std::logic_error("pending discard did not consume its membership");
        }
        for (std::size_t row = 0; row < lanes.size(); ++row) {
            if (result.resource_deltas[row] !=
                ResourceDelta{.removed = require_active(lanes[row])}) {
                throw std::logic_error("pending discard release acknowledgement is invalid");
            }
        }
        for (const LaneId lane : lanes) {
            if (!release_cancelled_lane(lane)) {
                throw std::logic_error("discarded pending ownership could not be released");
            }
        }
        if (!lanes.empty()) { invalidate_replica_policy(); }
    }

    void release_failed_commit(std::span<const LaneId> lanes) noexcept {
        bool released = false;
        for (const LaneId lane : lanes) {
            if (lane.value >= lane_count_ || ledger_.lane(lane).state != LogicalLaneState::Active) {
                continue;
            }
            released = release_cancelled_lane(lane) || released;
        }
        if (released) { invalidate_replica_policy(); }
    }

    [[nodiscard]] const ResourceLedger& ledger() const noexcept { return ledger_; }

    [[nodiscard]] bool has_replica_transition() const noexcept {
        return std::holds_alternative<ReplicaTransitionRecord>(context_transaction_);
    }

    [[nodiscard]] bool replica_policy_pending() const noexcept {
        return evaluated_replica_policy_generation_ != replica_policy_generation_;
    }

    [[nodiscard]] std::optional<ContextTransactionKind> context_transaction_kind() const noexcept {
        return std::visit(
            [](const auto& transaction) -> std::optional<ContextTransactionKind> {
                using Transaction = std::decay_t<decltype(transaction)>;
                if constexpr (std::is_same_v<Transaction, std::monostate>) {
                    return std::nullopt;
                } else if constexpr (std::is_same_v<Transaction, MaterializationRecord>) {
                    return ContextTransactionKind::Materialization;
                } else if constexpr (std::is_same_v<Transaction, ActiveCaptureRecord>) {
                    return ContextTransactionKind::ActiveCapture;
                } else {
                    return ContextTransactionKind::ReplicaTransition;
                }
            },
            context_transaction_);
    }

    void populate_runtime_stats(RuntimeStats& out) const noexcept {
        out.state_moves                       = context_stats_.state_moves;
        out.state_forks                       = context_stats_.state_forks;
        out.state_restores                    = context_stats_.state_restores;
        out.state_d2h_count                   = context_stats_.state_d2h_count;
        out.state_h2d_count                   = context_stats_.state_h2d_count;
        out.state_d2d_count                   = context_stats_.state_d2d_count;
        out.state_d2h_bytes                   = context_stats_.state_d2h_bytes;
        out.state_h2d_bytes                   = context_stats_.state_h2d_bytes;
        out.state_d2d_bytes                   = context_stats_.state_d2d_bytes;
        out.state_d2h_seconds                 = context_stats_.state_d2h_seconds;
        out.state_h2d_seconds                 = context_stats_.state_h2d_seconds;
        out.state_d2d_seconds                 = context_stats_.state_d2d_seconds;
        out.main_kv_d2h_pages                 = context_stats_.main_kv_d2h_pages;
        out.main_kv_h2d_pages                 = context_stats_.main_kv_h2d_pages;
        out.main_kv_d2d_pages                 = context_stats_.main_kv_d2d_pages;
        out.main_kv_d2h_bytes                 = context_stats_.main_kv_d2h_bytes;
        out.main_kv_h2d_bytes                 = context_stats_.main_kv_h2d_bytes;
        out.main_kv_d2d_bytes                 = context_stats_.main_kv_d2d_bytes;
        out.main_kv_d2h_seconds               = context_stats_.main_kv_d2h_seconds;
        out.main_kv_h2d_seconds               = context_stats_.main_kv_h2d_seconds;
        out.main_kv_d2d_seconds               = context_stats_.main_kv_d2d_seconds;
        out.backend_kv_d2h_pages              = context_stats_.backend_kv_d2h_pages;
        out.backend_kv_h2d_pages              = context_stats_.backend_kv_h2d_pages;
        out.backend_kv_d2d_pages              = context_stats_.backend_kv_d2d_pages;
        out.backend_kv_d2h_bytes              = context_stats_.backend_kv_d2h_bytes;
        out.backend_kv_h2d_bytes              = context_stats_.backend_kv_h2d_bytes;
        out.backend_kv_d2d_bytes              = context_stats_.backend_kv_d2d_bytes;
        out.backend_kv_d2h_seconds            = context_stats_.backend_kv_d2h_seconds;
        out.backend_kv_h2d_seconds            = context_stats_.backend_kv_h2d_seconds;
        out.backend_kv_d2d_seconds            = context_stats_.backend_kv_d2d_seconds;
        out.partial_spill_pages               = context_stats_.partial_spill_pages;
        out.partial_tail_cow_pages            = context_stats_.partial_tail_cow_pages;
        out.private_checkpoint_degradations   = context_stats_.private_checkpoint_degradations;
        out.private_checkpoint_evictions      = context_stats_.private_checkpoint_evictions;
        out.shared_checkpoint_degradations    = context_stats_.shared_checkpoint_degradations;
        out.shared_checkpoint_evictions       = context_stats_.shared_checkpoint_evictions;
        out.historical_fork_hits              = context_stats_.historical_fork_hits;
        out.last_predicted_materialization_ns = context_stats_.last_predicted_materialization_ns;
        out.actual_context_transfer_seconds   = context_stats_.actual_context_transfer_seconds;

        const ResourceVector used            = ledger_.used();
        out.device_state_occupied_slots      = used.device.state_slots;
        out.host_state_occupied_slots        = used.host.state_slots;
        out.device_main_kv_occupied_pages    = used.device.main_kv_pages;
        out.device_backend_kv_occupied_pages = used.device.backend_kv_pages;
        out.host_kv_occupied_bytes           = used.host.kv_bytes;
        std::uint32_t private_occupied = 0;
        for (const CatalogEntry& entry : catalog_) {
            if (entry.state != CatalogState::Vacant) { ++private_occupied; }
        }
        std::uint32_t shared_occupied  = 0;
        std::uint64_t shared_references = 0;
        for (const SharedCatalogEntry& entry : shared_catalog_) {
            if (entry.state != SharedCatalogState::Vacant) { ++shared_occupied; }
            if (entry.state == SharedCatalogState::Catalogued) {
                shared_references += entry.summary.active_references;
            }
        }
        out.private_catalog_occupied = private_occupied;
        out.shared_catalog_occupied  = shared_occupied;
        out.shared_active_references = shared_references > std::numeric_limits<std::uint32_t>::max()
                                           ? std::numeric_limits<std::uint32_t>::max()
                                           : static_cast<std::uint32_t>(shared_references);
    }

    [[nodiscard]] ResourceVector lane_entitlement(LaneId lane) const noexcept {
        return ledger_.lane(lane).state == LogicalLaneState::Free ? ResourceVector{}
                                                                  : ledger_.lane(lane).resources;
    }

    [[nodiscard]] CatalogState catalog_state(std::uint32_t slot) const noexcept {
        return slot < catalog_count_ ? catalog_[slot].state : CatalogState::Vacant;
    }

    void clear_after_program_cleanup() noexcept {
        context_transaction_.template emplace<std::monostate>();
        for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
            catalog_[slot].handle.reset();
            clear_catalog_entry(catalog_[slot]);
        }
        for (SessionIndexEntry& index : session_index_) { index = {}; }
        for (std::uint32_t lane = 0; lane < lane_count_; ++lane) { active_[lane] = {}; }
        for (SharedCatalogEntry& entry : shared_catalog_) {
            entry.handle.reset();
            clear_shared_catalog_entry(entry);
        }
        ledger_.clear();
        invalidate_replica_policy();
    }

private:
    void invalidate_replica_policy() noexcept {
        if (++replica_policy_generation_ == 0) { ++replica_policy_generation_; }
    }

    struct ReplicaTransitionRecord {
        bool shared            = false;
        std::uint32_t slot     = kInvalidCatalogSlot;
        std::uint64_t owner_id = 0;
        std::uint64_t revision = 0;
        ReplicaTransitionOption option;
        bool replacement_shared            = false;
        std::uint32_t replacement_slot     = kInvalidCatalogSlot;
        std::uint64_t replacement_owner_id = 0;
        std::uint64_t replacement_revision = 0;
        std::optional<typename Package::PressureOption> replacement;
        ResourceDemand demand;
    };

    struct ActiveCaptureRecord {
        LaneId lane;
        CaptureAssessment assessment;
        std::uint32_t publication_slot     = kInvalidCatalogSlot;
        std::uint64_t replacement_id       = 0;
        std::uint64_t replacement_revision = 0;
    };

    struct MaterializationRecord {
        LaneId destination;
        ResourceDemand demand;
        std::uint32_t source_slot           = kInvalidCatalogSlot;
        std::uint64_t source_id             = 0;
        ClaimDisposition source_disposition = ClaimDisposition::ConsumedToActive;
        std::uint32_t shared_source_slot    = kInvalidCatalogSlot;
        std::uint64_t shared_source_id      = 0;
        std::uint32_t publication_slot      = kInvalidCatalogSlot;
        std::vector<std::uint32_t> evictions;
        std::vector<std::uint64_t> eviction_ids;
        std::vector<typename Package::PressureOption> pressure_options;
        std::vector<std::uint32_t> shared_evictions;
        std::vector<std::uint64_t> shared_eviction_ids;
        std::vector<typename Package::PressureOption> shared_pressure_options;
        std::vector<PolicyObservationKey> eligible_observations;
        std::optional<PolicyObservationKey> selected_observation;
        std::optional<CacheSessionKey> session;
        RetentionClass retention                   = RetentionClass::RecentPrivate;
        bool update_session_index                  = true;
        std::uint64_t predicted_materialization_ns = 0;
        bool publish_continuation                  = true;
    };

    struct CatalogEntry {
        CatalogState state     = CatalogState::Vacant;
        std::uint64_t id       = 0;
        std::uint64_t revision = 1;
        ContinuationSummary summary;
        std::optional<ContinuationHandle> handle;
        std::optional<CacheSessionKey> session;
        std::vector<CheckpointObservation> observations;
        RetentionClass retention        = RetentionClass::RecentPrivate;
        std::uint32_t active_references = 0;
    };

    struct SharedCatalogEntry {
        SharedCatalogState state = SharedCatalogState::Vacant;
        std::uint64_t id         = 0;
        std::uint64_t revision   = 1;
        SharedPrefixSummary summary;
        std::optional<SharedPrefixHandle> handle;
        RetentionObservation observation{.retention_class = RetentionClass::SharedStable};
        std::uint32_t active_owner_mask = 0;
        std::uint32_t transaction_pins  = 0;
    };

    enum class SessionIndexState : std::uint8_t {
        Empty,
        Occupied,
        Deleted,
    };

    struct SessionIndexEntry {
        SessionIndexState state = SessionIndexState::Empty;
        CacheSessionKey key;
        std::uint32_t slot     = kInvalidCatalogSlot;
        std::uint64_t owner_id = 0;
        std::uint64_t revision = 0;
    };

    struct PrefixIndexEntry {
        bool occupied = false;
        bool shared   = false;
        PrefixShortlistKey key;
        std::uint32_t slot     = kInvalidCatalogSlot;
        std::uint64_t owner_id = 0;
        std::uint64_t revision = 0;
        CheckpointRef checkpoint;
    };

    struct ActiveEntry {
        bool occupied                  = false;
        std::uint32_t publication_slot = kInvalidCatalogSlot;
        std::uint64_t continuation_id  = 0;
        ResourceVector resources;
        std::optional<CacheSessionKey> session;
        RetentionClass retention           = RetentionClass::RecentPrivate;
        bool update_session_index          = true;
        bool publish_continuation          = true;
        std::uint32_t retained_source_slot = kInvalidCatalogSlot;
        std::uint64_t retained_source_id   = 0;
    };

    [[nodiscard]] static bool valid_prefill_work(PrefillWork work) noexcept {
        return work.tokens != 0;
    }

    [[nodiscard]] static std::size_t checked_prefix_index_capacity(std::uint32_t private_capacity,
                                                                   std::uint32_t shared_capacity,
                                                                   std::uint32_t max_long_anchors) {
        const std::size_t private_width = static_cast<std::size_t>(max_long_anchors) + 2U;
        if (private_capacity != 0 &&
            private_width >
                (std::numeric_limits<std::size_t>::max() - shared_capacity) / private_capacity) {
            throw std::overflow_error("content prefix index capacity overflow");
        }
        return static_cast<std::size_t>(private_capacity) * private_width + shared_capacity;
    }

    [[nodiscard]] bool
    valid_continuation_summary(const ContinuationSummary& summary) const noexcept {
        if ((!summary.endpoint && !summary.rewrite && summary.long_anchors.empty()) ||
            summary.long_anchors.size() > max_long_anchors_) {
            return false;
        }
        if (summary.endpoint &&
            (summary.endpoint->ref.kind != CheckpointKind::SessionEndpoint ||
             summary.endpoint->ref.frontier == 0 || summary.endpoint->ref.ordinal != 0 ||
             summary.endpoint->scope != CheckpointScope::Private ||
             summary.endpoint->shortlist_key.frontier != summary.endpoint->ref.frontier ||
             !valid_prefill_work(summary.endpoint->rebuild_work) ||
             summary.endpoint->required_kv.main_pages == 0)) {
            return false;
        }
        if (summary.rewrite &&
            (summary.rewrite->ref.kind == CheckpointKind::SessionEndpoint ||
             summary.rewrite->ref.kind == CheckpointKind::SharedStablePrefix ||
             summary.rewrite->ref.kind == CheckpointKind::LongAnchor ||
             summary.rewrite->ref.frontier == 0 || summary.rewrite->ref.ordinal != 0 ||
             (summary.endpoint && summary.rewrite->ref.frontier > summary.endpoint->ref.frontier) ||
             summary.rewrite->scope != CheckpointScope::Private ||
             summary.rewrite->shortlist_key.frontier != summary.rewrite->ref.frontier ||
             !valid_prefill_work(summary.rewrite->rebuild_work) ||
             summary.rewrite->required_kv.main_pages == 0)) {
            return false;
        }
        for (const auto& anchor : summary.long_anchors) {
            if (anchor.ref.kind != CheckpointKind::LongAnchor || anchor.ref.frontier == 0 ||
                (summary.endpoint && anchor.ref.frontier > summary.endpoint->ref.frontier) ||
                anchor.scope != CheckpointScope::Private ||
                anchor.shortlist_key.frontier != anchor.ref.frontier ||
                !valid_prefill_work(anchor.rebuild_work) || anchor.required_kv.main_pages == 0) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static bool
    valid_shared_prefix_summary(const SharedPrefixSummary& summary) noexcept {
        const auto& checkpoint = summary.checkpoint;
        return checkpoint.ref.kind == CheckpointKind::SharedStablePrefix &&
               checkpoint.ref.frontier != 0 && checkpoint.ref.ordinal == 0 &&
               checkpoint.scope == CheckpointScope::Shared &&
               valid_prefill_work(checkpoint.rebuild_work) &&
               checkpoint.required_kv.main_pages != 0 &&
               checkpoint.shortlist_key.frontier == checkpoint.ref.frontier;
    }

    static void advance_revision(std::uint64_t& revision) noexcept {
        if (++revision == 0) { ++revision; }
    }

    static void saturating_increment(std::uint64_t& value) noexcept {
        if (value != std::numeric_limits<std::uint64_t>::max()) { ++value; }
    }

    [[nodiscard]] static std::uint64_t saturating_add(std::uint64_t left,
                                                      std::uint64_t right) noexcept {
        return right > std::numeric_limits<std::uint64_t>::max() - left
                   ? std::numeric_limits<std::uint64_t>::max()
                   : left + right;
    }

    template <class Pressure>
    [[nodiscard]] static std::uint64_t pressure_spill_pages(const Pressure& option) noexcept {
        return static_cast<std::uint64_t>(option.main_kv.kind ==
                                                  decltype(option.main_kv.kind)::DemoteToHost
                                              ? option.main_kv.page_count
                                              : 0U) +
               static_cast<std::uint64_t>(option.backend_kv.kind ==
                                                  decltype(option.backend_kv.kind)::DemoteToHost
                                              ? option.backend_kv.page_count
                                              : 0U);
    }

    [[nodiscard]] static std::size_t
    transfer_resource_index(ContextResourceClass resource) noexcept {
        return static_cast<std::size_t>(resource);
    }

    [[nodiscard]] static std::size_t
    transfer_direction_index(ContextTransferDirection direction) noexcept {
        return static_cast<std::size_t>(direction);
    }

    void observe_transfer(const ContextTransferObservation& observation) {
        const std::size_t resource  = transfer_resource_index(observation.resource);
        const std::size_t direction = transfer_direction_index(observation.direction);
        if (resource >= 3 || direction >= 3 || observation.units == 0 ||
            observation.work.payload_bytes == 0 || observation.work.copy_operations == 0 ||
            observation.elapsed_ns == 0 ||
            (observation.resource == ContextResourceClass::State &&
             (observation.units != 1 || observation.page_count != 0)) ||
            (observation.resource != ContextResourceClass::State &&
             (observation.units != observation.work.payload_bytes ||
              observation.page_count == 0))) {
            throw std::logic_error("Program returned an invalid context-transfer observation");
        }
        context_stats_.actual_context_transfer_seconds +=
            static_cast<double>(observation.elapsed_ns) / 1'000'000'000.0;
        const double seconds = static_cast<double>(observation.elapsed_ns) / 1'000'000'000.0;
        if (observation.resource == ContextResourceClass::State) {
            if (observation.direction == ContextTransferDirection::DeviceToHost) {
                context_stats_.state_d2h_count =
                    saturating_add(context_stats_.state_d2h_count, observation.units);
                context_stats_.state_d2h_bytes =
                    saturating_add(context_stats_.state_d2h_bytes, observation.work.payload_bytes);
                context_stats_.state_d2h_seconds += seconds;
            } else if (observation.direction == ContextTransferDirection::HostToDevice) {
                context_stats_.state_h2d_count =
                    saturating_add(context_stats_.state_h2d_count, observation.units);
                context_stats_.state_h2d_bytes =
                    saturating_add(context_stats_.state_h2d_bytes, observation.work.payload_bytes);
                context_stats_.state_h2d_seconds += seconds;
            } else {
                context_stats_.state_d2d_count =
                    saturating_add(context_stats_.state_d2d_count, observation.units);
                context_stats_.state_d2d_bytes =
                    saturating_add(context_stats_.state_d2d_bytes, observation.work.payload_bytes);
                context_stats_.state_d2d_seconds += seconds;
            }
            return;
        }
        const bool main      = observation.resource == ContextResourceClass::MainKV;
        std::uint64_t* pages = nullptr;
        std::uint64_t* bytes = nullptr;
        double* elapsed      = nullptr;
        if (main && observation.direction == ContextTransferDirection::DeviceToHost) {
            pages   = &context_stats_.main_kv_d2h_pages;
            bytes   = &context_stats_.main_kv_d2h_bytes;
            elapsed = &context_stats_.main_kv_d2h_seconds;
        } else if (main && observation.direction == ContextTransferDirection::HostToDevice) {
            pages   = &context_stats_.main_kv_h2d_pages;
            bytes   = &context_stats_.main_kv_h2d_bytes;
            elapsed = &context_stats_.main_kv_h2d_seconds;
        } else if (main) {
            pages   = &context_stats_.main_kv_d2d_pages;
            bytes   = &context_stats_.main_kv_d2d_bytes;
            elapsed = &context_stats_.main_kv_d2d_seconds;
        } else if (observation.direction == ContextTransferDirection::DeviceToHost) {
            pages   = &context_stats_.backend_kv_d2h_pages;
            bytes   = &context_stats_.backend_kv_d2h_bytes;
            elapsed = &context_stats_.backend_kv_d2h_seconds;
        } else if (observation.direction == ContextTransferDirection::HostToDevice) {
            pages   = &context_stats_.backend_kv_h2d_pages;
            bytes   = &context_stats_.backend_kv_h2d_bytes;
            elapsed = &context_stats_.backend_kv_h2d_seconds;
        } else {
            pages   = &context_stats_.backend_kv_d2d_pages;
            bytes   = &context_stats_.backend_kv_d2d_bytes;
            elapsed = &context_stats_.backend_kv_d2d_seconds;
        }
        *pages = saturating_add(*pages, observation.page_count);
        *bytes = saturating_add(*bytes, observation.work.payload_bytes);
        *elapsed += seconds;
    }

    template <class Result>
    void observe_transfers(const Result& result) {
        for (const ContextTransferObservation& observation : result.transfer_observations) {
            observe_transfer(observation);
        }
    }

    template <class Result>
    void observe_operations(const Result& result) noexcept {
        context_stats_.state_moves =
            saturating_add(context_stats_.state_moves, result.operations.state_moves);
        context_stats_.state_forks =
            saturating_add(context_stats_.state_forks, result.operations.state_forks);
        context_stats_.state_restores =
            saturating_add(context_stats_.state_restores, result.operations.state_restores);
        context_stats_.partial_tail_cow_pages = saturating_add(
            context_stats_.partial_tail_cow_pages, result.operations.partial_tail_cow_pages);
        context_stats_.historical_fork_hits = saturating_add(
            context_stats_.historical_fork_hits, result.operations.historical_fork_hits);
    }

    [[nodiscard]] std::uint64_t estimate_transfer(ContextTransferDirection direction,
                                                  TransferWork work) const noexcept {
        return cost_model_.transfer_ns(direction, work);
    }

    [[nodiscard]] std::uint64_t estimate_prefill(PrefillWork work) const noexcept {
        return cost_model_.prefill_ns(work);
    }

    [[nodiscard]] static std::uint32_t saturating_add_u32(std::uint32_t left,
                                                          std::uint32_t right) noexcept {
        return right > std::numeric_limits<std::uint32_t>::max() - left
                   ? std::numeric_limits<std::uint32_t>::max()
                   : left + right;
    }

    static void add_tie_break(CostTieBreak& total, const CostTieBreak& value) noexcept {
        total.dropped_shared_stable =
            saturating_add_u32(total.dropped_shared_stable, value.dropped_shared_stable);
        total.dropped_live_session =
            saturating_add_u32(total.dropped_live_session, value.dropped_live_session);
        total.dropped_recent_private =
            saturating_add_u32(total.dropped_recent_private, value.dropped_recent_private);
        total.evicted_continuations =
            saturating_add_u32(total.evicted_continuations, value.evicted_continuations);
        total.dropped_checkpoints =
            saturating_add_u32(total.dropped_checkpoints, value.dropped_checkpoints);
        total.remaining_text_prefill =
            saturating_add(total.remaining_text_prefill, value.remaining_text_prefill);
        total.remaining_vision_prefill =
            saturating_add(total.remaining_vision_prefill, value.remaining_vision_prefill);
        total.transferred_state_images =
            saturating_add_u32(total.transferred_state_images, value.transferred_state_images);
        total.transferred_bytes = saturating_add(total.transferred_bytes, value.transferred_bytes);
        total.copy_operations   = saturating_add_u32(total.copy_operations, value.copy_operations);
        total.reused_prompt_tokens =
            saturating_add_u32(total.reused_prompt_tokens, value.reused_prompt_tokens);
    }

    static void add_cost(CostEstimate& total, const CostEstimate& value) noexcept {
        total.nanoseconds = saturating_add(total.nanoseconds, value.nanoseconds);
        add_tie_break(total.tie_break, value.tie_break);
    }

    [[nodiscard]] static CostEstimate subtract_cost_floor(CostEstimate after,
                                                          const CostEstimate& before) noexcept {
        const auto sub_u32 = [](std::uint32_t value, std::uint32_t removed) {
            return removed >= value ? 0U : value - removed;
        };
        const auto sub_u64 = [](std::uint64_t value, std::uint64_t removed) {
            return removed >= value ? 0ULL : value - removed;
        };
        after.nanoseconds = sub_u64(after.nanoseconds, before.nanoseconds);
        after.tie_break.dropped_shared_stable =
            sub_u32(after.tie_break.dropped_shared_stable, before.tie_break.dropped_shared_stable);
        after.tie_break.dropped_live_session =
            sub_u32(after.tie_break.dropped_live_session, before.tie_break.dropped_live_session);
        after.tie_break.dropped_recent_private = sub_u32(after.tie_break.dropped_recent_private,
                                                         before.tie_break.dropped_recent_private);
        after.tie_break.evicted_continuations =
            sub_u32(after.tie_break.evicted_continuations, before.tie_break.evicted_continuations);
        after.tie_break.dropped_checkpoints =
            sub_u32(after.tie_break.dropped_checkpoints, before.tie_break.dropped_checkpoints);
        after.tie_break.remaining_text_prefill   = sub_u64(after.tie_break.remaining_text_prefill,
                                                           before.tie_break.remaining_text_prefill);
        after.tie_break.remaining_vision_prefill = sub_u64(
            after.tie_break.remaining_vision_prefill, before.tie_break.remaining_vision_prefill);
        after.tie_break.transferred_state_images = sub_u32(
            after.tie_break.transferred_state_images, before.tie_break.transferred_state_images);
        after.tie_break.transferred_bytes =
            sub_u64(after.tie_break.transferred_bytes, before.tie_break.transferred_bytes);
        after.tie_break.copy_operations =
            sub_u32(after.tie_break.copy_operations, before.tie_break.copy_operations);
        after.tie_break.reused_prompt_tokens = 0;
        return after;
    }

    [[nodiscard]] static auto tie_break_key(const CostTieBreak& cost) noexcept {
        return std::tuple{
            cost.dropped_shared_stable,
            cost.dropped_live_session,
            cost.dropped_recent_private,
            cost.remaining_text_prefill,
            cost.remaining_vision_prefill,
            cost.transferred_state_images,
            cost.transferred_bytes,
            cost.copy_operations,
            cost.evicted_continuations,
            cost.dropped_checkpoints,
            std::numeric_limits<std::uint32_t>::max() - cost.reused_prompt_tokens,
        };
    }

    [[nodiscard]] static int compare_cost(const CostEstimate& left,
                                          const CostEstimate& right) noexcept {
        if (left.nanoseconds != right.nanoseconds) {
            return left.nanoseconds < right.nanoseconds ? -1 : 1;
        }
        const auto left_key  = tie_break_key(left.tie_break);
        const auto right_key = tie_break_key(right.tie_break);
        if (left_key < right_key) { return -1; }
        if (right_key < left_key) { return 1; }
        return 0;
    }

    [[nodiscard]] CostEstimate
    transfer_cost(std::span<const ContextTransferRequirement> requirements) const noexcept {
        CostEstimate cost;
        for (const ContextTransferRequirement& requirement : requirements) {
            if (requirement.units == 0) { continue; }
            if (requirement.resource == ContextResourceClass::State) {
                cost.tie_break.transferred_state_images =
                    saturating_add_u32(cost.tie_break.transferred_state_images,
                                       requirement.units > std::numeric_limits<std::uint32_t>::max()
                                           ? std::numeric_limits<std::uint32_t>::max()
                                           : static_cast<std::uint32_t>(requirement.units));
            }
            cost.tie_break.transferred_bytes =
                saturating_add(cost.tie_break.transferred_bytes, requirement.work.payload_bytes);
            cost.tie_break.copy_operations = saturating_add_u32(cost.tie_break.copy_operations,
                                                                requirement.work.copy_operations);
            cost.nanoseconds               = saturating_add(
                cost.nanoseconds, estimate_transfer(requirement.direction, requirement.work));
        }
        return cost;
    }

    [[nodiscard]] CostEstimate prefill_cost(PrefillWork work) const noexcept {
        CostEstimate cost;
        cost.tie_break.remaining_text_prefill   = work.tokens;
        cost.tie_break.remaining_vision_prefill = work.vision_patches;
        cost.nanoseconds                        = estimate_prefill(work);
        return cost;
    }

    [[nodiscard]] CostEstimate
    candidate_base_cost(const ResourceCandidateDescriptor& candidate) const noexcept {
        CostEstimate cost = prefill_cost(candidate.remaining_prefill_work);
        add_cost(cost, transfer_cost(candidate.transfer_requirements));
        cost.tie_break.reused_prompt_tokens = candidate.reused_prompt_tokens;
        return cost;
    }

    [[nodiscard]] static CostEstimate weighted_cost(CostEstimate cost,
                                                    std::uint32_t probability_q16) noexcept {
        const auto weighted_u64 = [probability_q16](std::uint64_t value) {
            if (value == 0 || probability_q16 == 0) { return std::uint64_t{0}; }
            const unsigned __int128 weighted =
                static_cast<unsigned __int128>(value) * probability_q16 + 0xffffU;
            const unsigned __int128 result = weighted >> 16U;
            return result > std::numeric_limits<std::uint64_t>::max()
                       ? std::numeric_limits<std::uint64_t>::max()
                       : static_cast<std::uint64_t>(result);
        };
        const auto weighted_u32 = [&](std::uint32_t value) {
            return static_cast<std::uint32_t>(std::min<std::uint64_t>(
                weighted_u64(value), std::numeric_limits<std::uint32_t>::max()));
        };
        cost.nanoseconds                      = weighted_u64(cost.nanoseconds);
        cost.tie_break.remaining_text_prefill = weighted_u64(cost.tie_break.remaining_text_prefill);
        cost.tie_break.remaining_vision_prefill =
            weighted_u64(cost.tie_break.remaining_vision_prefill);
        cost.tie_break.transferred_state_images =
            weighted_u32(cost.tie_break.transferred_state_images);
        cost.tie_break.transferred_bytes = weighted_u64(cost.tie_break.transferred_bytes);
        cost.tie_break.copy_operations   = weighted_u32(cost.tie_break.copy_operations);
        return cost;
    }

    template <class Impacts>
    [[nodiscard]] CostEstimate
    checkpoint_impacts_cost(const Impacts& impacts, const CatalogEntry* private_entry,
                            const SharedCatalogEntry* shared_entry) const noexcept {
        CostEstimate cost;
        for (const auto& impact : impacts) {
            RetentionObservation observation;
            if (shared_entry != nullptr &&
                shared_entry->summary.checkpoint.ref == impact.checkpoint) {
                observation = shared_entry->observation;
            } else if (private_entry != nullptr) {
                if (const RetentionObservation* found =
                        find_observation(private_entry->observations, impact.checkpoint)) {
                    observation = *found;
                } else {
                    observation.retention_class = private_entry->retention;
                }
            }

            CostEstimate after = prefill_cost(impact.fallback_rebuild_work);
            add_cost(after, transfer_cost(impact.fallback_restore_requirements));
            if (!impact.drops_checkpoint) {
                add_cost(after, transfer_cost(impact.current_restore_requirements));
            }
            add_cost(after, transfer_cost(impact.added_restore_requirements));
            const CostEstimate current = transfer_cost(impact.current_restore_requirements);
            CostEstimate marginal      = subtract_cost_floor(std::move(after), current);
            add_cost(cost,
                     weighted_cost(std::move(marginal), retention_probability_q16(observation)));
            if (!impact.drops_checkpoint) { continue; }
            cost.tie_break.dropped_checkpoints =
                saturating_add_u32(cost.tie_break.dropped_checkpoints, 1);
            switch (observation.retention_class) {
            case RetentionClass::SharedStable:
                cost.tie_break.dropped_shared_stable =
                    saturating_add_u32(cost.tie_break.dropped_shared_stable, 1);
                break;
            case RetentionClass::LiveSession:
                cost.tie_break.dropped_live_session =
                    saturating_add_u32(cost.tie_break.dropped_live_session, 1);
                break;
            case RetentionClass::RecentPrivate:
                cost.tie_break.dropped_recent_private =
                    saturating_add_u32(cost.tie_break.dropped_recent_private, 1);
                break;
            case RetentionClass::Disposable:
                break;
            }
        }
        return cost;
    }

    [[nodiscard]] CostEstimate pressure_cost(const typename Package::PressureOption& option,
                                             const CatalogEntry* private_entry,
                                             const SharedCatalogEntry* shared_entry) const {
        CostEstimate cost;
        add_cost(cost, transfer_cost(option.transfer_requirements));

        if (option.evicts_continuation && option.checkpoint_impacts.empty()) {
            throw std::logic_error("continuation eviction has no typed checkpoint impacts");
        }
        add_cost(cost,
                 checkpoint_impacts_cost(option.checkpoint_impacts, private_entry, shared_entry));
        if (option.evicts_continuation) { cost.tie_break.evicted_continuations = 1; }
        if (!option.removed_host_replica_impacts.empty()) {
            add_cost(cost, replica_value_cost(option.removed_host_replica_impacts, private_entry,
                                              shared_entry));
        }
        return cost;
    }

    template <class Impacts>
    [[nodiscard]] CostEstimate
    replica_value_cost(const Impacts& impacts, const CatalogEntry* private_entry,
                       const SharedCatalogEntry* shared_entry) const noexcept {
        CostEstimate value;
        for (const auto& impact : impacts) {
            RetentionObservation observation;
            if (shared_entry != nullptr &&
                shared_entry->summary.checkpoint.ref == impact.checkpoint) {
                observation = shared_entry->observation;
            } else if (private_entry != nullptr) {
                if (const RetentionObservation* found =
                        find_observation(private_entry->observations, impact.checkpoint)) {
                    observation = *found;
                } else {
                    observation.retention_class = private_entry->retention;
                }
            }
            CostEstimate without_backup = prefill_cost(impact.fallback_rebuild_work);
            add_cost(without_backup, transfer_cost(impact.fallback_restore_requirements));
            const CostEstimate with_backup = transfer_cost(impact.host_restore_requirements);
            CostEstimate marginal = subtract_cost_floor(std::move(without_backup), with_backup);
            add_cost(value,
                     weighted_cost(std::move(marginal), retention_probability_q16(observation)));
        }
        return value;
    }

    [[nodiscard]] std::uint64_t
    price_replica_option(const ReplicaTransitionOption& option) const noexcept {
        return estimate_transfer(ContextTransferDirection::DeviceToHost, option.transfer_work);
    }

    [[nodiscard]] static std::uint32_t eviction_priority(RetentionClass retention) noexcept {
        switch (retention) {
        case RetentionClass::Disposable:
            return 0;
        case RetentionClass::RecentPrivate:
            return 1;
        case RetentionClass::LiveSession:
            return 2;
        case RetentionClass::SharedStable:
            return 3;
        }
        return 3;
    }

    [[nodiscard]] static std::pair<std::uint64_t, std::uint64_t>
    retention_prior(RetentionClass retention) noexcept {
        switch (retention) {
        case RetentionClass::SharedStable:
            return {3, 1};
        case RetentionClass::LiveSession:
            return {2, 1};
        case RetentionClass::RecentPrivate:
            return {1, 1};
        case RetentionClass::Disposable:
            return {0, 1};
        }
        return {0, 1};
    }

    [[nodiscard]] static std::uint32_t
    retention_probability_q16(const RetentionObservation& observation) noexcept {
        const auto [prior_hits, prior_misses] = retention_prior(observation.retention_class);
        const unsigned __int128 numerator =
            (static_cast<unsigned __int128>(observation.selected_hit_count) + prior_hits) << 16U;
        const unsigned __int128 denominator =
            static_cast<unsigned __int128>(observation.exact_eligible_count) + prior_hits +
            prior_misses;
        if (denominator == 0) { return 0; }
        return static_cast<std::uint32_t>(std::min<unsigned __int128>(
            numerator / denominator, std::numeric_limits<std::uint32_t>::max()));
    }

    [[nodiscard]] static std::uint64_t newest_hit_epoch(const CatalogEntry& entry) noexcept {
        std::uint64_t epoch = 0;
        for (const CheckpointObservation& checkpoint : entry.observations) {
            epoch = std::max(epoch, checkpoint.observation.last_hit_epoch);
        }
        return epoch;
    }

    [[nodiscard]] static bool contains_checkpoint(const ContinuationSummary& summary,
                                                  CheckpointRef checkpoint) noexcept {
        if (summary.endpoint && summary.endpoint->ref == checkpoint) { return true; }
        if (summary.rewrite && summary.rewrite->ref == checkpoint) { return true; }
        return std::any_of(summary.long_anchors.begin(), summary.long_anchors.end(),
                           [&](const auto& anchor) { return anchor.ref == checkpoint; });
    }

    [[nodiscard]] static bool summaries_equal(const ContinuationSummary& left,
                                              const ContinuationSummary& right) noexcept {
        return left == right;
    }

    static void assign_continuation_summary(ContinuationSummary& destination,
                                            const ContinuationSummary& source) {
        if (source.long_anchors.size() > destination.long_anchors.capacity()) {
            throw std::logic_error("continuation summary exceeds its reserved backing");
        }
        destination.endpoint          = source.endpoint;
        destination.rewrite           = source.rewrite;
        destination.active_references = source.active_references;
        destination.long_anchors.clear();
        for (const auto& anchor : source.long_anchors) {
            destination.long_anchors.push_back(anchor);
        }
    }

    [[nodiscard]] static const RetentionObservation*
    find_observation(const std::vector<CheckpointObservation>& observations,
                     CheckpointRef checkpoint) noexcept {
        const auto found = std::find_if(observations.begin(), observations.end(),
                                        [&](const CheckpointObservation& candidate) {
                                            return candidate.checkpoint == checkpoint;
                                        });
        return found == observations.end() ? nullptr : &found->observation;
    }

    [[nodiscard]] static const RetentionObservation*
    find_kind_observation(const std::vector<CheckpointObservation>& observations,
                          CheckpointKind kind) noexcept {
        const auto found = std::find_if(observations.begin(), observations.end(),
                                        [&](const CheckpointObservation& candidate) {
                                            return candidate.checkpoint.kind == kind;
                                        });
        return found == observations.end() ? nullptr : &found->observation;
    }

    static void migrate_private_observations(
        CatalogEntry& entry, const ContinuationSummary& summary, RetentionClass retention,
        const RetentionObservation* replacement_session_endpoint = nullptr) {
        const std::size_t required = static_cast<std::size_t>(summary.endpoint.has_value()) +
                                     static_cast<std::size_t>(summary.rewrite.has_value()) +
                                     summary.long_anchors.size();
        if (entry.observations.capacity() == 0 || required > entry.observations.capacity() - 1U) {
            throw std::logic_error("private observation capacity diverged from the catalog");
        }

        std::size_t destination = 0;
        const auto append       = [&](CheckpointRef checkpoint,
                                std::optional<CheckpointKind> inherited_kind,
                                const RetentionObservation* external_inheritance = nullptr) {
            auto found =
                std::find_if(entry.observations.begin() + static_cast<std::ptrdiff_t>(destination),
                                   entry.observations.end(), [&](const CheckpointObservation& candidate) {
                                 return candidate.checkpoint == checkpoint;
                             });
            if (found == entry.observations.end() && inherited_kind) {
                found = std::find_if(
                    entry.observations.begin() + static_cast<std::ptrdiff_t>(destination),
                    entry.observations.end(), [&](const CheckpointObservation& candidate) {
                        return candidate.checkpoint.kind == *inherited_kind;
                    });
            }

            if (found != entry.observations.end()) {
                std::iter_swap(
                    entry.observations.begin() + static_cast<std::ptrdiff_t>(destination), found);
            } else {
                CheckpointObservation created{
                          .checkpoint  = checkpoint,
                          .observation = external_inheritance != nullptr
                                             ? *external_inheritance
                                             : RetentionObservation{.retention_class = retention},
                };
                if (destination < entry.observations.size()) {
                    entry.observations.push_back(std::move(created));
                    std::iter_swap(entry.observations.begin() +
                                             static_cast<std::ptrdiff_t>(destination),
                                         entry.observations.end() - 1);
                } else {
                    entry.observations.push_back(std::move(created));
                }
            }
            entry.observations[destination].checkpoint                  = checkpoint;
            entry.observations[destination].observation.retention_class = retention;
            ++destination;
        };

        if (summary.endpoint) {
            append(summary.endpoint->ref, CheckpointKind::SessionEndpoint,
                   replacement_session_endpoint);
        }
        if (summary.rewrite) { append(summary.rewrite->ref, summary.rewrite->ref.kind); }
        for (const auto& anchor : summary.long_anchors) { append(anchor.ref, std::nullopt); }
        entry.observations.resize(destination);
    }

    [[nodiscard]] RetentionObservation* find_observation(const PolicyObservationKey& key) noexcept {
        if (key.shared) {
            if (key.slot >= shared_catalog_count_) { return nullptr; }
            SharedCatalogEntry& entry = shared_catalog_[key.slot];
            if (entry.id != key.owner_id || entry.revision != key.revision || !entry.handle ||
                entry.summary.checkpoint.ref != key.checkpoint) {
                return nullptr;
            }
            return &entry.observation;
        }
        if (key.slot >= catalog_count_) { return nullptr; }
        CatalogEntry& entry = catalog_[key.slot];
        if (entry.id != key.owner_id || entry.revision != key.revision || !entry.handle ||
            !contains_checkpoint(entry.summary, key.checkpoint)) {
            return nullptr;
        }
        const auto found = std::find_if(entry.observations.begin(), entry.observations.end(),
                                        [&](const CheckpointObservation& observation) {
                                            return observation.checkpoint == key.checkpoint;
                                        });
        return found == entry.observations.end() ? nullptr : &found->observation;
    }

    void adopt_retention_observations(const MaterializationRecord& record) noexcept {
        for (const PolicyObservationKey& key : record.eligible_observations) {
            if (RetentionObservation* observation = find_observation(key)) {
                saturating_increment(observation->exact_eligible_count);
            }
        }
        if (!record.selected_observation) { return; }
        RetentionObservation* selected = find_observation(*record.selected_observation);
        if (selected == nullptr) { return; }
        saturating_increment(selected->selected_hit_count);
        saturating_increment(retention_epoch_);
        selected->last_hit_epoch = retention_epoch_;
    }

    void clear_catalog_entry(CatalogEntry& entry) noexcept {
        entry.state = CatalogState::Vacant;
        entry.id    = 0;
        entry.summary.endpoint.reset();
        entry.summary.rewrite.reset();
        entry.summary.long_anchors.clear();
        entry.handle.reset();
        entry.session.reset();
        entry.observations.clear();
        entry.retention         = RetentionClass::RecentPrivate;
        entry.active_references = 0;
        advance_revision(entry.revision);
    }

    void clear_catalog_slot(std::uint32_t slot) noexcept {
        if (slot >= catalog_count_) { return; }
        CatalogEntry& entry = catalog_[slot];
        if (entry.session && entry.id != 0) { erase_session_if_equals(*entry.session, entry.id); }
        clear_catalog_entry(entry);
    }

    void clear_shared_catalog_entry(SharedCatalogEntry& entry) noexcept {
        entry.state   = SharedCatalogState::Vacant;
        entry.id      = 0;
        entry.summary = {};
        entry.handle.reset();
        entry.observation = RetentionObservation{.retention_class = RetentionClass::SharedStable};
        entry.active_owner_mask = 0;
        entry.transaction_pins  = 0;
        advance_revision(entry.revision);
    }

    [[nodiscard]] bool valid_prefix_index_entry(const PrefixIndexEntry& index) const noexcept {
        if (!index.occupied) { return false; }
        if (index.shared) {
            if (index.slot >= shared_catalog_count_) { return false; }
            const SharedCatalogEntry& entry = shared_catalog_[index.slot];
            return entry.state == SharedCatalogState::Catalogued && entry.handle &&
                   entry.id == index.owner_id && entry.revision == index.revision &&
                   entry.summary.checkpoint.ref == index.checkpoint &&
                   entry.summary.checkpoint.shortlist_key == index.key;
        }
        if (index.slot >= catalog_count_) { return false; }
        const CatalogEntry& entry = catalog_[index.slot];
        if (entry.state != CatalogState::Catalogued || !entry.handle ||
            entry.id != index.owner_id || entry.revision != index.revision) {
            return false;
        }
        const auto matches = [&](const auto& checkpoint) {
            return checkpoint.ref == index.checkpoint && checkpoint.shortlist_key == index.key;
        };
        return (entry.summary.endpoint && matches(*entry.summary.endpoint)) ||
               (entry.summary.rewrite && matches(*entry.summary.rewrite)) ||
               std::any_of(entry.summary.long_anchors.begin(), entry.summary.long_anchors.end(),
                           matches);
    }

    void rebuild_prefix_indices() {
        for (PrefixIndexEntry& index : prefix_index_) { index = {}; }
        std::size_t cursor = 0;
        const auto append  = [&](bool shared, std::uint32_t slot, std::uint64_t owner_id,
                                std::uint64_t revision, const auto& checkpoint) {
            if (cursor >= prefix_index_.size()) {
                throw std::logic_error("content prefix index capacity diverged from catalog");
            }
            prefix_index_[cursor++] = PrefixIndexEntry{
                 .occupied   = true,
                 .shared     = shared,
                 .key        = checkpoint.shortlist_key,
                 .slot       = slot,
                 .owner_id   = owner_id,
                 .revision   = revision,
                 .checkpoint = checkpoint.ref,
            };
        };

        for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
            const CatalogEntry& entry = catalog_[slot];
            if (entry.state != CatalogState::Catalogued || !entry.handle) { continue; }
            if (entry.summary.long_anchors.size() > max_long_anchors_) {
                throw std::logic_error("private continuation exceeded content-index capacity");
            }
            if (entry.summary.endpoint) {
                append(false, slot, entry.id, entry.revision, *entry.summary.endpoint);
            }
            if (entry.summary.rewrite) {
                append(false, slot, entry.id, entry.revision, *entry.summary.rewrite);
            }
            for (const auto& anchor : entry.summary.long_anchors) {
                append(false, slot, entry.id, entry.revision, anchor);
            }
        }
        for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
            const SharedCatalogEntry& entry = shared_catalog_[slot];
            if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                !valid_shared_prefix_summary(entry.summary)) {
                continue;
            }
            append(true, slot, entry.id, entry.revision, entry.summary.checkpoint);
        }
    }

    void release_shared_active_references(LaneId lane) {
        if (lane.value >= lane_count_) {
            throw std::logic_error("shared active-reference lane is invalid");
        }
        const std::uint32_t bit = 1U << lane.value;
        for (SharedCatalogEntry& entry : shared_catalog_) {
            if ((entry.active_owner_mask & bit) == 0) { continue; }
            if (entry.state != SharedCatalogState::Catalogued ||
                entry.summary.active_references == 0) {
                throw std::logic_error("shared active-reference accounting diverged");
            }
            entry.active_owner_mask &= ~bit;
            --entry.summary.active_references;
        }
    }

    [[nodiscard]] bool valid_session_entry(const SessionIndexEntry& index) const noexcept {
        if (index.state != SessionIndexState::Occupied || index.slot >= catalog_count_) {
            return false;
        }
        const CatalogEntry& entry = catalog_[index.slot];
        return entry.id == index.owner_id && entry.revision == index.revision && entry.session &&
               *entry.session == index.key &&
               (entry.state == CatalogState::Catalogued ||
                entry.state == CatalogState::ReservedForActive);
    }

    [[nodiscard]] static std::uint64_t session_key_hash(const CacheSessionKey& key) noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        const auto mix     = [&](std::uint8_t byte) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        };
        for (const char byte : key.view()) { mix(static_cast<std::uint8_t>(byte)); }
        return hash;
    }

    [[nodiscard]] std::optional<std::size_t>
    find_session_cell(const CacheSessionKey& key) const noexcept {
        if (session_index_.empty()) { return std::nullopt; }
        const std::size_t begin = session_key_hash(key) % session_index_.size();
        for (std::size_t probe = 0; probe < session_index_.size(); ++probe) {
            const std::size_t position     = (begin + probe) % session_index_.size();
            const SessionIndexEntry& index = session_index_[position];
            if (index.state == SessionIndexState::Empty) { return std::nullopt; }
            if (index.state == SessionIndexState::Occupied && index.key == key) { return position; }
        }
        return std::nullopt;
    }

    static void erase_session_cell(SessionIndexEntry& index) noexcept {
        index       = {};
        index.state = SessionIndexState::Deleted;
    }

    [[nodiscard]] std::optional<std::uint32_t> lookup_session(const CacheSessionKey& key) {
        const std::optional<std::size_t> position = find_session_cell(key);
        if (!position) { return std::nullopt; }
        SessionIndexEntry& index = session_index_[*position];
        if (!valid_session_entry(index)) {
            erase_session_cell(index);
            return std::nullopt;
        }
        const CatalogEntry& entry = catalog_[index.slot];
        return entry.state == CatalogState::Catalogued && entry.handle &&
                       entry.active_references == 0
                   ? std::optional<std::uint32_t>(index.slot)
                   : std::nullopt;
    }

    [[nodiscard]] std::optional<SessionIndexEntry>
    session_binding(const CacheSessionKey& key) const noexcept {
        const std::optional<std::size_t> position = find_session_cell(key);
        return position && valid_session_entry(session_index_[*position])
                   ? std::optional<SessionIndexEntry>(session_index_[*position])
                   : std::nullopt;
    }

    [[nodiscard]] std::optional<SessionIndexEntry> exchange_session(const CacheSessionKey& key,
                                                                    std::uint32_t slot,
                                                                    std::uint64_t owner_id,
                                                                    std::uint64_t revision) {
        if (slot >= catalog_count_) { throw std::logic_error("session index slot is invalid"); }
        if (session_index_.empty()) { throw std::logic_error("session index has no storage"); }
        const std::size_t begin     = session_key_hash(key) % session_index_.size();
        SessionIndexEntry* reusable = nullptr;
        for (std::size_t probe = 0; probe < session_index_.size(); ++probe) {
            SessionIndexEntry& index = session_index_[(begin + probe) % session_index_.size()];
            if (index.state == SessionIndexState::Occupied && index.key == key) {
                const SessionIndexEntry previous = index;
                index.slot                       = slot;
                index.owner_id                   = owner_id;
                index.revision                   = revision;
                return previous;
            }
            if (index.state == SessionIndexState::Occupied && !valid_session_entry(index)) {
                erase_session_cell(index);
            }
            if (index.state == SessionIndexState::Deleted && reusable == nullptr) {
                reusable = &index;
                continue;
            }
            if (index.state == SessionIndexState::Empty) {
                reusable = reusable != nullptr ? reusable : &index;
                break;
            }
        }
        if (reusable == nullptr) {
            throw std::logic_error("session index capacity diverged from private catalog capacity");
        }
        *reusable = SessionIndexEntry{
            .state    = SessionIndexState::Occupied,
            .key      = key,
            .slot     = slot,
            .owner_id = owner_id,
            .revision = revision,
        };
        return std::nullopt;
    }

    void update_session_revision_if_equals(const CacheSessionKey& key, std::uint32_t slot,
                                           std::uint64_t owner_id,
                                           std::uint64_t revision) noexcept {
        const std::optional<std::size_t> position = find_session_cell(key);
        if (!position) { return; }
        SessionIndexEntry& index = session_index_[*position];
        if (index.slot == slot && index.owner_id == owner_id) { index.revision = revision; }
    }

    void erase_session_if_equals(const CacheSessionKey& key, std::uint64_t owner_id) noexcept {
        const std::optional<std::size_t> position = find_session_cell(key);
        if (!position) { return; }
        SessionIndexEntry& index = session_index_[*position];
        if (index.owner_id == owner_id) { erase_session_cell(index); }
    }

    void release_replaced_session(Program& program, const SessionIndexEntry& previous,
                                  std::uint32_t replacement_slot, std::uint64_t replacement_id) {
        if (previous.slot >= catalog_count_ ||
            (previous.slot == replacement_slot && previous.owner_id == replacement_id)) {
            return;
        }
        CatalogEntry& entry = catalog_[previous.slot];
        if (entry.id != previous.owner_id || !entry.session || *entry.session != previous.key) {
            return;
        }
        // A destructive Move keeps the SessionIndex pointing at its Active descriptor. A newer
        // finisher may temporarily replace that mapping, but must not revoke the active writer's
        // intent: if it finishes later it performs its own exchange and becomes latest.
        if (entry.state == CatalogState::ReservedForActive) { return; }
        if (entry.state != CatalogState::Catalogued || !entry.handle) { return; }
        entry.session.reset();
        entry.retention = RetentionClass::Disposable;
        for (CheckpointObservation& observation : entry.observations) {
            observation.observation.retention_class = RetentionClass::Disposable;
        }
        if (entry.active_references != 0) { return; }
        ReleaseResult released = program.release_continuation(std::move(*entry.handle));
        if (released.status != ConsumeStatus::Consumed ||
            released.resource_delta.added != ResourceVector{} ||
            !ledger_.release_inactive(released.resource_delta.removed)) {
            throw std::logic_error("replaced session continuation could not be released");
        }
        clear_catalog_slot(previous.slot);
    }

    [[nodiscard]] ResourceVector require_active(LaneId lane) const {
        if (lane.value >= lane_count_ || ledger_.lane(lane).state != LogicalLaneState::Active ||
            !active_[lane.value].occupied ||
            active_[lane.value].resources != ledger_.lane(lane).resources) {
            throw std::logic_error("resource lane is not active");
        }
        return active_[lane.value].resources;
    }

    static void add_projected_resources(DeviceResources& destination, DeviceResources value) {
        const auto checked_add = [](std::uint32_t left, std::uint32_t right) {
            if (right > std::numeric_limits<std::uint32_t>::max() - left) {
                throw std::overflow_error("protected projection resource overflow");
            }
            return left + right;
        };
        destination.active_lanes  = checked_add(destination.active_lanes, value.active_lanes);
        destination.state_slots   = checked_add(destination.state_slots, value.state_slots);
        destination.main_kv_pages = checked_add(destination.main_kv_pages, value.main_kv_pages);
        destination.backend_kv_pages =
            checked_add(destination.backend_kv_pages, value.backend_kv_pages);
    }

    struct ProjectedActivation {
        LaneId destination;
        DeviceResources active_resources;
        std::uint32_t source_slot           = kInvalidCatalogSlot;
        ClaimDisposition source_disposition = ClaimDisposition::ConsumedToActive;
        std::uint32_t shared_source_slot    = kInvalidCatalogSlot;
    };

    [[nodiscard]] static std::uint64_t
    projected_device_state_slots(const ProtectedHeadResourceProjection& projection) noexcept {
        std::uint64_t total = projection.fixed_non_reclaimable.state_slots;
        for (const DeviceResources& resources : projection.release_by_last_owner_mask) {
            total += resources.state_slots;
        }
        return total;
    }

    [[nodiscard]] bool
    device_state_headroom_preserved(const ProtectedHeadResourceProjection& projection,
                                    std::uint32_t active_lanes) const noexcept {
        if (active_lanes > lane_count_ || ledger_.capacity().device.state_slots < lane_count_) {
            return false;
        }
        const std::uint64_t protected_limit =
            static_cast<std::uint64_t>(ledger_.capacity().device.state_slots - lane_count_) +
            active_lanes;
        return projected_device_state_slots(projection) <= protected_limit;
    }

    [[nodiscard]] bool
    active_capture_state_headroom_preserved(std::uint64_t current_protected_state_slots,
                                            const CaptureAssessment& assessment) const noexcept {
        const std::uint64_t removed =
            assessment.active_entitlement_delta.removed.device.state_slots;
        const std::uint64_t preparation_removed =
            assessment.capacity_preparation_removed.device.state_slots;
        if (removed > current_protected_state_slots ||
            preparation_removed > current_protected_state_slots - removed) {
            return false;
        }
        std::uint64_t projected   = current_protected_state_slots - removed - preparation_removed;
        const std::uint64_t added = assessment.active_entitlement_delta.added.device.state_slots;
        if (added > std::numeric_limits<std::uint64_t>::max() - projected) { return false; }
        projected += added;
        if (assessment.publishes_shared && assessment.demand.final_added.device.state_slots != 0) {
            if (projected == std::numeric_limits<std::uint64_t>::max()) { return false; }
            ++projected;
        }
        if (ledger_.used().device.active_lanes > lane_count_ ||
            ledger_.capacity().device.state_slots < lane_count_) {
            return false;
        }
        const std::uint64_t protected_limit =
            static_cast<std::uint64_t>(ledger_.capacity().device.state_slots - lane_count_) +
            ledger_.used().device.active_lanes;
        return projected <= protected_limit;
    }

    [[nodiscard]] ProtectedHeadResourceProjection
    build_protected_projection(Program& program, const ProjectedActivation* hypothetical) const {
        ProtectedHeadResourceProjection projection;
        std::vector<std::uint32_t> private_masks(catalog_count_, 0);
        std::vector<std::uint32_t> shared_masks(shared_catalog_count_, 0);

        for (std::uint32_t lane = 0; lane < lane_count_; ++lane) {
            if (!active_[lane].occupied) { continue; }
            const std::uint32_t bit = 1U << lane;
            add_projected_resources(projection.release_by_last_owner_mask[bit],
                                    ledger_.lane(LaneId{lane}).resources.device);
            if (active_[lane].retained_source_slot != kInvalidCatalogSlot) {
                if (active_[lane].retained_source_slot >= catalog_count_) {
                    throw std::logic_error("active retained-source projection is stale");
                }
                private_masks[active_[lane].retained_source_slot] |= bit;
            }
        }
        for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
            shared_masks[slot] = shared_catalog_[slot].active_owner_mask;
        }

        if (hypothetical != nullptr) {
            if (hypothetical->destination.value >= lane_count_ ||
                ledger_.lane(hypothetical->destination).state != LogicalLaneState::Free) {
                throw std::logic_error("hypothetical protected projection destination is active");
            }
            const std::uint32_t bit = 1U << hypothetical->destination.value;
            add_projected_resources(projection.release_by_last_owner_mask[bit],
                                    hypothetical->active_resources);
            if (hypothetical->source_slot != kInvalidCatalogSlot &&
                hypothetical->source_disposition == ClaimDisposition::Retained) {
                private_masks[hypothetical->source_slot] |= bit;
            }
            if (hypothetical->shared_source_slot != kInvalidCatalogSlot) {
                shared_masks[hypothetical->shared_source_slot] |= bit;
            }
        }

        std::vector<typename Package::ProtectedPrivateOwner> private_owners;
        std::vector<typename Package::ProtectedSharedOwner> shared_owners;
        private_owners.reserve(catalog_count_);
        shared_owners.reserve(shared_catalog_count_);
        for (std::uint32_t slot = 0; slot < catalog_count_; ++slot) {
            if (private_masks[slot] == 0) { continue; }
            const CatalogEntry& entry = catalog_[slot];
            if ((entry.state != CatalogState::Catalogued && entry.state != CatalogState::Claimed) ||
                !entry.handle) {
                throw std::logic_error("protected private owner accounting diverged");
            }
            private_owners.push_back(typename Package::ProtectedPrivateOwner{
                .handle     = &*entry.handle,
                .owner_mask = private_masks[slot],
            });
        }
        for (std::uint32_t slot = 0; slot < shared_catalog_count_; ++slot) {
            if (shared_masks[slot] == 0) { continue; }
            const SharedCatalogEntry& entry = shared_catalog_[slot];
            if (entry.state != SharedCatalogState::Catalogued || !entry.handle) {
                throw std::logic_error("protected shared owner accounting diverged");
            }
            shared_owners.push_back(typename Package::ProtectedSharedOwner{
                .handle     = &*entry.handle,
                .owner_mask = shared_masks[slot],
            });
        }
        const auto pinned = program.project_protected_resources(private_owners, shared_owners);
        for (std::size_t mask = 1; mask < pinned.size(); ++mask) {
            add_projected_resources(projection.release_by_last_owner_mask[mask], pinned[mask]);
        }
        return projection;
    }

    [[nodiscard]] bool release_retained_source(ActiveEntry& active) noexcept {
        if (active.retained_source_slot == kInvalidCatalogSlot) {
            return active.retained_source_id == 0;
        }
        if (active.retained_source_slot >= catalog_count_) { return false; }
        CatalogEntry& source = catalog_[active.retained_source_slot];
        if (source.state != CatalogState::Catalogued || !source.handle ||
            source.id != active.retained_source_id || source.active_references == 0) {
            return false;
        }
        --source.active_references;
        active.retained_source_slot = kInvalidCatalogSlot;
        active.retained_source_id   = 0;
        return true;
    }

    [[nodiscard]] bool release_cancelled_lane(LaneId lane) noexcept {
        if (lane.value >= lane_count_ || !active_[lane.value].occupied) { return false; }
        const ResourceVector active = active_[lane.value].resources;
        try {
            release_shared_active_references(lane);
        } catch (...) { return false; }
        const bool reference_released = release_retained_source(active_[lane.value]);
        const bool ledger_released =
            ledger_.complete_active(lane, active, ResourceDelta{.removed = active});
        const std::uint32_t publication_slot = active_[lane.value].publication_slot;
        if (publication_slot < catalog_count_) { clear_catalog_slot(publication_slot); }
        active_[lane.value] = {};
        return reference_released && ledger_released;
    }

    void validate_choice(const Choice& choice) const {
        if (!choice.plan_ || choice.destination_.value >= lane_count_ ||
            ledger_.lane(choice.destination_).state != LogicalLaneState::Free ||
            choice.publication_slot_ >= catalog_count_ ||
            choice.pressure_options_.size() != choice.evictions_.size() ||
            choice.shared_pressure_options_.size() != choice.shared_evictions_.size() ||
            choice.shared_eviction_ids_.size() != choice.shared_evictions_.size() ||
            choice.shared_eviction_revisions_.size() != choice.shared_evictions_.size() ||
            choice.plan_->demand() != choice.demand_ ||
            (choice.source_slot_ != kInvalidCatalogSlot &&
             choice.shared_source_slot_ != kInvalidCatalogSlot) ||
            !detail::valid_demand(choice.demand_, choice.source_resources_,
                                  choice.source_disposition_)) {
            throw std::logic_error("admission choice is empty or its destination changed");
        }
        if (choice.source_slot_ != kInvalidCatalogSlot) {
            const CatalogEntry& source = catalog_[choice.source_slot_];
            if (source.state != CatalogState::Catalogued || source.id != choice.source_id_ ||
                source.revision != choice.source_revision_ || !source.handle ||
                source.active_references != 0) {
                throw std::logic_error("admission source changed after inspection");
            }
        }
        if (choice.shared_source_slot_ != kInvalidCatalogSlot) {
            if (choice.shared_source_slot_ >= shared_catalog_count_) {
                throw std::logic_error("admission shared source slot is invalid");
            }
            const SharedCatalogEntry& source = shared_catalog_[choice.shared_source_slot_];
            if (source.state != SharedCatalogState::Catalogued ||
                source.id != choice.shared_source_id_ ||
                source.revision != choice.shared_source_revision_ || !source.handle ||
                source.transaction_pins != 0) {
                throw std::logic_error("admission shared source changed after inspection");
            }
        }
        if (choice.source_slot_ == kInvalidCatalogSlot &&
            catalog_[choice.publication_slot_].state != CatalogState::Vacant &&
            std::find(choice.evictions_.begin(), choice.evictions_.end(),
                      choice.publication_slot_) == choice.evictions_.end()) {
            throw std::logic_error("root publication slot is no longer available");
        }
        for (std::size_t index = 0; index < choice.evictions_.size(); ++index) {
            const std::uint32_t slot  = choice.evictions_[index];
            const CatalogEntry& entry = catalog_[slot];
            if (slot == choice.source_slot_ || entry.state != CatalogState::Catalogued ||
                entry.id != choice.eviction_ids_[index] ||
                entry.revision != choice.eviction_revisions_[index] || !entry.handle ||
                entry.active_references != 0) {
                throw std::logic_error("admission victim changed after inspection");
            }
        }
        for (std::size_t index = 0; index < choice.shared_evictions_.size(); ++index) {
            const std::uint32_t slot = choice.shared_evictions_[index];
            if (slot >= shared_catalog_count_ || slot == choice.shared_source_slot_) {
                throw std::logic_error("admission shared victim slot is invalid");
            }
            const SharedCatalogEntry& entry = shared_catalog_[slot];
            if (entry.state != SharedCatalogState::Catalogued || !entry.handle ||
                entry.id != choice.shared_eviction_ids_[index] ||
                entry.revision != choice.shared_eviction_revisions_[index] ||
                entry.transaction_pins != 0 || entry.summary.active_references != 0) {
                throw std::logic_error("admission shared victim changed after inspection");
            }
        }

        if (!detail::valid_demand(choice.demand_, choice.source_resources_,
                                  choice.source_disposition_) ||
            !detail::demand_fits(ledger_.used(), choice.demand_, ledger_.capacity())) {
            throw std::logic_error("admission resource transaction changed after inspection");
        }
    }

    ResourceLedger ledger_;
    std::uint32_t lane_count_           = 0;
    std::uint32_t catalog_count_        = 0;
    std::uint32_t shared_catalog_count_ = 0;
    bool cache_enabled_                 = true;
    std::vector<CatalogEntry> catalog_;
    std::vector<SharedCatalogEntry> shared_catalog_;
    std::vector<SessionIndexEntry> session_index_;
    std::vector<PrefixIndexEntry> prefix_index_;
    std::uint32_t max_long_anchors_ = 0;
    std::array<ActiveEntry, kMaximumConcurrency> active_{};
    using ContextTransaction = std::variant<std::monostate, MaterializationRecord,
                                            ActiveCaptureRecord, ReplicaTransitionRecord>;
    ContextTransaction context_transaction_;
    ContextCostModel cost_model_;
    RuntimeStats context_stats_;
    std::uint64_t next_continuation_id_                = 1;
    std::uint64_t next_shared_prefix_id_               = 1;
    std::uint64_t retention_epoch_                     = 0;
    std::uint64_t replica_policy_generation_           = 1;
    std::uint64_t evaluated_replica_policy_generation_ = 0;
};

} // namespace ninfer::runtime
