#include "models/qwen4_exp/program/internal.h"
#include "models/common/context/work.h"

#include <algorithm>

namespace ninfer::models::qwen4_exp {

bool Program::valid_checkpoint(CheckpointHandle h) const noexcept {
    return impl_->checkpoints.valid(h);
}

bool Program::release_checkpoint(CheckpointHandle h) noexcept {
    return impl_->checkpoints.erase(h);
}

CheckpointMetadata Program::checkpoint_metadata(CheckpointHandle h) const {
    const auto& p = *impl_;
    const auto& c = p.checkpoints.at(h);
    return {c.frontier(), c.role, p.checkpoints[h.index].pins != 0};
}

PrefixShortlistKey Program::checkpoint_key(CheckpointHandle h, std::uint32_t f) const {
    const auto& c = impl_->checkpoints.at(h);
    if (f > c.frontier()) throw std::out_of_range("checkpoint key exceeds its frontier");
    return detail::key(c.snapshot.identity(), f);
}

bool Program::checkpoint_matches(CheckpointHandle h, const RequestBasePlan& base) const {
    if (!valid_checkpoint(h) || !base.plan_->reuse) return false;
    const auto& c        = impl_->checkpoints.at(h);
    const auto& identity = c.snapshot.identity();
    return identity.tag == base.plan_->identity->tag &&
           qwen::prefix_matches(base.plan_->prompt, identity.tokens, identity.positions,
                                c.frontier());
}

std::uint32_t Program::checkpoint_recovery_frontier(CheckpointHandle h, const RequestBasePlan& base,
                                                    std::uint32_t target) const {
    return checkpoint_matches(h, base) && impl_->checkpoints.at(h).frontier() <= target
               ? impl_->checkpoints.at(h).frontier()
               : 0;
}

std::optional<SourceCandidate>
Program::inspect_source(const RequestBasePlan& base, std::optional<CheckpointHandle> h,
                        bool consume, std::span<const CheckpointHandle> carried,
                        std::span<const CheckpointHandle> retired) const {
    const auto& p = *impl_;
    if (h && !checkpoint_matches(*h, base)) return {};
    SourceCandidate out;
    out.checkpoint     = h;
    out.reused_tokens  = h ? p.checkpoints.at(*h).frontier() : 0;
    out.remaining_work = runtime::make_prefill_work(
        out.reused_tokens, base.summary().prompt_tokens - out.reused_tokens, 0, 0,
        p.shape.prefill_chunk);
    for (const auto& item : base.plan_->prompt.vision_items)
        if (item.token_spans.back().begin + item.token_spans.back().count > out.reused_tokens) {
            ++out.remaining_work.vision_items;
            out.remaining_work.vision_patches += item.patch_count;
        }
    if (const auto& vision = base.plan_->vision) {
        out.remaining_work.chunks = 0;
        for (auto cursor = out.reused_tokens; cursor < base.summary().prompt_tokens;) {
            cursor = vision->window_end(
                cursor, std::min(base.summary().prompt_tokens, cursor + p.shape.prefill_chunk));
            ++out.remaining_work.chunks;
        }
    }
    if (h) {
        const auto& c      = p.checkpoints.at(*h);
        const auto image   = c.snapshot.continuation().image;
        const auto& states = *p.resources.state_store;
        out.consume_source = consume && p.checkpoints[h->index].pins == 0 &&
                             states.checkpoint_references(image) == 1 && !states.source_pins(image);
        if (!states.device_resident(image))
            out.transfers.push_back(context::state_transfer_requirement(
                {p.resources.states->storage().image_bytes(), 1},
                runtime::ContextTransferDirection::HostToDevice));
        for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool) {
            const auto& storage   = p.resources.pool(pool);
            const auto& prefix    = c.snapshot.prefix(pool);
            std::uint32_t missing = 0;
            for (std::uint32_t i = 0; i < (prefix.frontier() + 63) / 64; ++i) {
                const auto page = storage.histories.logical_page(prefix.history(), i);
                missing +=
                    !storage.pages.device_covers(page, std::min(64U, prefix.frontier() - i * 64));
            }
            if (missing)
                out.transfers.push_back(context::kv_transfer_requirement(
                    {runtime::KvPoolId{pool}}, runtime::ContextTransferDirection::HostToDevice,
                    missing,
                    {p.pools[pool].device_page_bytes * missing,
                     static_cast<std::uint32_t>(storage.cache.pages().plane_count()) * missing}));
        }
    }
    for (const auto point : carried)
        if ((!h || point != *h || !out.consume_source) && checkpoint_matches(point, base) &&
            p.checkpoints.at(point).frontier() <= out.reused_tokens)
            out.private_points.push_back(point);
    for (const auto point : retired)
        if ((!h || point != *h) && valid_checkpoint(point)) out.retired_points.push_back(point);
    return out;
}

