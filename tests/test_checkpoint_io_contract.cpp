#include "runtime/contract/checkpoint_io.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ninfer::runtime;

int check(bool condition, const std::string& message) {
    if (condition) { return 0; }
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

CheckpointDigest digest(std::uint8_t value) {
    CheckpointDigest result{};
    result.fill(value);
    return result;
}

std::array<std::byte, 4> state_bytes() {
    return {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
}

std::array<std::byte, 8> kv_bytes() {
    return {std::byte{5}, std::byte{6},  std::byte{7},  std::byte{8},
            std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12}};
}

CheckpointManifestV1 manifest() {
    const auto state = state_bytes();
    const auto kv    = kv_bytes();
    CheckpointManifestV1 value;
    value.magic                       = kCheckpointMagic;
    value.schema_version              = kCheckpointSchemaVersion;
    value.journal_version             = kCheckpointJournalVersion;
    value.generation                  = 17;
    value.identity.model              = digest(1);
    value.identity.runtime_source     = digest(2);
    value.identity.deployment_profile = digest(3);
    value.identity.layout             = digest(4);
    value.identity.token_count        = 105000;
    value.identity.context_capacity   = 131072;
    value.payloads                    = {
        {CheckpointPayloadKind::StateImage, state.size(), sha256(state)},
        {CheckpointPayloadKind::MainKv, kv.size(), sha256(kv)},
    };
    return value;
}

CheckpointExpectation expectation() {
    CheckpointExpectation value;
    value.manifest        = manifest();
    value.manifest_sha256 = checkpoint_manifest_sha256(value.manifest);
    return value;
}

CheckpointImage image() {
    const auto state = state_bytes();
    const auto kv    = kv_bytes();
    CheckpointImage value;
    value.manifest = manifest();
    value.payloads = {
        {CheckpointPayloadKind::StateImage, std::vector(state.begin(), state.end())},
        {CheckpointPayloadKind::MainKv, std::vector(kv.begin(), kv.end())},
    };
    return value;
}

class FakeQuiescence final : public CheckpointQuiescence {
public:
    CheckpointPauseToken pause_admission() override {
        record("pause");
        if (fail == "pause") { throw std::runtime_error("pause failed"); }
        return CheckpointPauseToken(token_generation);
    }

    void drain_transactions(const CheckpointPauseToken& token) override {
        require_token(token);
        record("drain");
        if (mutate_on_drain) { mutate_on_drain(); }
        if (fail == "drain") { throw std::runtime_error("drain failed"); }
    }

    void fence_device(const CheckpointPauseToken& token) override {
        require_token(token);
        record("fence");
        if (fail == "fence") { throw std::runtime_error("fence failed"); }
    }

    void resume_admission(const CheckpointPauseToken& token) noexcept override {
        if (token.generation() != token_generation) { wrong_token = true; }
        record("resume");
    }

    void require_token(const CheckpointPauseToken& token) {
        if (token.generation() != token_generation) {
            throw std::runtime_error("wrong pause token");
        }
    }

    void record(const std::string& event) {
        std::lock_guard lock(events_mutex);
        events.push_back(event);
        if (shared_events != nullptr) { shared_events->push_back(event); }
    }

    std::vector<std::string> events;
    std::vector<std::string>* shared_events = nullptr;
    std::mutex events_mutex;
    std::function<void()> mutate_on_drain;
    std::string fail;
    std::uint64_t token_generation = 41;
    bool wrong_token               = false;
};

class FakeBackend final : public CheckpointBackend {
public:
    CheckpointBackendCapabilities capabilities() const noexcept override { return caps; }

    std::uint64_t committed_generation() const noexcept override { return committed; }

    void stage(const CheckpointManifestV1&, std::span<const CheckpointPayload> payloads,
               const CheckpointStageKey& key) override {
        record("stage");
        {
            std::unique_lock lock(block_mutex);
            stage_entered = true;
            block_cv.notify_all();
            block_cv.wait(lock, [&] { return !block_stage; });
        }
        if (fail_stage) { throw std::runtime_error("stage failed"); }
        for (const CheckpointPayload& payload : payloads) {
            staged_payload_digests.push_back(sha256(payload.bytes));
        }
        last_key = key;
    }

    void commit(const CheckpointStageKey& key) override {
        record("commit");
        if (fail_commit) { throw std::runtime_error("commit failed"); }
        committed = key.generation;
    }

    void abort(const CheckpointStageKey& key) noexcept override {
        record("abort");
        aborted_key = key;
    }

    CheckpointImage load(const CheckpointExpectation& expected) override {
        record("load");
        load_expectation = expected;
        if (fail_load) { throw std::runtime_error("load failed"); }
        return loaded_image;
    }

    void record(const std::string& event) {
        std::lock_guard lock(events_mutex);
        events.push_back(event);
        if (shared_events != nullptr) { shared_events->push_back(event); }
    }

    void wait_until_stage() {
        std::unique_lock lock(block_mutex);
        block_cv.wait(lock, [&] { return stage_entered; });
    }

    void release_stage() {
        std::lock_guard lock(block_mutex);
        block_stage = false;
        block_cv.notify_all();
    }

    CheckpointBackendCapabilities caps{true, true, false};
    CheckpointImage loaded_image = image();
    CheckpointExpectation load_expectation;
    CheckpointStageKey last_key;
    CheckpointStageKey aborted_key;
    std::vector<CheckpointDigest> staged_payload_digests;
    std::vector<std::string> events;
    std::vector<std::string>* shared_events = nullptr;
    std::mutex events_mutex;
    std::mutex block_mutex;
    std::condition_variable block_cv;
    std::uint64_t committed = 0;
    bool block_stage        = false;
    bool stage_entered      = false;
    bool fail_stage         = false;
    bool fail_commit        = false;
    bool fail_load          = false;
};

class FakeRestorer final : public CheckpointRestorer {
public:
    void apply(const CheckpointImage&) override {
        events.push_back("apply");
        if (shared_events != nullptr) { shared_events->push_back("apply"); }
        if (fail) { throw std::runtime_error("apply failed"); }
    }

    std::vector<std::string> events;
    std::vector<std::string>* shared_events = nullptr;
    bool fail                               = false;
};

bool throws(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::exception&) { return true; }
    return false;
}

