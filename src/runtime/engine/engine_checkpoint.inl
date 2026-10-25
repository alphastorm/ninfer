// Included inside EngineCore: every Program operation, including lease destruction, runs on
// its device-bound worker. Disk publication uses only CheckpointControl's independent gate.
    template <class Function>
    auto checkpoint_call(Function&& function) -> std::invoke_result_t<Function> {
        using Result = std::invoke_result_t<Function>;
        if (std::this_thread::get_id() == worker_.get_id()) {
            struct Relock {
                std::unique_lock<std::mutex>& lock;
                bool acquired;
                ~Relock() { if (acquired) { lock.unlock(); } }
            } scope{*checkpoint_execution_lock_, !checkpoint_execution_lock_->owns_lock()};
            if (scope.acquired) { scope.lock.lock(); }
            return std::forward<Function>(function)();
        }
        auto command = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Function>(function));
        auto result = command->get_future();
        {
            std::lock_guard lock(queue_mutex_);
            if (stopping_) { throw std::runtime_error("checkpoint worker is stopping"); }
            checkpoint_commands_.emplace_back([command] { (*command)(); });
        }
        queue_cv_.notify_one();
        return result.get();
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

    [[nodiscard]] DurableExportResult export_session(
        std::string_view digest, std::string_view tag, ContinuationCheckpointWriter& writer,
        const DurableLimits& limits) {
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
            DurableExportResult result;
            checkpoint_call([&] {
                if (failed_) { result.reason = DurableReason::IoUnavailable; return; }
                const auto gate = checkpoints_.session(digest);
                std::lock_guard lock(gate->mutex);
                result.captured_epoch = gate->epoch;
                if (gate->tag != tag) {
                    result.state = DurableExportState::Stale;
                    result.reason = DurableReason::TagMismatch;
                    return;
                }
                bool pending = false;
                const auto points = resources_.session_points(*instance_.program, digest, tag, pending);
                if (points.empty()) {
                    result.reason = pending ? DurableReason::TransactionBusy
                                            : DurableReason::SessionNotIndexed;
                    return;
                }
                lease.emplace(instance_.program->prepare_checkpoint_export(points, limits));
                result.reason = lease->reason();
            });
            if (!lease) { return result; }
            try {
                // Inline worker saves use the same off-execution-lock writer boundary as an
                // external control caller. The immutable lease alone owns copied content.
                const bool worker = std::this_thread::get_id() == worker_.get_id();
                std::unique_lock<std::mutex>* execution =
                    worker && checkpoint_execution_lock_->owns_lock() ? checkpoint_execution_lock_ : nullptr;
                if (execution) { execution->unlock(); }
                try {
                    const auto epoch = result.captured_epoch;
                    result = lease->write(writer);
                    result.captured_epoch = epoch;
                } catch (...) {
                    if (execution) { execution->lock(); }
                    throw;
                }
                if (execution) { execution->lock(); }
            } catch (...) {
                checkpoint_call([&] { lease.reset(); });
                throw;
            }
            checkpoint_call([&] { lease.reset(); });
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
                std::size_t count = 0;
            };
            auto pending = std::make_shared<Pending>();
            pending->target = target;
            PreparedEngineCheckpoint result;
            const auto oracle = checkpoints_.oracle();
            checkpoint_call([&] {
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
                    const auto reason = read_checkpoint_index(input, limits, set);
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
                    pending->native.emplace(instance_.program->prepare_checkpoint_import(set, input, limits));
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
                            scheduler_.capacity_released();
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
            });
            if (result.state != DurablePrepareState::Prepared) { return result; }
            try {
                result.abort = [this, pending] {
                    if (!pending->native) { return; }
                    checkpoint_call([&] { pending->native.reset(); });
                };
                result.commit = [this, pending]() -> DurableImportResult {
                    return checkpoint_call([&]() -> DurableImportResult {
                        if (!pending->native) {
                            return {.state = DurableImportState::PolicyDisabled,
                                    .reason = DurableReason::Cancelled};
                        }
                        const auto& target = pending->target;
                        const auto gate = checkpoints_.session(target.session_sha256);
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
                        if (!checkpoints_.commit_import(gate, target.expected_epoch, tag, order, [&] {
                            auto handles = instance_.program->commit_import(std::move(*pending->native));
                            pending->native.reset();
                            resources_.publish_import(*instance_.program, std::move(publication), handles, order);
                        })) {
                            pending->native.reset();
                            return {.state = DurableImportState::PolicyDisabled,
                                    .reason = DurableReason::StalePublication};
                        }
                        scheduler_.capacity_released();
                        request_admission_check();
                        return {DurableImportState::Imported, DurableReason::None, stats};
                    });
                };
            } catch (...) {
                checkpoint_call([&] { pending->native.reset(); });
                throw;
            }
            return result;
        }
    }

private:
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
        std::deque<std::function<void()>> commands;
        {
            std::lock_guard lock(queue_mutex_);
            commands.swap(checkpoint_commands_);
        }
        for (auto& command : commands) { command(); }
    }
    bool checkpoint_pressure(std::span<PressureCheckpointVictim> victims) {
        if (victims.empty()) { return true; }
        checkpoint_execution_lock_->unlock();
        const bool settled = checkpoints_.settle(victims);
        checkpoint_execution_lock_->lock();
        return settled;
    }
    bool checkpoint_request_settled(const Request& request) {
        // A paused request has not acknowledged a final response. Native owns its
        // retained prompt/progress and may revoke the snapshot in favour of replay.
        if (!request.is_model_finished()) { return true; }
        if (request.checkpoint_session.empty() || request.checkpoint_tag.empty()) { return true; }
        PressureCheckpointVictim victim{request.checkpoint_session, request.checkpoint_tag};
        return checkpoint_pressure({&victim, 1});
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
