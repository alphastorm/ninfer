#include "models/qwen4_exp/program/internal.h"

#include <algorithm>

namespace ninfer::models::qwen4_exp::detail {

std::optional<CheckpointHandle> ProgramImpl::detach(Sequence& s, runtime::CheckpointRole role) {
    const auto f = s.continuation.metadata.main_frontier;
    if (!s.request->can_checkpoint(f)) return {};
    auto identity = capture_identity(s);
    std::vector<context::KVPrefixRef> prefixes;
    for (std::uint16_t pool = 0; pool < resources.pool_count(); ++pool)
        prefixes.push_back(resources.pool(pool).histories.retain_prefix(
            s.histories[pool].handle(),
            pool == 0 ? f : s.continuation.metadata.mtp_frontier.value()));
    const auto record = checkpoints.reserve();
    if (!record) return {};
    auto& states     = *resources.state_store;
    const auto image = s.continuation.image;
    if (states.role(image) == context::StateImageRole::ActiveMutable) states.freeze(image);
    checkpoints.publish(
        *record,
        {Snapshot(states, s.continuation, std::move(prefixes), std::move(identity)), role});
    if (s.write_destination.valid()) {
        states.abort_write(image, s.write_destination);
        if (!states.release(s.write_destination))
            throw std::logic_error("unused state writer remains pinned");
        s.write_destination = {};
    }
    s.continuation.image = {};
    for (auto& history : s.histories) history.reset();
    return record;
}

} // namespace ninfer::models::qwen4_exp::detail

namespace ninfer::models::qwen4_exp {

std::optional<CapturePreparation> Program::prepare_capture(SequenceHandle h) {
    auto& p = *impl_;
    auto& s = p.sequence(h);
    if (!s.permit || !s.request->summary.publish_continuation) return {};
    if (s.recovery_peak) {
        s.capture.reset();
        while (s.next_capture < s.request->captures.size() &&
               s.request->captures[s.next_capture].frontier <= s.permitted_frontier)
            ++s.next_capture;
        return {};
    }
    if (s.capture) return CapturePreparation{s.request->captures[s.next_capture].frontier, true};
    while (s.next_capture < s.request->captures.size() &&
           s.request->captures[s.next_capture].frontier <= s.continuation.metadata.main_frontier)
        ++s.next_capture;
    if (s.next_capture == s.request->captures.size()) return {};
    const auto& point = s.request->captures[s.next_capture];
    if (point.frontier > s.permitted_frontier) return {};
    auto ticket         = std::make_unique<detail::CaptureReservation>();
    ticket->states      = p.resources.state_store.get();
    ticket->checkpoints = &p.checkpoints;
    ticket->records.reserve(point.roles.size());
    for (const auto role : point.roles) {
        const auto record = p.checkpoints.reserve();
        if (!record) return CapturePreparation{point.frontier, false};
        ticket->records.emplace_back(*record, role);
    }
    auto destination = ticket->states->reserve_destination();
    if (!destination && p.resources.host_context) {
        ticket->host =
            p.resources.host_context->allocate(p.resources.states->storage().image_bytes());
        if (ticket->host) destination = ticket->states->reserve_logical_destination();
    }
    if (!destination)
        return CapturePreparation{
            point.frontier, false, {.state_slots = 1}, p.resources.states->storage().image_bytes()};
    ticket->destination = *destination;
    s.capture           = std::move(ticket);
    return CapturePreparation{point.frontier, true};
}

bool Program::capture_is_input(SequenceHandle h) const {
    const auto& s = impl_->sequence(h);
    if (s.next_capture >= s.request->captures.size()) return false;
    const auto& point = s.request->captures[s.next_capture];
    return (s.capture_pending || (s.permit && point.frontier <= s.permitted_frontier)) &&
           std::find(point.roles.begin(), point.roles.end(),
                     runtime::CheckpointRole::InputReplay) != point.roles.end();
}

void Program::skip_capture(SequenceHandle h) {
    auto& s = impl_->sequence(h);
    s.capture.reset();
    s.capture_pending = false;
    if (s.next_capture < s.request->captures.size()) ++s.next_capture;
}

bool Program::reclaim_capture_reservation(ContextResources shortage) {
    for (auto& s : impl_->sequences) {
        if (!s || !s->capture || s->capture_pending) continue;
        if ((shortage.state_slots &&
             impl_->resources.state_store->device_resident(s->capture->destination)) ||
            (shortage.host_bytes && s->capture->host)) {
            s->capture.reset();
            return true;
        }
    }
    return false;
}

bool Program::start_capture(SequenceHandle h) {
    auto& p = *impl_;
    auto& s = p.sequence(h);
    if (p.operation || p.pending_transaction || !s.capture_pending || !s.capture ||
        s.write_destination.valid())
        return false;
    p.operation = std::make_unique<detail::ContextOperation>(
        p.resources, detail::CaptureOperation{s.lane, std::move(s.capture)});
    auto& migration   = p.operation->migration;
    auto& tx          = std::get<detail::CaptureOperation>(p.operation->action);
    auto& reservation = *tx.reservation;
    try {
        migration.own_state(*p.resources.state_store, reservation.destination);
        reservation.destination = {};
        std::vector<context::KVPrefixRef> prefixes;
        for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool)
            prefixes.push_back(p.resources.pool(pool).histories.retain_prefix(
                s.histories[pool].handle(), pool == 0
                                                ? s.continuation.metadata.main_frontier
                                                : s.continuation.metadata.mtp_frontier.value()));
        p.resources.state_store->freeze(s.continuation.image);
        tx.snapshot.emplace(*p.resources.state_store, s.continuation, std::move(prefixes),
                            s.request->identity);
        if (reservation.host) {
            auto copy = p.resources.state_store->reserve_device_to_host(
                s.continuation.image, std::move(*reservation.host));
            reservation.host.reset();
            if (!copy) throw std::logic_error("capture lost its Host reservation");
            migration.add_state_copy(std::move(*copy), true);
        }
        migration.submit();
    } catch (...) {
        p.cancel_operation();
        throw;
    }
    return true;
}

