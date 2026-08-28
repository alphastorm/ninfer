#include "runtime/contract/checkpoint_io.h"

#include <array>
#include <atomic>
#include <chrono>
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

const std::array<std::byte, 4> kState{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
const std::array<std::byte, 8> kKv{std::byte{5}, std::byte{6},  std::byte{7},  std::byte{8},
                                   std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12}};

CheckpointManifestV1 manifest() {
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
        {CheckpointPayloadKind::StateImage, kState.size(), sha256(kState)},
        {CheckpointPayloadKind::MainKv, kKv.size(), sha256(kKv)},
    };
    return value;
}

CheckpointExpectation expectation() {
    CheckpointExpectation value;
    value.manifest        = manifest();
    value.manifest_sha256 = checkpoint_manifest_sha256(value.manifest);
    return value;
}

std::array<CheckpointPayloadView, 2> payload_views() {
    return {{{CheckpointPayloadKind::StateImage, kState}, {CheckpointPayloadKind::MainKv, kKv}}};
}

CheckpointImage image() {
    CheckpointImage value;
    value.manifest = manifest();
    value.payloads = {
        {CheckpointPayloadKind::StateImage, std::vector(kState.begin(), kState.end())},
        {CheckpointPayloadKind::MainKv, std::vector(kKv.begin(), kKv.end())},
    };
    return value;
}

class FakeQuiescence final : public CheckpointQuiescence {
public:
    CheckpointPauseToken pause_admission() override {
        record("pause");
        if (fail == "pause") { throw std::runtime_error("pause failed"); }
        return {token_generation};
    }

    void drain_transactions(CheckpointPauseToken token) override {
        require_token(token);
        record("drain");
        if (fail == "drain") { throw std::runtime_error("drain failed"); }
    }

    void fence_device(CheckpointPauseToken token) override {
        require_token(token);
        record("fence");
        if (fail == "fence") { throw std::runtime_error("fence failed"); }
    }

    void resume_admission(CheckpointPauseToken token) noexcept override {
        if (token.generation != token_generation) { wrong_token = true; }
        record("resume");
    }

    void require_token(CheckpointPauseToken token) {
        if (token.generation != token_generation) { throw std::runtime_error("wrong pause token"); }
    }

    void record(const std::string& event) {
        std::lock_guard lock(events_mutex);
        events.push_back(event);
        if (shared_events != nullptr) { shared_events->push_back(event); }
    }

    std::vector<std::string> events;
    std::vector<std::string>* shared_events = nullptr;
    std::mutex events_mutex;
    std::string fail;
    std::uint64_t token_generation = 41;
    bool wrong_token               = false;
};

class FakeBackend final : public CheckpointBackend {
public:
    CheckpointBackendCapabilities capabilities() const noexcept override { return caps; }

    CheckpointStageReceipt stage(const CheckpointManifestV1& value,
                                 std::span<const CheckpointPayloadView>) override {
        record("stage");
        {
            std::unique_lock lock(block_mutex);
            stage_entered = true;
            block_cv.notify_all();
            block_cv.wait(lock, [&] { return !block_stage; });
        }
        if (fail_stage) { throw std::runtime_error("stage failed"); }
        CheckpointStageReceipt receipt{kCheckpointJournalVersion, value.generation,
                                       checkpoint_manifest_sha256(value)};
        if (invalid_stage) { receipt.manifest_sha256 = digest(99); }
        return receipt;
    }

    void commit(const CheckpointStageReceipt&) override {
        record("commit");
        if (fail_commit) { throw std::runtime_error("commit failed"); }
    }

    void abort(const CheckpointStageReceipt&) noexcept override { record("abort"); }