BindingReservation Program::start_binding(const RequestBasePlan& base, runtime::LaneId lane,
                                          const SourceCandidate& candidate, ResumeState* resume,
                                          ExecutionUnitKind resume_kind, std::uint32_t tokens) {
    auto& p = *impl_;
    if (p.operation || p.pending_transaction || lane.value >= p.shape.max_concurrency ||
        p.sequences[lane.value])
        throw std::logic_error("binding requires an empty lane and transaction slot");
    if (resume && (!resume->state_ || resume->state_->owner != &p || !resume->state_->sequence))
        throw std::logic_error("binding has an invalid resume owner");
    const bool own    = resume && resume->has_snapshot();
    const auto source = own ? resume->snapshot_handle() : candidate.checkpoint;
    if (source &&
        (!p.checkpoints.valid(*source) ||
         (!own && (!checkpoint_matches(*source, base) ||
                   (resume && p.checkpoints.at(*source).frontier() > resume->frontier())))))
        return {.source_valid = false};
    auto& states           = *p.resources.state_store;
    const auto* checkpoint = source ? &p.checkpoints.at(*source) : nullptr;
    const auto f           = checkpoint ? checkpoint->frontier() : 0;
    const auto image    = checkpoint ? checkpoint->snapshot.continuation().image : detail::Image{};
    const bool resident = checkpoint && states.device_resident(image);
    const bool consume  = checkpoint && (own || candidate.consume_source) &&
                         p.checkpoints[source->index].pins == 0 &&
                         states.checkpoint_references(image) == 1 && !states.source_pins(image);
    const bool split = checkpoint && !consume && resident && states.host_resident(image) &&
                       !states.source_pins(image);
    const auto target = resume ? resume->frontier() : f;
    const auto requested_peak =
        resume ? std::uint64_t(target) +
                     (resume_kind == ExecutionUnitKind::Decode
                          ? std::min(p.shape.draft_tokens + 1, std::max(tokens, 1U))
                          : tokens)
               : std::min<std::uint64_t>(base.summary().prompt_tokens,
                                         std::uint64_t(f) + p.shape.prefill_chunk);
    auto peak =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(requested_peak, p.shape.max_context));
    if (resume && resume_kind == ExecutionUnitKind::Prefill)
        peak = std::min<std::uint64_t>(base.summary().prompt_tokens,
                                       std::uint64_t(target) + p.shape.prefill_chunk);
    if ((!resume || resume_kind == ExecutionUnitKind::Prefill) && base.plan_->vision)
        peak = base.plan_->vision->window_end(target, peak);
    std::vector<CheckpointHandle> retired;
    std::array<std::vector<const context::KVPrefixRef*>, 2> retired_prefixes;
    for (const auto point : candidate.retired_points) {
        if ((source && point == *source) ||
            std::find(retired.begin(), retired.end(), point) != retired.end())
            continue;
        if (!p.checkpoints.erasable(point)) return {.source_valid = false};
        retired.push_back(point);
        for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool)
            retired_prefixes[pool].push_back(&p.checkpoints.at(point).snapshot.prefix(pool));
    }
    if (consume)
        for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool)
            retired_prefixes[pool].push_back(&checkpoint->snapshot.prefix(pool));
    const auto released = p.footprint(retired, true);
    BindingReservation result;
    if ((!resident || (!consume && !split)) &&
        states.device_occupied() - released.state_slots == states.device_capacity())
        result.shortage.state_slots = 1;
    result.capacity_possible = true;
    for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool) {
        const auto& storage = p.resources.pool(pool);
        const auto* prefix  = checkpoint ? &checkpoint->snapshot.prefix(pool) : nullptr;
        const auto end      = pool == 0 ? peak : mtp_history_frontier(peak);
        const auto demand   = storage.histories.binding_demand(
            prefix, prefix ? prefix->frontier() : 0, end, retired_prefixes[pool]);
        const auto available =
            storage.cache.pages().available_pages() + released.kv_page_groups[pool];
        if (demand.device_pages() > available)
            result.shortage.kv_page_groups[pool] = demand.device_pages() - available;
        result.capacity_possible &=
            (end + 63) / 64 + demand.tail_copies <= p.pools[pool].device_page_groups;
    }
    if (result.shortage.state_slots || result.shortage.first_kv_deficit()) return result;

    detail::BindOperation action;
    action.lane           = lane.value;
    action.frontier       = f;
    action.target         = target;
    action.peak           = peak;
    action.request        = base.plan_;
    action.resume         = resume;
    action.source         = source;
    action.source_image   = image;
    action.consume_source = consume;
    using StateAction     = detail::StateBindingAction;
    action.state_action   = !source     ? StateAction::Root
                            : !resident ? StateAction::Restore
                            : consume   ? StateAction::Move
                            : split     ? StateAction::Split
                                        : StateAction::Fork;
    p.operation     = std::make_unique<detail::ContextOperation>(p.resources, std::move(action));
    auto& tx        = std::get<detail::BindOperation>(p.operation->action);
    auto& migration = p.operation->migration;
    try {
        if (source) tx.source_lease.emplace(p.checkpoints.lease(*source));
        tx.retired.reserve(retired.size() + 1);
        tx.carried.reserve(candidate.private_points.size());
        tx.clones.reserve(candidate.private_points.size());
        tx.carried_leases.reserve(candidate.private_points.size());
        for (const auto point : retired) {
            if (!p.checkpoints.erase(point))
                throw std::logic_error("binding retirement changed after its reservation");
            tx.retired.push_back(point);
        }
        for (const auto point : candidate.private_points) {
            if (!p.checkpoints.valid(point) || (source && point == *source && (consume || own)))
                continue;
            if (point != source) tx.carried_leases.push_back(p.checkpoints.lease(point));
            if (candidate.take_private)
                tx.carried.push_back(point);
            else if (const auto clone = p.checkpoints.reserve())
                tx.clones.emplace_back(point, *clone);
        }
        for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool) {
            const auto* prefix = checkpoint ? &checkpoint->snapshot.prefix(pool) : nullptr;
            migration.add_binding({pool}, p.resources.pool(pool).histories.prepare_binding(
                                              prefix, prefix ? prefix->frontier() : 0,
                                              pool == 0 ? peak : mtp_history_frontier(peak),
                                              lane.value, consume));
        }
        if (tx.state_action != StateAction::Move) {
            const bool device =
                tx.state_action == StateAction::Root || tx.state_action == StateAction::Fork;
            const auto destination =
                device ? states.reserve_destination() : states.reserve_logical_destination();
            if (!destination) throw std::logic_error("binding lost its state reservation");
            migration.own_state(states, *destination);
            if (tx.state_action == StateAction::Root)
                migration.clear_state(p.resources.states->storage(), states);
            else if (tx.state_action == StateAction::Restore) {
                auto transfer = states.reserve_host_to_device(image, *destination);
                if (!transfer) throw std::logic_error("binding lost its Host state source");
                migration.add_state_copy(std::move(*transfer), true);
            }
        }
        migration.submit();
        result.reserved       = true;
        result.retired_points = tx.retired;
        if (consume) result.consumed_source = source;
        return result;
    } catch (...) {
        p.cancel_operation();
        throw;
    }
}