int test_sha256_and_manifest_anchor() {
    int failures          = 0;
    const std::string abc = "abc";
    failures += check(sha256_hex(sha256(std::as_bytes(std::span(abc)))) ==
                          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                      "portable SHA-256 failed its known vector");
    CheckpointManifestV1 left  = manifest();
    CheckpointManifestV1 right = left;
    right.identity.token_count += 1;
    failures += check(checkpoint_manifest_sha256(left) != checkpoint_manifest_sha256(right),
                      "manifest digest ignored token_count");
    right                    = left;
    right.payloads[0].sha256 = digest(99);
    failures += check(checkpoint_manifest_sha256(left) != checkpoint_manifest_sha256(right),
                      "manifest digest ignored payload identity");
    const CheckpointManifestV1 empty;
    failures += check(empty.identity.model == CheckpointDigest{} && empty.payloads.empty() &&
                          empty.magic == 0,
                      "default manifest contains accepting or indeterminate identity");
    return failures;
}

int test_compatibility_matrix() {
    const CheckpointManifestV1 expected = manifest();
    int failures                        = 0;
    failures += check(validate_checkpoint_compatibility(expected, expected).compatible(),
                      "identical manifest was incompatible");
    const auto verify = [&](CheckpointManifestV1 observed, CheckpointCompatibilityError error,
                            const std::string& label) {
        return check(validate_checkpoint_compatibility(expected, observed).error == error, label);
    };
    CheckpointManifestV1 observed{};
    failures += verify(observed, CheckpointCompatibilityError::Magic,
                       "default manifest passed structural identity");
    observed                = expected;
    observed.schema_version = 2;
    failures +=
        verify(observed, CheckpointCompatibilityError::SchemaVersion, "stale schema accepted");
    observed = expected;
    observed.generation += 1;
    failures +=
        verify(observed, CheckpointCompatibilityError::Generation, "wrong generation accepted");
    observed                = expected;
    observed.identity.model = digest(8);
    failures += verify(observed, CheckpointCompatibilityError::Model, "wrong model accepted");
    observed                         = expected;
    observed.identity.runtime_source = digest(8);
    failures += verify(observed, CheckpointCompatibilityError::RuntimeSource,
                       "wrong runtime source accepted");
    observed                             = expected;
    observed.identity.deployment_profile = digest(8);
    failures += verify(observed, CheckpointCompatibilityError::DeploymentProfile,
                       "stale deployment profile accepted");
    observed                 = expected;
    observed.identity.layout = digest(8);
    failures += verify(observed, CheckpointCompatibilityError::Layout, "wrong layout accepted");
    observed                      = expected;
    observed.identity.token_count = 0xffffffffU;
    failures += verify(observed, CheckpointCompatibilityError::TokenCount,
                       "oversized token count accepted");
    observed = expected;
    observed.identity.context_capacity += 1;
    failures += verify(observed, CheckpointCompatibilityError::ContextCapacity,
                       "wrong context capacity accepted");
    observed = expected;
    observed.payloads.push_back(observed.payloads.front());
    failures += verify(observed, CheckpointCompatibilityError::PayloadSet,
                       "duplicate payload kind accepted");
    return failures;
}

