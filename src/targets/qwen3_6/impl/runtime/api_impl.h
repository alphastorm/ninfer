#include "targets/qwen3_6/impl/runtime/instance.h"

#include <ninfer/targets/qwen3_6/prepared_prompt.h>

#include "targets/qwen3_6/impl/runtime/layouts.h"
#include "targets/qwen3_6/impl/runtime/program.h"

#include <stdexcept>
#include <utility>

namespace ninfer::targets::qwen3_6 {

using detail::NINFER_QWEN36_RUNTIME_NS::Variant;

template <>
SequencePlan<Variant>::SequencePlan(
    std::unique_ptr<detail::SequencePlanImpl<Variant>> impl) noexcept
    : impl_(std::move(impl)) {}

template <>
SequencePlan<Variant>::SequencePlan(SequencePlan&&) noexcept = default;
template <>
SequencePlan<Variant>& SequencePlan<Variant>::operator=(SequencePlan&&) noexcept = default;
template <>
SequencePlan<Variant>::~SequencePlan() = default;

template <>
std::uint32_t SequencePlan<Variant>::capacity() const noexcept {
    return impl_ != nullptr ? impl_->capacity : 0;
}

template <>
std::uint32_t SequencePlan<Variant>::kv_capacity() const noexcept {
    return impl_ != nullptr ? impl_->kv_capacity : 0;
}

template <>
std::uint32_t SequencePlan<Variant>::max_concurrency() const noexcept {
    return impl_ != nullptr ? impl_->max_concurrency : 0;
}

template <>
std::size_t SequencePlan<Variant>::device_reservation_bytes() const noexcept {
    return impl_ != nullptr ? impl_->device_reservation_bytes : 0;
}

template <>
std::size_t SequencePlan<Variant>::workspace_capacity_bytes() const noexcept {
    return impl_ != nullptr ? impl_->workspace.capacity : 0;
}

template <>
SequencePlanner<Variant>::SequencePlanner(
    std::unique_ptr<detail::SequencePlannerImpl<Variant>> impl) noexcept
    : impl_(std::move(impl)) {}

template <>
SequencePlanner<Variant>::SequencePlanner(SequencePlanner&&) noexcept = default;
template <>
SequencePlanner<Variant>& SequencePlanner<Variant>::operator=(SequencePlanner&&) noexcept = default;
template <>
SequencePlanner<Variant>::~SequencePlanner() = default;

template <>
const runtime::SequenceCapacityCurve& SequencePlanner<Variant>::capacity_curve() const noexcept {
    static const runtime::SequenceCapacityCurve empty;
    return impl_ != nullptr ? impl_->curve : empty;
}

template <>
SequencePlan<Variant> SequencePlanner<Variant>::finalize(std::uint32_t main_page_groups) && {
    if (impl_ == nullptr) { throw std::logic_error("sequence planner is empty"); }
    return SequencePlan<Variant>(detail::NINFER_QWEN36_RUNTIME_NS::finalize_sequence_plan_impl(
        std::move(impl_), main_page_groups));
}

template <>
RequestBasePlan<Variant>::RequestBasePlan(
    std::unique_ptr<detail::RequestBasePlanImpl<Variant>> impl) noexcept
    : impl_(std::move(impl)) {}

template <>
RequestBasePlan<Variant>::RequestBasePlan(RequestBasePlan&&) noexcept = default;
template <>
RequestBasePlan<Variant>& RequestBasePlan<Variant>::operator=(RequestBasePlan&&) noexcept = default;
template <>
RequestBasePlan<Variant>::~RequestBasePlan() = default;

template <>
const runtime::RequestPlanSummary& RequestBasePlan<Variant>::summary() const noexcept {
    static const runtime::RequestPlanSummary empty;
    return impl_ != nullptr ? impl_->summary : empty;
}

template <>
const runtime::ResourceDemand& RequestBasePlan<Variant>::root_demand() const noexcept {
    static const runtime::ResourceDemand empty;
    return impl_ != nullptr ? impl_->root_demand : empty;
}

template <>
const PreparedContextCache& RequestBasePlan<Variant>::context_cache() const noexcept {
    static const PreparedContextCache empty;
    return impl_ != nullptr ? impl_->context_cache : empty;
}

template <>
std::optional<PrefixShortlistKey>
RequestBasePlan<Variant>::prefix_shortlist_key(std::uint32_t frontier) const noexcept {
    if (impl_ == nullptr || frontier == 0 || frontier > impl_->prefix_digests.size()) {
        return std::nullopt;
    }
    return PrefixShortlistKey{
        .digest       = impl_->prefix_digests.at(frontier),
        .frontier     = frontier,
        .identity_tag = impl_->prefix_identity_tag,
    };
}

template <>
AdmissionPlan<Variant>::AdmissionPlan(
    std::unique_ptr<detail::AdmissionPlanImpl<Variant>> impl) noexcept
    : impl_(std::move(impl)) {}

template <>
AdmissionPlan<Variant>::AdmissionPlan(AdmissionPlan&&) noexcept = default;
template <>
AdmissionPlan<Variant>& AdmissionPlan<Variant>::operator=(AdmissionPlan&&) noexcept = default;
template <>
AdmissionPlan<Variant>::~AdmissionPlan() = default;

template <>
const runtime::RequestPlanSummary& AdmissionPlan<Variant>::summary() const noexcept {
    static const runtime::RequestPlanSummary empty;
    return impl_ != nullptr ? impl_->summary : empty;
}

template <>
const runtime::ResourceDemand& AdmissionPlan<Variant>::demand() const noexcept {
    static const runtime::ResourceDemand empty;
    return impl_ != nullptr ? impl_->demand : empty;
}

template <>
runtime::ResourceVector AdmissionPlan<Variant>::source_resources() const noexcept {
    return impl_ != nullptr ? impl_->source_resources : runtime::ResourceVector{};
}

template <>
runtime::ClaimDisposition AdmissionPlan<Variant>::source_disposition() const noexcept {
    return impl_ != nullptr ? impl_->source_disposition
                            : runtime::ClaimDisposition::ConsumedToActive;
}

template <>
bool AdmissionPlan<Variant>::needs_transfer() const noexcept {
    return impl_ != nullptr && impl_->needs_transfer;
}

template <>
bool AdmissionPlan<Variant>::temporal_eligible() const noexcept {
    return impl_ != nullptr && impl_->temporal_eligible;
}

template <>
runtime::PrefillWork AdmissionPlan<Variant>::remaining_prefill_work() const noexcept {
    return impl_ != nullptr ? impl_->remaining_prefill_work : runtime::PrefillWork{};
}

template <>
std::span<const runtime::ContextTransferRequirement>
AdmissionPlan<Variant>::transfer_requirements() const noexcept {
    return impl_ != nullptr
               ? std::span<const runtime::ContextTransferRequirement>(impl_->transfer_requirements)
               : std::span<const runtime::ContextTransferRequirement>{};
}

template <>
Program<Variant>::Program(std::unique_ptr<detail::ProgramImpl<Variant>> impl) noexcept
    : impl_(std::move(impl)) {}

template <>
Program<Variant>::~Program() noexcept = default;

template <>
RequestBasePlan<Variant>
Program<Variant>::plan_request(const PreparedPrompt& prompt,
                               const runtime::ResolvedExecutionOptions& options) {
    return impl_->plan_request(PreparedPromptAccess::view(prompt), options);
}

template <>
std::optional<AdmissionPlan<Variant>> Program<Variant>::inspect_admission(
    const PreparedPrompt& prompt, const RequestBasePlan<Variant>& base, runtime::LaneId destination,
    const ContinuationHandle<Variant>* source, const SharedPrefixHandle<Variant>* shared_source,
    std::optional<runtime::CheckpointRef> checkpoint, bool must_retain_private_source) {
    return impl_->inspect_admission(PreparedPromptAccess::view(prompt), base, destination, source,
                                    shared_source, checkpoint, must_retain_private_source);
}

template <>
std::vector<PressureOption>
Program<Variant>::inspect_pressure_options(const ContinuationHandle<Variant>& continuation,
                                           runtime::ResourceVector deficit) const {
    return impl_->inspect_pressure_options(continuation, deficit);
}

template <>
std::vector<PressureOption>
Program<Variant>::inspect_pressure_options(const AdmissionPlan<Variant>& admission,
                                           const ContinuationHandle<Variant>& continuation,
                                           runtime::ResourceVector deficit) const {
    return impl_->inspect_pressure_options(admission, continuation, deficit);
}

template <>
PressureOption
Program<Variant>::inspect_eviction_option(const ContinuationHandle<Variant>& continuation) const {
    return impl_->inspect_eviction_option(continuation);
}

template <>
std::vector<PressureOption>
Program<Variant>::inspect_shared_pressure_options(const SharedPrefixHandle<Variant>& shared,
                                                  runtime::ResourceVector deficit) const {
    return impl_->inspect_shared_pressure_options(shared, deficit);
}

template <>
std::vector<PressureOption>
Program<Variant>::inspect_shared_pressure_options(const AdmissionPlan<Variant>& admission,
                                                  const SharedPrefixHandle<Variant>& shared,
                                                  runtime::ResourceVector deficit) const {
    return impl_->inspect_shared_pressure_options(admission, shared, deficit);
}

template <>
PressureOption
Program<Variant>::inspect_shared_eviction_option(const SharedPrefixHandle<Variant>& shared) const {
    return impl_->inspect_shared_eviction_option(shared);
}

template <>
std::optional<runtime::MaterializationPressureEffect>
Program<Variant>::inspect_combined_pressure_effect(
    const AdmissionPlan<Variant>& admission,
    std::span<const ContinuationHandle<Variant>* const> pressure_owners,
    std::span<const PressureOption> pressure_options,
    std::span<const SharedPrefixHandle<Variant>* const> shared_pressure_owners,
    std::span<const PressureOption> shared_pressure_options) const {
    return impl_->inspect_combined_pressure_effect(admission, pressure_owners, pressure_options,
                                                   shared_pressure_owners, shared_pressure_options);
}

template <>
std::optional<AdmissionPlan<Variant>> Program<Variant>::compose_materialization(
    AdmissionPlan<Variant>&& admission,
    std::span<const ContinuationHandle<Variant>* const> pressure_owners,
    std::span<const PressureOption> pressure_options,
    std::span<const SharedPrefixHandle<Variant>* const> shared_pressure_owners,
    std::span<const PressureOption> shared_pressure_options) {
    return impl_->compose_materialization(std::move(admission), pressure_owners, pressure_options,
                                          shared_pressure_owners, shared_pressure_options);
}

template <>
runtime::ResourceVector Program<Variant>::admission_capacity() const noexcept {
    return impl_->admission_capacity();
}

template <>
runtime::PreflightStatus Program<Variant>::revalidate_materialization(
    const AdmissionPlan<Variant>& plan, const PreparedPrompt& prompt,
    const ContinuationHandle<Variant>* source, const SharedPrefixHandle<Variant>* shared_source,
    std::span<const ContinuationHandle<Variant>* const> victims,
    std::span<const SharedPrefixHandle<Variant>* const> shared_victims) const {
    return impl_->revalidate_materialization(plan, PreparedPromptAccess::view(prompt), source,
                                             shared_source, victims, shared_victims);
}

template <>
runtime::ContextTransactionReserveStatus Program<Variant>::reserve_materialization(
    AdmissionPlan<Variant>&& plan, PreparedPrompt&& prompt,
    const ContinuationHandle<Variant>* source, const SharedPrefixHandle<Variant>* shared_source,
    std::span<const ContinuationHandle<Variant>* const> victims,
    std::span<const SharedPrefixHandle<Variant>* const> shared_victims,
    runtime::CancellationFlagView cancellation) {
    return impl_->reserve_materialization(std::move(plan),
                                          PreparedPromptAccess::take(std::move(prompt)), source,
                                          shared_source, victims, shared_victims, cancellation);
}

template <>
ContextTransactionProgress<Variant>
Program<Variant>::progress_context_transaction(runtime::CancellationFlagView cancellation) {
    return impl_->progress_context_transaction(cancellation);
}

template <>
void Program<Variant>::finalize_context_transaction() noexcept {
    impl_->finalize_context_transaction();
}

template <>
bool Program<Variant>::has_context_transaction() const noexcept {
    return impl_->has_context_transaction();
}

template <>
PrefillProgress<Variant>
Program<Variant>::advance_prefill(SequenceHandle<Variant> sequence,
                                  runtime::ExecutionTiming* failed_timing) {
    return impl_->advance_prefill(sequence, failed_timing);
}

template <>
CaptureAssessment
Program<Variant>::inspect_capture(const CaptureOffer<Variant>& offer,
                                  const SharedPrefixHandle<Variant>* exact_shared,
                                  const SharedPrefixHandle<Variant>* replacement,
                                  std::optional<runtime::CheckpointRef> private_replacement) const {
    return impl_->inspect_capture(offer, exact_shared, replacement, private_replacement);
}

template <>
bool Program<Variant>::shared_capture_matches(const CaptureOffer<Variant>& offer,
                                              const SharedPrefixHandle<Variant>& shared) const {
    return impl_->shared_capture_matches(offer, shared);
}

template <>
void Program<Variant>::skip_capture(CaptureOffer<Variant>&& offer) {
    impl_->skip_capture(std::move(offer));
}

template <>
runtime::ContextTransactionReserveStatus
Program<Variant>::reserve_active_capture(CaptureOffer<Variant>&& offer,
                                         const SharedPrefixHandle<Variant>* exact_shared,
                                         const SharedPrefixHandle<Variant>* replacement,
                                         std::optional<runtime::CheckpointRef> private_replacement,
                                         runtime::CancellationFlagView cancellation) {
    return impl_->reserve_active_capture(std::move(offer), exact_shared, replacement,
                                         private_replacement, cancellation);
}

template <>
std::optional<ReplicaTransitionOption>
Program<Variant>::inspect_replica_transition(const ContinuationHandle<Variant>& owner,
                                             runtime::CheckpointRef checkpoint) const {
    return impl_->inspect_replica_transition(owner, checkpoint);
}

template <>
std::optional<ReplicaTransitionOption>
Program<Variant>::inspect_replica_transition(const SharedPrefixHandle<Variant>& owner) const {
    return impl_->inspect_replica_transition(owner);
}

template <>
runtime::PreflightStatus Program<Variant>::revalidate_replica_transition(
    const ContinuationHandle<Variant>* private_owner,
    const SharedPrefixHandle<Variant>* shared_owner, const ReplicaTransitionOption& option,
    const ContinuationHandle<Variant>* private_replacement,
    const SharedPrefixHandle<Variant>* shared_replacement,
    const PressureOption* replacement) const {
    return impl_->revalidate_replica_transition(
        private_owner, shared_owner, option, private_replacement, shared_replacement, replacement);
}

template <>
runtime::ContextTransactionReserveStatus Program<Variant>::reserve_prevalidated_replica_transition(
    const ContinuationHandle<Variant>* private_owner,
    const SharedPrefixHandle<Variant>* shared_owner, ReplicaTransitionOption option,
    const ContinuationHandle<Variant>* private_replacement,
    const SharedPrefixHandle<Variant>* shared_replacement,
    std::optional<PressureOption> replacement, runtime::CancellationFlagView cancellation) {
    return impl_->reserve_prevalidated_replica_transition(
        private_owner, shared_owner, std::move(option), private_replacement, shared_replacement,
        std::move(replacement), cancellation);
}

template <>
PendingBatch<Variant> Program<Variant>::decode(std::span<const SequenceHandle<Variant>> sequences,
                                               std::span<const runtime::RoundBudget> budgets,
                                               runtime::ExecutionTiming* failed_timing) {
    return impl_->decode(sequences, budgets, failed_timing);
}

template <>
runtime::ExecutionTiming Program<Variant>::append_forced_tokens(
    std::span<const SequenceHandle<Variant>> sequences, std::span<const TokenId> row_major_tokens,
    std::uint32_t row_stride, runtime::ExecutionTiming* failed_timing) {
    return impl_->append_forced_tokens(sequences, row_major_tokens, row_stride, failed_timing);
}

template <>
CommitResult<Variant> Program<Variant>::commit(PendingBatch<Variant>&& pending,
                                               std::span<const runtime::CommitDecision> decisions,
                                               runtime::CommitObservation observation,
                                               runtime::ExecutionTiming* failed_timing) {
    return impl_->commit(std::move(pending), decisions, observation, failed_timing);
}

template <>
DiscardResult<Variant> Program<Variant>::abort_pending(PendingBatch<Variant>&& pending) noexcept {
    return impl_->abort_pending(std::move(pending));
}

template <>
FinishResult<Variant> Program<Variant>::finish(SequenceHandle<Variant> sequence) noexcept {
    return impl_->finish(sequence);
}

template <>
AbortResult<Variant> Program<Variant>::abort(SequenceHandle<Variant> sequence) noexcept {
    return impl_->abort(sequence);
}

template <>
ReleaseResult<Variant>
Program<Variant>::release_continuation(ContinuationHandle<Variant>&& continuation) noexcept {
    return impl_->release_continuation(std::move(continuation));
}

template <>
ReleaseResult<Variant>
Program<Variant>::release_shared_prefix(SharedPrefixHandle<Variant>&& shared) noexcept {
    return impl_->release_shared_prefix(std::move(shared));
}

template <>
std::optional<runtime::ContinuationCheckpointStats>
Program<Variant>::checkpoint_continuation(
    const ContinuationHandle<Variant>& continuation,
    runtime::ContinuationCheckpointWriter& writer, std::size_t staging_bytes) const {
    return impl_->checkpoint_continuation(continuation, writer, staging_bytes);
}

template <>
std::optional<RestoredContinuation<Variant>> Program<Variant>::restore_continuation(
    const runtime::ContinuationCheckpointReader& reader, std::size_t staging_bytes) {
    return impl_->restore_continuation(reader, staging_bytes);
}
template <>
std::array<runtime::DeviceResources, 1U << kMaximumConcurrency>
Program<Variant>::project_protected_resources(
    std::span<const ProtectedPrivateOwner<Variant>> private_owners,
    std::span<const ProtectedSharedOwner<Variant>> shared_owners) const {
    return impl_->project_protected_resources(private_owners, shared_owners);
}

template <>
void Program<Variant>::fail_all_cleanup() noexcept {
    impl_->fail_all_cleanup();
}

template <>
MemorySummary Program<Variant>::memory_summary() const noexcept {
    return impl_->memory_summary();
}

template <>
void Program<Variant>::reset_memory_peaks() noexcept {
    impl_->reset_memory_peaks();
}

template <>
SequencePlanner<Variant> make_sequence_planner<Variant>(DeviceContext& device,
                                                        const EngineOptions& options,
                                                        Variant::WeightsProfile weights_profile) {
    return SequencePlanner<Variant>(detail::NINFER_QWEN36_RUNTIME_NS::make_sequence_planner_impl(
        device, options, weights_profile));
}

template <>
std::unique_ptr<Program<Variant>>
create_program<Variant>(const Variant::ModelView& model, Variant::WeightsProfile weights_profile,
                        SequencePlan<Variant>&& plan, DeviceContext& device) {
    if (plan.impl_ == nullptr) { throw std::invalid_argument("sequence plan is empty"); }
    if (plan.impl_->weights_profile != weights_profile) {
        throw std::invalid_argument(
            "loaded model weights profile does not match the sequence plan");
    }
    auto impl = std::make_unique<detail::ProgramImpl<Variant>>(model, *plan.impl_, device);
    plan.impl_.reset();
    return std::unique_ptr<Program<Variant>>(new Program<Variant>(std::move(impl)));
}

} // namespace ninfer::targets::qwen3_6