ContextProgress Program::poll_context(runtime::CancellationFlagView cancellation) {
    auto& p = *impl_;
    if (!p.operation) throw std::logic_error("no context operation");
    auto& operation = *p.operation;
    auto& migration = operation.migration;
    ContextProgress result;
    result.kind = operation.kind();
    if (cancellation.requested()) migration.cancel();
    p.ple.progress();
    if (!migration.ready()) return result;
    result.transfers = migration.observations();
    result.complete  = true;
    if (migration.cancelled()) {
        p.cancel_operation();
        return result;
    }
    try {
        migration.publish();
        auto& states = *p.resources.state_store;
        if (auto* tx = std::get_if<detail::BindOperation>(&operation.action)) {
            tx->binding = tx->resume ? std::move(tx->resume->state_->sequence)
                                     : std::make_unique<detail::Sequence>();
            auto& s     = *tx->binding;
            s.lane      = tx->lane;
            if (!tx->resume) {
                s.id                      = p.next_sequence++;
                s.request                 = tx->request;
                s.identity                = *tx->request->identity;
                s.timings.prepare_seconds = tx->request->prompt.prepare.seconds;
                if (p.parameters.mtp) {
                    s.speculative.backend      = SpeculativeBackend::Mtp;
                    s.speculative.enabled      = true;
                    s.speculative.draft_window = p.shape.draft_tokens;
                    s.speculative.accepted_per_position.resize(p.shape.draft_tokens);
                }
            }
            s.epoch         = ++p.epochs[tx->lane];
            s.reused        = tx->resume ? s.reused : tx->frontier;
            s.resume_phase  = tx->resume ? s.phase : detail::Phase::Prefill;
            s.phase         = tx->target > tx->frontier ? detail::Phase::Replay : s.resume_phase;
            s.replay_target = tx->target;
            s.recovery_peak = tx->resume ? tx->peak : 0;
            s.permitted_frontier = tx->peak;
            s.continuation =
                tx->source ? p.checkpoints.at(*tx->source).snapshot.continuation() : Continuation{};
            s.continuation.metadata.rope_delta = s.request->prompt.rope_delta;
            if (!tx->source) {
                const auto eos          = p.parameters.model.config().text.ple.eos;
                s.continuation.metadata = {0,
                                           s.request->prompt.rope_delta,
                                           {eos, eos},
                                           p.resources.mtp ? std::optional<std::uint32_t>{0}
                                                           : std::nullopt};
            }
            while (s.next_capture < s.request->captures.size() &&
                   s.request->captures[s.next_capture].frontier <= tx->frontier)
                ++s.next_capture;
            for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool)
                s.histories[pool] = migration.binding({pool})->commit();
            using StateAction = detail::StateBindingAction;
            if (tx->state_action == StateAction::Move) {
                const auto source    = *tx->source;
                s.continuation.image = p.checkpoints.at(source).snapshot.take_mutable_image();
                tx->source_lease.reset();
                if (!p.checkpoints.erase(source))
                    throw std::logic_error("consumed checkpoint still has readers");
                tx->retired.push_back(source);
                tx->source.reset();
                ++result.operations.state_moves;
            } else {
                auto& destination = migration.state_destination();
                if (tx->state_action == StateAction::Fork) {
                    (void)destination.begin_write(tx->source_image);
                    s.write_destination = destination.release();
                    ++result.operations.state_forks;
                } else {
                    if (tx->state_action == StateAction::Split) {
                        states.split_device_replica_identity(tx->source_image, destination.image());
                        ++result.operations.state_moves;
                    }
                    states.complete_initialization(destination.image());
                    s.continuation.image = destination.release();
                    if (tx->state_action == StateAction::Restore)
                        ++result.operations.state_restores;
                }
            }
            for (const auto& [from, to] : tx->clones) {
                p.checkpoints.publish(to, p.checkpoints.at(from));
                tx->carried.push_back(to);
            }
            p.restore_counts(s);
            tx->source_lease.reset();
            if (tx->consume_source && tx->source) {
                if (!p.checkpoints.erase(*tx->source))
                    throw std::logic_error("consumed checkpoint still has readers");
                tx->retired.push_back(*tx->source);
            }
            if (tx->resume) tx->resume->state_.reset();
            result.operations.partial_tail_cow_pages = migration.tail_copy_page_groups();
            result.sequence                          = p.handle(s);
            result.replaying                         = s.phase == detail::Phase::Replay;
            result.private_points                    = std::move(tx->carried);
            result.retired_checkpoints               = std::move(tx->retired);
            p.sequences[tx->lane]                    = std::move(tx->binding);
            tx->clones.clear();
        } else if (auto* tx = std::get_if<detail::CaptureOperation>(&operation.action)) {
            auto& s = *p.sequences[tx->lane];
            result.captured_checkpoints.reserve(tx->reservation->records.size());
            for (const auto& [record, role] : tx->reservation->records) {
                p.checkpoints.publish(record, {*tx->snapshot, role});
                result.captured_checkpoints.push_back(record);
            }
            auto& destination = migration.state_destination();
            if (states.device_resident(destination.image())) {
                (void)destination.begin_write(s.continuation.image);
                s.write_destination = destination.release();
                ++result.operations.state_forks;
            } else {
                states.split_device_replica_identity(s.continuation.image, destination.image());
                states.complete_initialization(destination.image());
                s.continuation.image = destination.release();
            }
            tx->reservation->records.clear();
            s.capture_pending = false;
            ++s.next_capture;
        } else if (auto* tx = std::get_if<detail::PauseOperation>(&operation.action)) {
            result.request_timings     = tx->paused->sequence->timings;
            result.request_speculative = tx->paused->sequence->speculative;
            result.paused.emplace(ResumeState(std::move(tx->paused)));
        } else {
            result.operations.pressure_spill_pages = migration.spill_page_groups();
        }
        result.published = true;
        p.operation.reset();
        return result;
    } catch (...) {
        p.cancel_operation();
        throw;
    }
}

