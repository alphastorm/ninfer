#include "runtime/contract/checkpoint_io.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ninfer::runtime;

int check(bool condition, const std::string& message) {
    if (condition) { return 0; }
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

CheckpointDigest digest(unsigned char value) {
    CheckpointDigest result{};
    result.fill(static_cast<std::byte>(value));
    return result;
}

CheckpointManifestV1 manifest() {
    CheckpointManifestV1 value;
    value.generation                  = 17;
    value.identity.model              = digest(1);
    value.identity.runtime_source     = digest(2);
    value.identity.deployment_profile = digest(3);
    value.identity.layout             = digest(4);
    value.identity.token_count        = 105000;
    value.identity.context_capacity   = 131072;
    value.payloads                    = {
        {CheckpointPayloadKind::StateImage, 4, digest(5)},
        {CheckpointPayloadKind::MainKv, 8, digest(6)},
    };
    return value;
}

class FakeQuiescence final : public CheckpointQuiescence {
public:
    CheckpointPauseToken pause_admission() override {
        record("pause");
        if (fail == "pause") { throw std::runtime_error("pause failed"); }
        return {41};
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
        if (token.generation != 41) { wrong_token = true; }
        record("resume");
    }

    void require_token(CheckpointPauseToken token) {
        if (token.generation != 41) { throw std::runtime_error("wrong pause token"); }
    }

    void record(const std::string& event) {
        events.push_back(event);
        if (shared_events != nullptr) { shared_events->push_back(event); }
    }

    std::vector<std::string> events;
    std::vector<std::string>* shared_events = nullptr;
    std::string fail;
    bool wrong_token = false;
};

class FakeBackend final : public CheckpointBackend {
public:
    CheckpointBackendCapabilities capabilities() const noexcept override { return caps; }

    CheckpointJournalReceipt store_atomic(const CheckpointManifestV1& value,
                                          std::span<const CheckpointPayloadView>) override {
        record("store");
        if (fail_store) { throw std::runtime_error("store failed"); }
        return {kCheckpointJournalVersion, value.generation, digest(9), receipt_committed};
    }

    CheckpointImage load() override {
        record("load");
        if (fail_load) { throw std::runtime_error("load failed"); }
        return image;
    }

    void record(const std::string& event) {
        events.push_back(event);
        if (shared_events != nullptr) { shared_events->push_back(event); }
    }

    CheckpointBackendCapabilities caps{true, true, false};
    CheckpointImage image;
    std::vector<std::string> events;
    std::vector<std::string>* shared_events = nullptr;
    bool fail_store                         = false;
    bool fail_load                          = false;
    bool receipt_committed                  = true;
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
    CheckpointManifestV1 observed = expected;
    observed.magic                = 0;
    failures += verify(observed, CheckpointCompatibilityError::Magic, "bad magic accepted");
    observed                = expected;
    observed.schema_version = 2;
    failures +=
        verify(observed, CheckpointCompatibilityError::SchemaVersion, "stale schema accepted");
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
    observed                           = expected;
    observed.identity.context_capacity = expected.identity.context_capacity + 1;
    failures += verify(observed, CheckpointCompatibilityError::ContextCapacity,
                       "oversized context accepted");
    observed                    = expected;
    observed.payloads[0].sha256 = digest(8);
    failures += verify(observed, CheckpointCompatibilityError::PayloadSet,
                       "wrong payload identity accepted");
    return failures;
}

int test_save_order_and_failure_resume() {
    CheckpointManifestV1 value = manifest();
    const std::array<std::byte, 4> state{};
    const std::array<std::byte, 8> kv{};
    const std::array<CheckpointPayloadView, 2> payloads{{
        {CheckpointPayloadKind::StateImage, state},
        {CheckpointPayloadKind::MainKv, kv},
    }};

    FakeQuiescence quiescence;
    FakeBackend backend;
    std::vector<std::string> lifecycle;
    quiescence.shared_events = &lifecycle;
    backend.shared_events    = &lifecycle;
    CheckpointCoordinator coordinator(quiescence, backend);
    const CheckpointOperationReceipt receipt = coordinator.save(value, payloads);
    int failures                             = 0;
    failures += check(receipt.terminal_phase == CheckpointPhase::Resumed &&
                          receipt.journal.committed && receipt.generation == value.generation,
                      "successful save returned the wrong receipt");
    failures +=
        check(lifecycle == std::vector<std::string>({"pause", "drain", "fence", "store", "resume"}),
              "save lifecycle order changed");

    quiescence.events.clear();
    backend.events.clear();
    lifecycle.clear();
    backend.fail_store = true;
    failures += check(throws([&] { (void)coordinator.save(value, payloads); }),
                      "backend save failure was ignored");
    failures += check(quiescence.events.back() == "resume",
                      "backend save failure did not resume admission");

    quiescence.events.clear();
    lifecycle.clear();
    backend.fail_store = false;
    quiescence.fail    = "drain";
    failures += check(throws([&] { (void)coordinator.save(value, payloads); }),
                      "drain failure was ignored");
    failures += check(quiescence.events == std::vector<std::string>({"pause", "drain", "resume"}),
                      "drain failure did not resume without fencing");
    return failures;
}

int test_backend_and_payload_fail_closed() {
    CheckpointManifestV1 value = manifest();
    const std::array<std::byte, 4> state{};
    const std::array<CheckpointPayloadView, 1> short_payloads{{
        {CheckpointPayloadKind::StateImage, state},
    }};
    FakeQuiescence quiescence;
    FakeBackend backend;
    CheckpointCoordinator coordinator(quiescence, backend);
    int failures                = 0;
    backend.caps.atomic_replace = false;
    failures += check(throws([&] { (void)coordinator.save(value, short_payloads); }),
                      "non-atomic backend was accepted");
    failures += check(quiescence.events.empty(), "invalid backend paused admission");

    backend.caps.atomic_replace = true;
    failures += check(throws([&] { (void)coordinator.save(value, short_payloads); }),
                      "incomplete payload set was accepted");
    failures += check(quiescence.events.empty(), "invalid payload set paused admission");
    return failures;
}

int test_restore_order_and_stale_rejection() {
    const CheckpointManifestV1 expected = manifest();
    FakeQuiescence quiescence;
    FakeBackend backend;
    backend.image.manifest = expected;
    backend.image.payloads = {
        {CheckpointPayloadKind::StateImage, std::vector<std::byte>(4), digest(5)},
        {CheckpointPayloadKind::MainKv, std::vector<std::byte>(8), digest(6)},
    };
    FakeRestorer restorer;
    std::vector<std::string> lifecycle;
    quiescence.shared_events = &lifecycle;
    backend.shared_events    = &lifecycle;
    restorer.shared_events   = &lifecycle;
    CheckpointCoordinator coordinator(quiescence, backend);

    const CheckpointOperationReceipt receipt = coordinator.restore(expected, restorer);
    int failures                             = 0;
    failures += check(receipt.terminal_phase == CheckpointPhase::Resumed &&
                          lifecycle == std::vector<std::string>(
                                           {"load", "pause", "drain", "fence", "apply", "resume"}),
                      "restore lifecycle order changed");

    quiescence.events.clear();
    restorer.events.clear();
    backend.events.clear();
    lifecycle.clear();
    backend.image.manifest.identity.deployment_profile = digest(9);
    failures += check(throws([&] { (void)coordinator.restore(expected, restorer); }),
                      "stale profile restore was accepted");
    failures += check(quiescence.events.empty() && restorer.events.empty(),
                      "stale profile mutated runtime state");

    backend.image.manifest           = expected;
    backend.image.payloads[0].sha256 = digest(9);
    failures += check(throws([&] { (void)coordinator.restore(expected, restorer); }),
                      "payload digest mismatch was accepted");
    failures += check(quiescence.events.empty() && restorer.events.empty(),
                      "payload mismatch mutated runtime state");

    backend.image.payloads[0].sha256 = digest(5);
    restorer.fail                    = true;
    failures += check(throws([&] { (void)coordinator.restore(expected, restorer); }),
                      "restore apply failure was ignored");
    failures += check(quiescence.events.back() == "resume",
                      "restore apply failure did not resume admission");
    return failures;
}

} // namespace

int main() {
    int failures = 0;
    failures += test_compatibility_matrix();
    failures += test_save_order_and_failure_resume();
    failures += test_backend_and_payload_fail_closed();
    failures += test_restore_order_and_stale_rejection();
    if (failures == 0) { std::cout << "ok\n"; }
    return failures == 0 ? 0 : 1;
}