std::optional<std::size_t> Program::pause_host_bytes(SequenceHandle h) const {
    const auto& p = *impl_;
    const auto& s = p.sequence(h);
    if (!s.request->can_checkpoint(s.continuation.metadata.main_frontier) ||
        !p.resources.host_context)
        return {};
    std::size_t bytes = p.resources.state_store->host_resident(s.continuation.image)
                            ? 0
                            : p.resources.states->storage().image_bytes();
    for (std::uint16_t pool = 0; pool < p.resources.pool_count(); ++pool) {
        const auto& storage = p.resources.pool(pool);
        const auto frontier = pool == 0 ? s.continuation.metadata.main_frontier
                                        : s.continuation.metadata.mtp_frontier.value();
        for (std::uint32_t i = 0; i < (frontier + 63) / 64; ++i) {
            const auto page = storage.histories.logical_page(s.histories[pool].handle(), i);
            if (storage.pages.active_history_references(page) <= 1 &&
                !storage.pages.host_replica_current(page))
                bytes += p.pools[pool].host_page_stride;
        }
    }
    return bytes;
}

bool Program::start_pause(SequenceHandle h, bool save, runtime::ExecutionTiming* observation) {
    auto& p = *impl_;
    if (p.operation || p.pending_transaction) return false;
    auto& s = p.sequence(h);
    if (s.phase != detail::Phase::Prefill && s.phase != detail::Phase::Replay &&
        s.phase != detail::Phase::Active)
        throw std::logic_error("pause requires a committed boundary");
    if (save) {
        const auto bytes = pause_host_bytes(h);
        if (!bytes || *bytes > p.resources.host_context->free_bytes()) return false;
    }
    runtime::ExecutionTimingRecorder timing(runtime::ExecutionTimingPhase::Submit, observation);
    timing.begin_wait();
    p.ple.wait_compute();
    timing.end_wait();
    auto paused   = std::make_unique<detail::Suspended>();
    paused->owner = &p;
    paused->frontier =
        s.phase == detail::Phase::Replay ? s.replay_target : s.continuation.metadata.main_frontier;
    p.operation = std::make_unique<detail::ContextOperation>(
        p.resources, detail::PauseOperation{h.lane, std::move(paused)});
    auto& tx        = std::get<detail::PauseOperation>(p.operation->action);
    auto& migration = p.operation->migration;
    try {
        s.capture.reset();
        s.capture_pending = false;
        s.recovery_peak   = 0;
        p.retire_vision(s);
        p.settle(s);
        if (s.phase == detail::Phase::Replay) s.phase = s.resume_phase;
        if (save) tx.paused->snapshot = p.detach(s, runtime::CheckpointRole::Continuation);
        if (!tx.paused->snapshot) p.clear(s);
        p.ple.cancel(s.id);
        tx.paused->sequence = std::move(p.sequences[h.lane]);
        if (tx.paused->snapshot) {
            const auto demotion =
                p.demotion({&*tx.paused->snapshot, 1},
                           {.state_slots = 1, .kv_page_groups = {UINT32_MAX, UINT32_MAX}});
            if (demotion && !p.prepare_demotion(migration, *demotion)) {
                migration.reset_plan();
                if (!p.checkpoints.erase(*tx.paused->snapshot))
                    throw std::logic_error("failed pause backup still has readers");
                tx.paused->snapshot.reset();
            }
        }
        migration.submit();
        return true;
    } catch (...) {
        p.cancel_operation();
        throw;
    }
}

bool Program::revoke_snapshot(ResumeState& state) noexcept {
    if (!state.state_ || state.state_->owner != impl_.get() || !state.state_->snapshot)
        return false;
    if (!impl_->checkpoints.erase(*state.state_->snapshot)) return false;
    state.state_->snapshot.reset();
    return true;
}

ContextResources Program::snapshot_resources(const ResumeState& s) const {
    return s.has_snapshot() ? impl_->footprint({&*s.state_->snapshot, 1}, true)
                            : ContextResources{};
}

} // namespace ninfer::models::qwen4_exp