int test_save_order_and_abort() {
    const CheckpointManifestV1 value = manifest();
    auto state                       = state_bytes();
    auto kv                          = kv_bytes();
    const std::array<CheckpointPayloadView, 2> payloads{{
        {CheckpointPayloadKind::StateImage, state},
        {CheckpointPayloadKind::MainKv, kv},
    }};
    FakeQuiescence quiescence;
    FakeBackend backend;
    std::mutex operation_mutex;
    std::vector<std::string> lifecycle;
    quiescence.shared_events = &lifecycle;
    backend.shared_events    = &lifecycle;
    CheckpointCoordinator coordinator(quiescence, backend, operation_mutex);

    const CheckpointOperationReceipt receipt = coordinator.save(value, payloads);
    int failures                             = 0;
    failures += check(receipt.operation == CheckpointOperationKind::Save && receipt.journal &&
                          receipt.journal->manifest_sha256 == checkpoint_manifest_sha256(value),
                      "successful save returned the wrong receipt");
    failures += check(lifecycle == std::vector<std::string>(
                                       {"pause", "drain", "fence", "stage", "commit", "resume"}),
                      "save lifecycle order changed");
    failures += check(backend.staged_payload_digests ==
                          std::vector<CheckpointDigest>({sha256(state), sha256(kv)}),
                      "backend did not receive the hashed immutable payload copies");

    lifecycle.clear();
    backend.fail_commit               = true;
    CheckpointManifestV1 failed_value = value;
    failed_value.generation += 1;
    const std::uint64_t prior_generation = backend.committed;
    failures += check(throws([&] { (void)coordinator.save(failed_value, payloads); }),
                      "commit failure was ignored");
    failures +=
        check(lifecycle == std::vector<std::string>(
                               {"pause", "drain", "fence", "stage", "commit", "abort", "resume"}) &&
                  backend.aborted_key.generation == failed_value.generation &&
                  backend.aborted_key.manifest_sha256 == checkpoint_manifest_sha256(failed_value) &&
                  backend.committed == prior_generation,
              "commit failure did not abort the coordinator-owned stage key");
    return failures;
}

int test_save_race_and_failure_resume() {
    const CheckpointManifestV1 value = manifest();
    auto state                       = state_bytes();
    auto kv                          = kv_bytes();
    const std::array<CheckpointPayloadView, 2> payloads{{
        {CheckpointPayloadKind::StateImage, state},
        {CheckpointPayloadKind::MainKv, kv},
    }};
    FakeQuiescence quiescence;
    FakeBackend backend;
    std::mutex operation_mutex;
    CheckpointCoordinator coordinator(quiescence, backend, operation_mutex);
    int failures = 0;

    quiescence.mutate_on_drain = [&] { state[0] = std::byte{99}; };
    failures += check(throws([&] { (void)coordinator.save(value, payloads); }),
                      "payload mutation during drain was accepted");
    failures += check(quiescence.events ==
                              std::vector<std::string>({"pause", "drain", "fence", "resume"}) &&
                          backend.events.empty() && !quiescence.wrong_token,
                      "payload race did not fail before staging and resume once");

    state = state_bytes();
    quiescence.events.clear();
    quiescence.mutate_on_drain = {};
    quiescence.fail            = "drain";
    failures += check(throws([&] { (void)coordinator.save(value, payloads); }),
                      "drain failure was ignored");
    failures += check(quiescence.events == std::vector<std::string>({"pause", "drain", "resume"}) &&
                          !quiescence.wrong_token,
                      "drain failure did not resume exactly once with its token");

    quiescence.events.clear();
    quiescence.fail = "fence";
    failures += check(throws([&] { (void)coordinator.save(value, payloads); }),
                      "fence failure was ignored");
    failures += check(quiescence.events ==
                              std::vector<std::string>({"pause", "drain", "fence", "resume"}) &&
                          !quiescence.wrong_token,
                      "fence failure did not resume exactly once with its token");

    quiescence.events.clear();
    quiescence.fail.clear();
    backend.fail_stage = true;
    failures += check(throws([&] { (void)coordinator.save(value, payloads); }),
                      "stage failure was ignored");
    failures += check(quiescence.events.back() == "resume" && !quiescence.wrong_token,
                      "stage failure did not resume admission");
    return failures;
}

int test_save_validation_precedes_pause() {
    CheckpointManifestV1 value = manifest();
    auto state                 = state_bytes();
    auto kv                    = kv_bytes();
    const std::array<CheckpointPayloadView, 2> payloads{{
        {CheckpointPayloadKind::StateImage, state},
        {CheckpointPayloadKind::MainKv, kv},
    }};
    FakeQuiescence quiescence;
    FakeBackend backend;
    std::mutex operation_mutex;
    CheckpointCoordinator coordinator(quiescence, backend, operation_mutex);
    int failures = 0;

    backend.caps.atomic_replace = false;
    failures += check(throws([&] { (void)coordinator.save(value, payloads); }),
                      "non-atomic backend was accepted");
    failures += check(quiescence.events.empty(), "invalid backend paused admission");

    backend.caps.atomic_replace = true;
    backend.committed           = value.generation;
    failures += check(throws([&] { (void)coordinator.save(value, payloads); }),
                      "reused checkpoint generation was accepted");
    failures += check(quiescence.events.empty(), "stale generation paused admission");
    return failures;
}

int test_restore_integrity_and_order() {
    const CheckpointExpectation expected = expectation();
    FakeQuiescence quiescence;
    FakeBackend backend;
    FakeRestorer restorer;
    std::mutex operation_mutex;
    std::vector<std::string> lifecycle;
    quiescence.shared_events = &lifecycle;
    backend.shared_events    = &lifecycle;
    restorer.shared_events   = &lifecycle;
    CheckpointCoordinator coordinator(quiescence, backend, operation_mutex);

    const CheckpointOperationReceipt receipt = coordinator.restore(expected, restorer);
    int failures                             = 0;
    failures += check(receipt.operation == CheckpointOperationKind::Restore && !receipt.journal &&
                          lifecycle == std::vector<std::string>({"load", "pause", "drain", "fence",
                                                                 "apply", "resume"}) &&
                          backend.load_expectation.manifest_sha256 == expected.manifest_sha256,
                      "restore lifecycle, receipt, or bounded load expectation changed");

    lifecycle.clear();
    quiescence.events.clear();
    backend.events.clear();
    restorer.events.clear();
    CheckpointExpectation bad_expectation = expected;
    bad_expectation.manifest_sha256       = digest(99);
    failures += check(throws([&] { (void)coordinator.restore(bad_expectation, restorer); }),
                      "untrusted expectation digest was accepted");
    failures += check(lifecycle.empty() && restorer.events.empty(),
                      "bad expectation touched the backend or runtime");

    backend.loaded_image                      = image();
    backend.loaded_image.payloads[0].bytes[0] = std::byte{99};
    failures += check(throws([&] { (void)coordinator.restore(expected, restorer); }),
                      "payload byte substitution was accepted");
    failures += check(quiescence.events.empty() && restorer.events.empty(),
                      "payload mismatch paused or applied runtime state");

    backend.loaded_image                                      = image();
    backend.loaded_image.manifest.identity.deployment_profile = digest(9);
    failures += check(throws([&] { (void)coordinator.restore(expected, restorer); }),
                      "stale profile restore was accepted");
    failures += check(restorer.events.empty(), "stale profile applied runtime state");

    backend.loaded_image = image();
    restorer.fail        = true;
    failures += check(throws([&] { (void)coordinator.restore(expected, restorer); }),
                      "restore apply failure was ignored");
    failures += check(quiescence.events.back() == "resume" && !quiescence.wrong_token,
                      "restore apply failure did not resume with its token");
    return failures;
}

int test_shared_single_flight_lock() {
    const CheckpointManifestV1 value = manifest();
    auto state                       = state_bytes();
    auto kv                          = kv_bytes();
    const std::array<CheckpointPayloadView, 2> payloads{{
        {CheckpointPayloadKind::StateImage, state},
        {CheckpointPayloadKind::MainKv, kv},
    }};
    FakeQuiescence quiescence;
    FakeBackend backend;
    backend.block_stage = true;
    std::mutex operation_mutex;
    CheckpointCoordinator first_coordinator(quiescence, backend, operation_mutex);
    CheckpointCoordinator second_coordinator(quiescence, backend, operation_mutex);
    std::atomic<bool> failed{false};

    std::thread first([&] {
        try {
            (void)first_coordinator.save(value, payloads);
        } catch (...) { failed = true; }
    });
    backend.wait_until_stage();
    int failures = check(!operation_mutex.try_lock(),
                         "coordinator released the shared lock before backend commit");
    backend.release_stage();
    first.join();
    failures += check(!failed, "first serialized checkpoint operation failed");
    failures += check(operation_mutex.try_lock(), "shared lock remained held after resume");
    operation_mutex.unlock();

    backend.committed = 0;
    failures += check(!throws([&] { (void)second_coordinator.save(value, payloads); }),
                      "second coordinator could not use the shared lock after completion");
    return failures;
}

} // namespace

int main() {
    int failures = 0;
    failures += test_sha256_and_manifest_anchor();
    failures += test_compatibility_matrix();
    failures += test_save_order_and_abort();
    failures += test_save_race_and_failure_resume();
    failures += test_save_validation_precedes_pause();
    failures += test_restore_integrity_and_order();
    failures += test_shared_single_flight_lock();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