ResourceReservation Program::reserve_units(std::span<const ExecutionUnit> units) {
    auto& p = *impl_;

    struct Need {
        detail::Sequence* sequence;
        std::uint32_t end;
        std::array<std::uint32_t, 2> pages{};
    };

    std::vector<Need> needed;
    ContextResources additional;
    for (const auto& unit : units) {
        auto& s = p.sequence(unit.sequence);
        if (std::any_of(needed.begin(), needed.end(),
                        [&](const auto& n) { return n.sequence == &s; }))
            throw std::logic_error("execution reservation repeats a sequence");
        Need need{&s, std::max(s.recovery_peak, p.unit_end(s, unit.kind, unit.tokens))};
        if (need.end > p.shape.max_context) throw std::out_of_range("execution exceeds context");
        for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool) {
            const auto& histories = p.resources.pool(pool).histories;
            const auto handle     = s.histories[pool].handle();
            const auto begin      = pool == 0 ? s.continuation.metadata.main_frontier
                                              : s.continuation.metadata.mtp_frontier.value();
            const auto end        = pool == 0 ? need.end : mtp_history_frontier(need.end);
            need.pages[pool]      = histories.write_pages(handle, begin, end);
            const auto reserved   = histories.reserved_growth_pages(handle);
            additional.kv_page_groups[pool] +=
                need.pages[pool] > reserved ? need.pages[pool] - reserved : 0;
        }
        needed.push_back(need);
    }
    ContextResources shortage;
    for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool) {
        const auto free = p.resources.pool(pool).cache.pages().available_pages();
        if (additional.kv_page_groups[pool] > free)
            shortage.kv_page_groups[pool] = additional.kv_page_groups[pool] - free;
    }
    if (shortage.first_kv_deficit()) return {.shortage = shortage};
    for (std::size_t i = 0; i < units.size(); ++i) {
        auto& s = *needed[i].sequence;
        for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool)
            p.resources.pool(pool).histories.reserve_growth(s.histories[pool].handle(),
                                                            needed[i].pages[pool]);
        s.permit             = units[i];
        s.permitted_frontier = needed[i].end;
    }
    return {.reserved = true};
}

