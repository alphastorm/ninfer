// Included inside EngineCore: every Program operation, including lease destruction, runs on
// its device-bound worker. Disk publication uses only CheckpointControl's independent gate.
    // An operation-free call is a durable prepare that a context operation in flight would refuse
    // as a busy transaction. Off the worker it waits in the worker for the next operation-free
    // boundary instead; an inline worker call runs at once.
    template <class Function>
    auto checkpoint_call(Function&& function, bool operation_free = false)
        -> std::invoke_result_t<Function> {
        if (std::this_thread::get_id() == worker_.get_id()) {
            struct Relock {
                std::unique_lock<std::mutex>& lock;
                bool acquired;
                ~Relock() { if (acquired) { lock.unlock(); } }
            } scope{*checkpoint_execution_lock_, !checkpoint_execution_lock_->owns_lock()};
            if (scope.acquired) { scope.lock.lock(); }
            return std::forward<Function>(function)();
        }
        return reserve_checkpoint_command(std::forward<Function>(function), operation_free)();
    }

    void set_pressure_checkpoint_handler(std::shared_ptr<PressureCheckpointHandler> handler) {
        std::lock_guard providers(checkpoint_provider_mutex_);
        const bool enabled = bool(handler);
        checkpoints_.set_handler(std::move(handler));
        resources_.enable_release_guard(enabled);
        request_admission_check();
        queue_cv_.notify_all();
    }
    void set_reclaimable_session_oracle(std::shared_ptr<const ReclaimableSessionOracle> oracle) {
        checkpoints_.set_oracle(std::move(oracle));
        request_admission_check();
        queue_cv_.notify_all();
    }
    void notify_checkpoint_progress() noexcept {
        checkpoints_.notify_progress(false);
        admission_check_pending_.store(true, std::memory_order_release);
        queue_cv_.notify_all();
    }
    [[nodiscard]] std::uint64_t checkpoint_progress() const noexcept {
        return checkpoints_.observed_progress();
    }
    void response_stored(std::string_view digest, std::string_view tag) {
        checkpoint_call([&] {
            const auto order = resources_.completed_checkpoint_order(digest, tag);
            if (!order) { return; }
            const auto state = checkpoints_.session(digest);
            std::lock_guard lock(state->mutex);
            state->stored(std::string(tag), order);
            // Also clears an older owner that completed after a newer acknowledgement.
            resources_.supersede_durable(digest, state->committed_order);
        });
        request_admission_check();
        queue_cv_.notify_all();
    }
    [[nodiscard]] DurableSessionEpoch session_epoch(std::string_view digest) {
        return checkpoints_.epoch(digest);
    }
    [[nodiscard]] DurablePublicationResult publish_export(
        std::string_view digest, std::string_view tag, DurableSessionEpoch epoch,
        const std::function<bool()>& callback) {
        return checkpoints_.publish(digest, tag, epoch, callback);
    }
    [[nodiscard]] DurableNativeLayout checkpoint_layout() {
        if constexpr (requires { instance_.program->checkpoint_layout(); }) {
            return checkpoint_call([&] { return instance_.program->checkpoint_layout(); });
        } else {
            throw std::invalid_argument("native checkpoint codec is unavailable");
        }
    }

    [[nodiscard]] bool admission_capacity_progress_pending(bool restoring) const {
        return !resident_empty() || (!restoring && !paused_.empty()) ||
               checkpoint_exports_in_flight_ != 0;
    }

    [[nodiscard]] DurableExportResult export_session(
        std::string_view digest, std::string_view tag, ContinuationCheckpointWriter& writer,
        const DurableLimits& limits, DurableSaveIntent intent = DurableSaveIntent::Automatic,
        DurableExportReclaim* summary = nullptr) {
        using Program = typename ModelContract::Program;
        if constexpr (!requires(Program& program, std::span<const Checkpoint> points) {
            program.prepare_checkpoint_export(points, limits);
        }) {
            return {.reason = DurableReason::UnsupportedCodec};
        } else {
            if (!checkpoint_cache_enabled_) { return {.reason = DurableReason::CacheDisabled}; }
            if (tag.empty()) { return {.reason = DurableReason::EmptyTag}; }
            using Lease = decltype(instance_.program->prepare_checkpoint_export(
                std::span<const Checkpoint>{}, limits));
            std::optional<Lease> lease;
            bool prepared_export = false;
            std::optional<std::chrono::steady_clock::time_point> staged_since;
            DurableExportResult result;
            const auto release_export = [&] {
                checkpoint_change([&] {
                    // Native readers can pin every idle Device image while the writer
                    // runs off-lock. Retire them before advertising usable capacity.
                    lease.reset();
                    if (prepared_export) {
                        prepared_export = false;
                        --checkpoint_exports_in_flight_;
                        request_admission_check();
                        queue_cv_.notify_all();
                    }
                });
            };
            const bool worker = std::this_thread::get_id() == worker_.get_id();
            // Reserve the task, future and queue node before acquiring any native readers.
            // Enqueueing this command after either writer exit cannot allocate.
            std::optional<decltype(reserve_checkpoint_command(release_export))> retirement;
            if (!worker) { retirement.emplace(reserve_checkpoint_command(release_export)); }
            const auto retire_export = [&] {
                if (retirement) { (*retirement)(); }
                else { release_export(); }
            };
            // Prepared only at an operation-free boundary: never refused for an operation in
            // flight. The retirement above is an ordinary command and is never held back.
            checkpoint_change([&] {
                const auto gate = checkpoints_.session(digest);
                auto& program = *instance_.program;
                using Manager = std::remove_reference_t<decltype(resources_)>;
                std::optional<typename Manager::ReclaimCursor> cursor;
                std::vector<typename Manager::DurableVictim> victims;
                std::vector<std::vector<Checkpoint>> closures;
                std::vector<Checkpoint> excluded;
                std::size_t victim_index = 0, ordinary_steps = 0, ordinary_bound = 0;
                bool redundant_used = false, contiguity_retry = false, changed = false;
                const auto oracle = (intent == DurableSaveIntent::Explicit || intent == DurableSaveIntent::Final)
                    ? checkpoints_.oracle() : nullptr;
                const auto notify = [&] {
                    if (changed) {
                        notify_checkpoint_progress();
                    }
                };
                // The Host route, including its reclaim, stays first. Once it is exhausted with a
                // Host shortage, one Device-staged attempt reads the current points again.
                bool device_staging = false;
                constexpr bool can_stage = requires(Program& staged, std::span<const Checkpoint> points) {
                    staged.prepare_checkpoint_export(points, limits, true);
                };
                const auto fall_back = [&](bool host_shortage) {
                    if (!can_stage || !host_shortage || device_staging) { return false; }
                    device_staging = result.reclaim.device_staging_attempted = true;
                    return true;
                };
                try {
                    for (;;) {
                        if (failed_) { result.reason = DurableReason::IoUnavailable; break; }
                        std::vector<Checkpoint> points;
                        {
                            std::lock_guard lock(gate->mutex);
                            result.captured_epoch = {gate->epoch.value, gate};
                            bool pending = false;
                            points = resources_.session_points(program, digest, tag, pending);
                            if (points.empty()) {
                                result.reason = pending ? DurableReason::TransactionBusy
                                                        : DurableReason::SessionNotIndexed;
                                break;
                            }
                            if (gate->tag != tag) {
                                result.state = DurableExportState::Stale;
                                result.reason = DurableReason::TagMismatch;
                                break;
                            }
                            if (!device_staging) { ++result.reclaim.prepare_attempts; }
                            if constexpr (can_stage) {
                                lease.emplace(program.prepare_checkpoint_export(points, limits, device_staging));
                            } else {
                                lease.emplace(program.prepare_checkpoint_export(points, limits));
                            }
                        }
                        result.reason = lease->reason();
                        if (lease->state() == DurablePrepareState::Prepared) {
                            prepared_export = true;
                            ++checkpoint_exports_in_flight_;
                            result.reclaim.capacity_site = DurableCapacitySite::None;
                            if (device_staging) {
                                staged_since = std::chrono::steady_clock::now();
                                if constexpr (requires { lease->device_staged_states(); }) {
                                    result.reclaim.device_staged_states = lease->device_staged_states();
                                }
                            }
                            break;
                        }
                        const auto state = lease->state();
                        const auto kind = lease->host_shortage_kind();
                        using Kind = decltype(kind);
                        const auto shortage = lease->host_shortage_bytes();
                        classify_refusal(*lease, !device_staging, result.reclaim);
                        lease.reset();
                        const bool host_shortage = state == DurablePrepareState::CapacityRefused &&
                            result.reason == DurableReason::Capacity &&
                            (kind == Kind::HostContiguity || (kind == Kind::HostCapacity && shortage));
                        if (device_staging || !host_shortage || checkpoint_pressure_depth_ ||
                            contiguity_retry) {
                            if (fall_back(host_shortage)) { continue; }
                            break;
                        }
                        result.reclaim.considered = true;
                        const auto before = program.physical_usage().occupied.host_bytes;
                        struct AccountProgress {
                            Program& program;
                            DurableExportReclaim& summary;
                            std::size_t before;
                            ~AccountProgress() {
                                const auto after = program.physical_usage().occupied.host_bytes;
                                if (before > after) summary.reclaimed_host_bytes += before - after;
                            }
                        } account{program, result.reclaim, before};
                        const auto freed = [&] {
                            const auto now = program.physical_usage().occupied.host_bytes;
                            return before > now ? before - now : 0;
                        };
                        const auto lossless_excluded = resources_.export_reclaim_exclusions(points, false);
                        if (!redundant_used) {
                            redundant_used = true;
                            changed = program.release_redundant_host(lossless_excluded) || changed;
                        }
                        if (kind == Kind::HostContiguity) {
                            if (!freed()) {
                                if (fall_back(true)) { continue; }
                                break;
                            }
                            contiguity_retry = true;
                            continue;
                        }
                        if (!cursor) {
                            cursor.emplace(resources_.begin_reclaim(
                                program, std::nullopt, {ReclaimPurpose::OptionalWrite}));
                            ordinary_bound = cursor->victims.size();
                            if (oracle) victims = resources_.durable_victims(digest);
                            result.proof_pins.reserve(2 * victims.size());
                            excluded = resources_.export_reclaim_exclusions(points, true);
                            closures.reserve(victims.size());
                            for (const auto& victim : victims)
                                closures.push_back(resources_.proven_victim_closure(
                                    program, victim, *cursor, points, excluded));
                        }
                        while (freed() < shortage && victim_index < victims.size()) {
                            const auto& closure = closures[victim_index];
                            const auto& victim = victims[victim_index++];
                            bool released = false;
                            if (!closure.empty()) {
                                const auto prove = [&]() {
                                    auto pin = oracle->pin_recoverable(victim.session, victim.tag);
                                    if (!pin) return false;
                                    auto verification = pin->fresh_reader();
                                    if (!verification) return false;
                                    DurableCheckpointSet set;
                                    if (read_checkpoint_index(*verification, limits, set) != DurableReason::None)
                                        return false;
                                    const auto validated = program.validate_checkpoint_import(set, *verification, limits);
                                    if (validated.state != decltype(validated.state)::Validated) return false;
                                    auto current = oracle->pin_recoverable(victim.session, victim.tag);
                                    if (!current) return false;
                                    const auto a = pin->file_descriptor("engine/index.cbor");
                                    const auto b = current->file_descriptor("engine/index.cbor");
                                    if (!a || !b || a->bytes != b->bytes || a->sha256 != b->sha256) return false;
                                    result.proof_pins.push_back(std::move(pin));
                                    result.proof_pins.push_back(std::move(current));
                                    return resources_.release_proven_durable_victim(
                                        program, victim, closure, *cursor, points, excluded);
                                };
                                released = prove();
                            }
                            if (released) {
                                changed = true;
                                ++result.reclaim.reclaimed_owners;
                            }
                            else ++result.reclaim.reclaim_declined;
                        }
                        while (freed() < shortage && ordinary_steps < ordinary_bound) {
                            const auto progress = resources_.reclaim(
                                program, {.host_bytes = shortage - freed()}, excluded, *cursor);
                            if (progress != ReclaimProgress::Changed) break;
                            changed = true;
                            ++ordinary_steps;
                            ++result.reclaim.reclaimed_cache_steps;
                        }
                        if (!freed()) {
                            if (fall_back(true)) { continue; }
                            break;
                        }
                    }
                } catch (const std::bad_alloc&) {
                    // Optional reclaim cannot turn a capacity refusal into an Engine failure.
                    lease.reset();
                    if (!result.reclaim.considered || result.reason != DurableReason::Capacity) {
                        notify();
                        throw;
                    }
                } catch (...) {
                    lease.reset();
                    notify();
                    throw;
                }
                notify();
            }, /*operation_free=*/true);
            if (summary) { *summary = result.reclaim; }
            if (!lease) { return result; }
            // Gate hold of a Device-staged export, measured once its native pins retire.
            const auto staged_hold = [&] {
                if (!staged_since) { return; }
                result.reclaim.device_staged_hold_ms = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - *staged_since).count());
                if (summary) { summary->device_staged_hold_ms = result.reclaim.device_staged_hold_ms; }
            };
            try {
                // Inline worker saves use the same off-execution-lock writer boundary as an
                // external control caller. The immutable lease alone owns copied content.
                std::unique_lock<std::mutex>* execution =
                    worker && checkpoint_execution_lock_->owns_lock() ? checkpoint_execution_lock_ : nullptr;
                if (execution) { execution->unlock(); }
                try {
                    const auto epoch = result.captured_epoch;
                    const auto reclaim = result.reclaim;
                    auto pins = std::move(result.proof_pins);
                    result = lease->write(writer);
                    result.captured_epoch = epoch;
                    result.reclaim = reclaim;
                    result.proof_pins = std::move(pins);
                } catch (...) {
                    if (execution) { execution->lock(); }
                    throw;
                }
                if (execution) { execution->lock(); }
            } catch (...) {
                retire_export();
                staged_hold();
                throw;
            }
            retire_export();
            staged_hold();
            return result;
        }
    }

    [[nodiscard]] PreparedEngineCheckpoint prepare_import(
        const DurableImportTarget& target, const ContinuationCheckpointReader& reader,
        const RuntimeFingerprintV2& fingerprint, const DurableCheckpointStats& expected,
        const DurableLimits& limits) {
        // Policy is evaluated before any queue, Reader access, allocation or reservation.
        if (!checkpoint_cache_enabled_ || !target.allow_prefix_reuse ||
            !target.cache_participation) { return {}; }
        using Program = typename ModelContract::Program;
        if constexpr (!requires(Program& program, const DurableCheckpointSet& set) {
            program.prepare_checkpoint_import(set, reader, limits);
        }) {
            return {.state = DurablePrepareState::PolicyDisabled,
                    .reason = DurableReason::UnsupportedCodec};
        } else {
            using Reservation = decltype(instance_.program->prepare_checkpoint_import(
                std::declval<const DurableCheckpointSet&>(), reader, limits));
            struct Pending {
                std::optional<Reservation> native;
                DurableImportTarget target;
                // The gate that validated the epoch; commit rechecks under this same gate.
                std::shared_ptr<CheckpointControl::Session> gate;
                std::size_t count = 0;
            };
            auto pending = std::make_shared<Pending>();
            pending->target = target;
            PreparedEngineCheckpoint result;
            const auto oracle = checkpoints_.oracle();
            // Reclaimed victims and a prepared reservation change catalog and Host occupancy.
            // Like an export, prepared only at an operation-free boundary (a busy refusal here
            // would drop a restore-on-demand and its continuation's parent response).
            checkpoint_change([&] {
                auto observed_limits = limits;
                observed_limits.restore_timing = &cumulative_stats_.checkpoint_restore;
                if (failed_) {
                    result.state = DurablePrepareState::Unavailable;
                    result.reason = DurableReason::IoUnavailable;
                    return;
                }
                const auto gate = checkpoints_.session(target.session_sha256);
                {
                    std::lock_guard lock(gate->mutex);
                    if (gate->epoch != target.expected_epoch) {
                        result.reason = DurableReason::StalePublication;
                        return;
                    }
                }
                pending->gate = gate;
                // Fixed initial membership bounds the retry budget. Later arrivals do not
                // make a restore an unbounded eviction loop. Every victim lease survives
                // all attempts, even after its resident points have been reclaimed.
                const auto victims = resources_.durable_victims(target.session_sha256);
                std::vector<std::shared_ptr<const ContinuationCheckpointReader>> pins;
                pins.reserve(2 * victims.size());
                std::size_t victim_index = 0;
                const std::size_t attempts = 2 * victims.size() + 1;
                std::shared_ptr<const ContinuationCheckpointReader> fresh;
                for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
                    const auto& input = fresh ? *fresh : reader;
                    DurableCheckpointSet set;
                    const auto reason = read_checkpoint_index(input, observed_limits, set);
                    if (reason != DurableReason::None) {
                        result.state = reason == DurableReason::IoUnavailable
                            ? DurablePrepareState::Unavailable : DurablePrepareState::Corrupt;
                        result.reason = reason;
                        return;
                    }
                    if (fingerprint.native_layout != instance_.program->checkpoint_layout()) {
                        result.state = DurablePrepareState::IdentityMismatch;
                        result.reason = DurableReason::FingerprintMismatch;
                        return;
                    }
                    pending->native.emplace(instance_.program->prepare_checkpoint_import(
                        set, input, observed_limits));
                    result.state = pending->native->state();
                    result.reason = pending->native->reason();
                    if (result.state == DurablePrepareState::Prepared) {
                        const auto stats = pending->native->stats();
                        // K1's manifest attests these three values. Point/state/page counts
                        // are native index-derived result statistics, not manifest fields.
                        if (stats.frontier_tokens != expected.frontier_tokens ||
                            stats.restored_tokens != expected.restored_tokens ||
                            stats.payload_bytes != expected.payload_bytes) {
                            pending->native.reset();
                            result.state = DurablePrepareState::Corrupt;
                            result.reason = DurableReason::PayloadSizeMismatch;
                            return;
                        }
                        pending->count = set.points.size();
                        return;
                    }
                    pending->native.reset();
                    if (result.state != DurablePrepareState::CapacityRefused ||
                        result.reason != DurableReason::Capacity || !oracle) { return; }
                    bool reclaimed = false;
                    while (victim_index < victims.size() && !reclaimed) {
                        const auto& victim = victims[victim_index++];
                        auto pin = oracle->pin_recoverable(victim.session, victim.tag);
                        if (!pin) { continue; }
                        auto verification = pin->fresh_reader();
                        if (!verification) { continue; }
                        DurableCheckpointSet victim_set;
                        if (read_checkpoint_index(*verification, limits, victim_set) != DurableReason::None) {
                            continue;
                        }
                        const auto validated = instance_.program->validate_checkpoint_import(
                            victim_set, *verification, limits);
                        if (validated.state != decltype(validated.state)::Validated) { continue; }
                        // Validate the *current* pointer again after the expensive byte proof.
                        // This pin is held alongside the verified generation until restore ends.
                        auto current = oracle->pin_recoverable(victim.session, victim.tag);
                        if (!current) { continue; }
                        const auto verified_index = pin->file_descriptor("engine/index.cbor");
                        const auto current_index = current->file_descriptor("engine/index.cbor");
                        if (!verified_index || !current_index ||
                            verified_index->bytes != current_index->bytes ||
                            verified_index->sha256 != current_index->sha256) { continue; }
                        pins.push_back(std::move(pin));
                        pins.push_back(std::move(current));
                        reclaimed = resources_.release_durable_victim(*instance_.program, victim);
                        if (reclaimed) {
                            request_admission_check();
                        }
                    }
                    if (!reclaimed) { return; }
                    fresh = reader.fresh_reader();
                    if (!fresh) {
                        result.state = reader.failure_reason() == DurableReason::IoUnavailable
                            ? DurablePrepareState::Unavailable : DurablePrepareState::Corrupt;
                        result.reason = reader.failure_reason() == DurableReason::None
                            ? DurableReason::InvalidIndex : reader.failure_reason();
                        return;
                    }
                }
            }, /*operation_free=*/true);
            if (result.state != DurablePrepareState::Prepared) { return result; }
            try {
                result.abort = [this, pending] {
                    if (!pending->native) { return; }
                    checkpoint_change([&] { pending->native.reset(); });
                };
                result.commit = [this, pending]() -> DurableImportResult {
                    return checkpoint_change([&]() -> DurableImportResult {
                        if (!pending->native) {
                            return {.state = DurableImportState::PolicyDisabled,
                                    .reason = DurableReason::Cancelled};
                        }
                        const auto& target = pending->target;
                        // Allocate every policy/index replacement before native publication.
                        auto publication = resources_.prepare_import_publication(
                            checkpoint_key("http:" + target.session_sha256),
                            target.shared_reuse_scope
                                ? std::optional(checkpoint_key(*target.shared_reuse_scope)) : std::nullopt,
                            target.session_sha256, target.expected_tag, pending->count);
                        std::string tag = target.expected_tag;
                        if (failed_) {
                            pending->native.reset();
                            return {.state = DurableImportState::Unavailable,
                                    .reason = DurableReason::IoUnavailable};
                        }
                        const auto stats = pending->native->stats();
                        std::uint64_t order;
                        {
                            std::lock_guard queue(queue_mutex_);
                            if (!next_publication_order_) { throw std::overflow_error("publication order exhausted"); }
                            order = next_publication_order_++;
                        }
                        if (!checkpoints_.commit_import(pending->gate, target.expected_epoch, tag, order, [&] {
                            auto handles = instance_.program->commit_import(std::move(*pending->native));
                            pending->native.reset();
                            resources_.publish_import(*instance_.program, std::move(publication), handles, order);
                        })) {
                            pending->native.reset();
                            return {.state = DurableImportState::PolicyDisabled,
                                    .reason = DurableReason::StalePublication};
                        }
                        request_admission_check();
                        return {DurableImportState::Imported, DurableReason::None, stats};
                    });
                };
            } catch (...) {
                checkpoint_change([&] { pending->native.reset(); });
                throw;
            }
            return result;
        }
    }