    CheckpointImage load() override {
        record("load");
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
    std::vector<std::string> events;
    std::vector<std::string>* shared_events = nullptr;
    std::mutex events_mutex;
    std::mutex block_mutex;
    std::condition_variable block_cv;
    bool block_stage   = false;
    bool stage_entered = false;
    bool fail_stage    = false;
    bool fail_commit   = false;
    bool fail_load     = false;
    bool invalid_stage = false;
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
    return failures;
}

int test_compatibility_matrix() {
    const CheckpointManifestV1 expected = manifest();
    int failures                        = 0;
    failures += check(validate_checkpoint_compatibility(expected, expected).compatible(),
                      "identical manifest was incompatible");

    const auto verify = [&](CheckpointManifestV1 observed, CheckpointCompatibilityError error,
                            const std::string& label) {
        const CheckpointCompatibility result =
            validate_checkpoint_compatibility(expected, observed);
        return check(result.error == error, label);
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
    const auto payloads              = payload_views();
    FakeQuiescence quiescence;
    FakeBackend backend;
    std::vector<std::string> lifecycle;
    quiescence.shared_events = &lifecycle;
    backend.shared_events    = &lifecycle;
    CheckpointCoordinator coordinator(quiescence, backend);

    const CheckpointOperationReceipt receipt = coordinator.save(value, payloads);
    int failures                             = 0;
    failures += check(receipt.operation == CheckpointOperationKind::Save &&
                          receipt.terminal_phase == CheckpointPhase::Resumed && receipt.journal &&
                          receipt.journal->manifest_sha256 == checkpoint_manifest_sha256(value),
                      "successful save returned the wrong receipt");
    failures += check(lifecycle == std::vector<std::string>(
                                       {"pause", "drain", "fence", "stage", "commit", "resume"}),
                      "save lifecycle order changed");

    lifecycle.clear();
    backend.invalid_stage = true;
    failures += check(throws([&] { (void)coordinator.save(value, payloads); }),
                      "invalid stage receipt was accepted");
    failures += check(lifecycle == std::vector<std::string>(
                                       {"pause", "drain", "fence", "stage", "abort", "resume"}),
                      "invalid stage receipt did not abort and resume");

    lifecycle.clear();
    backend.invalid_stage = false;
    backend.fail_commit   = true;
    failures += check(throws([&] { (void)coordinator.save(value, payloads); }),
                      "commit failure was ignored");
    failures += check(lifecycle == std::vector<std::string>({"pause", "drain", "fence", "stage",
                                                             "commit", "abort", "resume"}),
                      "commit failure did not abort and resume");
    return failures;
}

int test_save_validation_precedes_pause() {
    CheckpointManifestV1 value = manifest();
    const auto payloads        = payload_views();
    FakeQuiescence quiescence;
    FakeBackend backend;
    CheckpointCoordinator coordinator(quiescence, backend);
    int failures = 0;

    backend.caps.atomic_replace = false;
    failures += check(throws([&] { (void)coordinator.save(value, payloads); }),
                      "non-atomic backend was accepted");
    failures += check(quiescence.events.empty(), "invalid backend paused admission");

    backend.caps.atomic_replace = true;
    value.payloads[0].sha256    = digest(99);
    failures += check(throws([&] { (void)coordinator.save(value, payloads); }),
                      "incorrect save payload digest was accepted");
    failures += check(quiescence.events.empty(), "invalid payload paused admission");

    value                       = manifest();
    quiescence.token_generation = 0;
    failures += check(throws([&] { (void)coordinator.save(value, payloads); }),
                      "invalid pause token was accepted");
    failures += check(quiescence.events == std::vector<std::string>({"pause"}),
                      "invalid pause token advanced the lifecycle");
    return failures;
}

int test_restore_integrity_and_order() {
    const CheckpointExpectation expected = expectation();
    FakeQuiescence quiescence;
    FakeBackend backend;
    FakeRestorer restorer;
    std::vector<std::string> lifecycle;
    quiescence.shared_events = &lifecycle;
    backend.shared_events    = &lifecycle;
    restorer.shared_events   = &lifecycle;
    CheckpointCoordinator coordinator(quiescence, backend);

    const CheckpointOperationReceipt receipt = coordinator.restore(expected, restorer);
    int failures                             = 0;
    failures += check(receipt.operation == CheckpointOperationKind::Restore && !receipt.journal &&
                          receipt.terminal_phase == CheckpointPhase::Resumed &&
                          lifecycle == std::vector<std::string>(
                                           {"load", "pause", "drain", "fence", "apply", "resume"}),
                      "restore lifecycle or receipt changed");

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
    failures += check(quiescence.events.back() == "resume",
                      "restore apply failure did not resume admission");
    return failures;
}

int test_operations_are_single_flight() {
    const CheckpointManifestV1 value = manifest();
    const auto payloads              = payload_views();
    FakeQuiescence quiescence;
    FakeBackend backend;
    backend.block_stage = true;
    CheckpointCoordinator coordinator(quiescence, backend);
    std::atomic<bool> failed{false};

    std::thread first([&] {
        try {
            (void)coordinator.save(value, payloads);
        } catch (...) { failed = true; }
    });
    backend.wait_until_stage();
    std::thread second([&] {
        try {
            (void)coordinator.save(value, payloads);
        } catch (...) { failed = true; }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    int failures = 0;
    {
        std::lock_guard lock(quiescence.events_mutex);
        failures +=
            check(std::count(quiescence.events.begin(), quiescence.events.end(), "pause") == 1,
                  "overlapping checkpoint operations both paused admission");
    }
    backend.release_stage();
    first.join();
    second.join();
    failures += check(!failed, "serialized checkpoint operation failed");
    failures += check(std::count(quiescence.events.begin(), quiescence.events.end(), "pause") == 2,
                      "second checkpoint operation never ran after serialization");
    return failures;
}

} // namespace

int main() {
    int failures = 0;
    failures += test_sha256_and_manifest_anchor();
    failures += test_compatibility_matrix();
    failures += test_save_order_and_abort();
    failures += test_save_validation_precedes_pause();
    failures += test_restore_integrity_and_order();
    failures += test_operations_are_single_flight();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