void Program::release_units(std::span<const SequenceHandle> handles) noexcept {
    try {
        for (const auto h : handles)
            if (impl_->valid(h)) impl_->settle(impl_->sequence(h));
    } catch (...) { std::terminate(); }
}

} // namespace ninfer::models::qwen4_exp

namespace ninfer::models::qwen4_exp::detail {

void ProgramImpl::cancel_operation() noexcept {
    if (!operation) return;
    try {
        auto& tx = *operation;
        tx.migration.drain();
        tx.migration.discard();
        if (auto* bind = std::get_if<BindOperation>(&tx.action)) {
            if (bind->binding) clear(*bind->binding);
            bind->source_lease.reset();
            bind->carried_leases.clear();
            for (const auto& [from, to] : bind->clones) {
                if (checkpoints.valid(to)) {
                    if (!checkpoints.erase(to)) std::terminate();
                } else
                    checkpoints.cancel(to);
            }
        } else if (auto* capture = std::get_if<CaptureOperation>(&tx.action)) {
            capture->reservation.reset();
            if (auto& sequence = sequences[capture->lane]) {
                auto& states     = *resources.state_store;
                const auto image = sequence->continuation.image;
                if (capture->snapshot && capture->snapshot->continuation().image == image) {
                    if (sequence->write_destination.valid()) {
                        states.abort_write(image, sequence->write_destination);
                        if (!states.release(sequence->write_destination)) std::terminate();
                        sequence->write_destination = {};
                    }
                    sequence->continuation.image = capture->snapshot->take_mutable_image();
                } else if (!capture->snapshot &&
                           states.role(image) == context::StateImageRole::CheckpointImmutable &&
                           !states.checkpoint_references(image)) {
                    states.make_mutable(image);
                }
                sequence->capture_pending = false;
            }
        } else if (auto* pause = std::get_if<PauseOperation>(&tx.action)) {
            if (pause->paused && pause->paused->sequence) clear(*pause->paused->sequence);
            if (sequences[pause->lane]) {
                clear(*sequences[pause->lane]);
                sequences[pause->lane].reset();
            }
        }
        operation.reset();
    } catch (...) { std::terminate(); }
}

} // namespace ninfer::models::qwen4_exp::detail