private:
    // One worker command. An operation-free command never meets an in-flight context operation:
    // the runner holds it, in arrival order, until the next operation-free boundary.
    struct CheckpointCommand {
        std::function<void()> run;
        bool operation_free = false;
    };
    // Own every fallible part of a worker command before it can become mandatory cleanup.
    template <class Function>
    auto reserve_checkpoint_command(Function&& function, bool operation_free = false) {
        using Result = std::invoke_result_t<Function>;
        auto command = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(function));
        auto result = command->get_future();
        std::list<CheckpointCommand> commands;
        // Only an off-worker caller's command is ever held: a command the worker itself queued
        // would wait on its own thread. (Worker calls run inline and do not reserve today.)
        const bool hold = operation_free && std::this_thread::get_id() != worker_.get_id();
        commands.push_back(CheckpointCommand{[command] { (*command)(); }, hold});
        return [this, commands = std::move(commands), result = std::move(result)]() mutable -> Result {
            {
                std::lock_guard lock(queue_mutex_);
                if (stopping_) { throw std::runtime_error("checkpoint worker is stopping"); }
                checkpoint_commands_.splice(checkpoint_commands_.end(), commands);
            }
            queue_cv_.notify_one();
            return result.get();
        };
    }
    // Runs a checkpoint operation that changes catalog entries or physical occupancy. As with a
    // terminal response, its result or exception reaches the caller only after the published
    // runtime statistics include that change.
    template <class Function>
    auto checkpoint_change(Function&& function, bool operation_free = false)
        -> std::invoke_result_t<Function> {
        return checkpoint_call([&]() -> std::invoke_result_t<Function> {
            using Core = std::remove_pointer_t<decltype(this)>;
            const struct Publication {
                Core& core;
                ~Publication() { core.publish_checkpoint_change(); }
            } publication{*this};
            return std::forward<Function>(function)();
        }, operation_free);
    }
    // A committed import or a retired lease cannot be undone, so a failed publication still
    // returns the operation's result. The worker then fails the Engine with that failure, as it
    // does after a failed terminal-response publication.
    void publish_checkpoint_change() noexcept {
        try {
            publish_runtime_stats();
        } catch (...) {
            if (!checkpoint_publication_failure_) {
                checkpoint_publication_failure_ = std::current_exception();
            }
        }
    }
    DurableReason validate_pressure_checkpoint(const ContinuationCheckpointReader& reader) {
        using Program = typename ModelContract::Program;
        const DurableLimits limits;
        if constexpr (requires(Program& program, const DurableCheckpointSet& set) {
            program.validate_checkpoint_import(set, reader, limits);
        }) {
            DurableCheckpointSet set;
            const auto reason = read_checkpoint_index(reader, limits, set);
            if (reason != DurableReason::None) { return reason; }
            const auto result = instance_.program->validate_checkpoint_import(set, reader, limits);
            return result.state == decltype(result.state)::Validated
                ? DurableReason::None
                : result.reason == DurableReason::None ? DurableReason::InvalidIndex : result.reason;
        } else {
            return DurableReason::UnsupportedCodec;
        }
    }
    void run_checkpoint_commands() {
        {
            std::lock_guard lock(queue_mutex_);
            checkpoint_command_batch_.swap(checkpoint_commands_);
        }
        // Held commands keep their place ahead of every command that arrived after them.
        checkpoint_command_batch_.splice(checkpoint_command_batch_.begin(), checkpoint_held_commands_);
        run_command_batch();
        // Every worker turn starts here. Gates that finished requests, operations and dropped
        // tokens left behind retire once no native owner names their session; native
        // ownership is only stable on this worker.
        checkpoints_.retire_unowned([&](std::string_view digest) {
            return resources_.holds_durable_session(digest);
        });
        checkpoint_command_batch_.clear();
    }
    // The worker calls this right after the turn's first completion poll: a command held behind
    // the operation that poll completed runs before anything in the turn can start another.
    void run_held_checkpoint_commands() {
        if (checkpoint_held_commands_.empty() || hold_for_context_operation()) { return; }
        checkpoint_command_batch_.splice(checkpoint_command_batch_.end(), checkpoint_held_commands_);
        run_command_batch();
        checkpoint_command_batch_.clear();
    }
    void run_command_batch() {
        for (auto it = checkpoint_command_batch_.begin(); it != checkpoint_command_batch_.end();) {
            if (it->operation_free && hold_for_context_operation()) {
                checkpoint_held_commands_.splice(checkpoint_held_commands_.end(),
                                                 checkpoint_command_batch_, it++);
                continue;
            }
            it->run();
            ++it;
        }
    }
    // A durable prepare refuses with TransactionBusy while a context operation is in flight;
    // such a command waits for the operation instead. After an Engine failure nothing completes
    // an operation, so a held command runs at once and refuses with IoUnavailable.
    [[nodiscard]] bool hold_for_context_operation() const noexcept {
        if constexpr (requires { instance_.program->has_context_operation(); }) {
            return !failed_ && instance_.program->has_context_operation();
        } else {
            return false;
        }
    }
    // The next operation-free boundary is taken: a Pending pressure save, or a durable prepare
    // held until the operation in flight completes. Admission, resident reclaim and capture
    // preparation wait on either, a capture start only on a held prepare: every start site waits
    // while one is held, so it waits for at most the one operation in flight when it arrived.
    // Held-with-no-operation ends at the next top-of-turn runner, which runs every held command.
    [[nodiscard]] bool operation_boundary_taken() const noexcept {
        return checkpoints_.pending() || !checkpoint_held_commands_.empty();
    }
    bool checkpoint_pressure(std::span<PressureCheckpointVictim> victims) {
        if (victims.empty()) { return true; }
        using Core = std::remove_pointer_t<decltype(this)>;
        struct PressureScope {
            Core& core;
            ~PressureScope() {
                core.checkpoint_execution_lock_->lock();
                --core.checkpoint_pressure_depth_;
            }
        } scope{*this};
        ++checkpoint_pressure_depth_;
        checkpoint_execution_lock_->unlock();
        return checkpoints_.settle(victims);
    }
    bool checkpoint_request_settled(const Request& request) {
        // A paused request has not acknowledged a final response. Native owns its
        // retained prompt/progress and may revoke the snapshot in favour of replay.
        if (!request.is_model_finished()) { return true; }
        if (request.checkpoint_session.empty() || request.checkpoint_tag.empty()) { return true; }
        PressureCheckpointVictim victim{request.checkpoint_session, request.checkpoint_tag};
        return checkpoint_pressure({&victim, 1});
    }
    // Receipt classification of a refused native attempt: the Host-route shortage (Host-route
    // attempts only, so a staged refusal keeps the shortage that led to it) and the native site
    // of a Capacity refusal. Never persisted.
    template <class Lease>
    static void classify_refusal(const Lease& lease, bool host_route, DurableExportReclaim& summary) noexcept {
        if (host_route) {
            using Kind = decltype(lease.host_shortage_kind());
            const auto kind = lease.host_shortage_kind();
            summary.host_shortage = kind == Kind::HostCapacity     ? DurableHostShortage::HostCapacity
                                    : kind == Kind::HostContiguity ? DurableHostShortage::HostContiguity
                                                                   : DurableHostShortage::None;
            if constexpr (requires { Kind::Busy; }) {
                if (kind == Kind::Busy) { summary.host_shortage = DurableHostShortage::Busy; }
            }
            if constexpr (requires { Kind::NoHostArena; }) {
                if (kind == Kind::NoHostArena) { summary.host_shortage = DurableHostShortage::NoHostArena; }
            }
        }
        summary.capacity_site = DurableCapacitySite::None;
        if constexpr (requires { lease.capacity_site(); }) { summary.capacity_site = lease.capacity_site(); }
    }
    static typename ModelContract::CacheSessionKey checkpoint_key(std::string_view value) {
        typename ModelContract::CacheSessionKey key;
        if (value.size() > key.bytes.size()) { throw std::invalid_argument("checkpoint scope exceeds capacity"); }
        key.size = static_cast<std::uint16_t>(value.size());
        std::copy(value.begin(), value.end(), key.bytes.begin());
        return key;
    }
    static DurableReason read_checkpoint_index(const ContinuationCheckpointReader& reader,
                                               const DurableLimits& limits,
                                               DurableCheckpointSet& set) {
        CheckpointRestoreTimer timing(limits.restore_timing,
                                     &RuntimeCheckpointRestoreStats::index_read_ns);
        const auto bytes = reader.file_size("engine/index.cbor");
        if (!bytes || !*bytes || *bytes > limits.manifest_bytes || *bytes > limits.staging_bytes ||
            *bytes > std::numeric_limits<std::size_t>::max()) { return DurableReason::InvalidIndex; }
        std::vector<std::byte> data(static_cast<std::size_t>(*bytes));
        if (!reader.read_file("engine/index.cbor", 0, data)) {
            return reader.failure_reason() == DurableReason::None
                ? DurableReason::InvalidIndex : reader.failure_reason();
        }
        try {
            set = ModelContract::Program::decode_checkpoint_index(data, limits);
        } catch (const std::invalid_argument&) { return DurableReason::InvalidIndex; }
        return DurableReason::None;
    }
public:
