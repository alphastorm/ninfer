#include "runtime/windows/direct_storage_checkpoint_backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace ninfer::runtime;
using namespace ninfer::runtime::windows;

int check(bool condition, const std::string& message) {
    if (condition) { return 0; }
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

bool throws(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::exception&) { return true; }
    return false;
}

std::string thrown_message(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::exception& error) { return error.what(); }
    return {};
}

CheckpointDigest digest(std::uint8_t value) {
    CheckpointDigest result{};
    result.fill(value);
    return result;
}

struct Fixture {
    CheckpointManifestV1 manifest;
    std::vector<CheckpointPayload> payloads;
    CheckpointExpectation expectation;
    CheckpointStageKey key;
};

Fixture fixture(std::uint64_t generation = 17) {
    Fixture value;
    value.payloads = {
        {CheckpointPayloadKind::StateImage,
         {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}},
        {CheckpointPayloadKind::MainKv,
         {std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10},
          std::byte{11}, std::byte{12}}},
    };
    value.manifest.magic                       = kCheckpointMagic;
    value.manifest.schema_version              = kCheckpointSchemaVersion;
    value.manifest.journal_version             = kCheckpointJournalVersion;
    value.manifest.generation                  = generation;
    value.manifest.identity.model              = digest(1);
    value.manifest.identity.runtime_source     = digest(2);
    value.manifest.identity.deployment_profile = digest(3);
    value.manifest.identity.layout             = digest(4);
    value.manifest.identity.token_count        = 105000;
    value.manifest.identity.context_capacity   = 131072;
    for (const CheckpointPayload& payload : value.payloads) {
        value.manifest.payloads.push_back(
            {payload.kind, payload.bytes.size(), sha256(payload.bytes)});
    }
    value.expectation.manifest        = value.manifest;
    value.expectation.manifest_sha256 = checkpoint_manifest_sha256(value.manifest);
    value.key = {kCheckpointJournalVersion, generation, value.expectation.manifest_sha256};
    return value;
}

std::string path_key(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

class EmptyDirectoryLock final : public CheckpointDirectoryLock {};

class FakeFileSystem final : public CheckpointFileSystem {
public:
    void ensure_directory(const std::filesystem::path&) override {}

    std::unique_ptr<CheckpointDirectoryLock> lock_directory(const std::filesystem::path&,
                                                            std::uint32_t timeout_ms) override {
        ++lock_count;
        observed_lock_timeout_ms = timeout_ms;
        return std::make_unique<EmptyDirectoryLock>();
    }

    std::vector<std::filesystem::path> list_regular_files(const std::filesystem::path& path,
                                                          std::size_t max_entries) override {
        std::lock_guard lock(mutex);
        std::vector<std::filesystem::path> result;
        const std::filesystem::path parent = path.lexically_normal();
        for (const auto& [name, bytes] : files) {
            (void)bytes;
            const std::filesystem::path candidate(name);
            if (candidate.parent_path() != parent) { continue; }
            if (result.size() == max_entries) {
                throw std::runtime_error("fake cleanup bound exceeded");
            }
            result.push_back(candidate);
        }
        return result;
    }

    bool file_exists(const std::filesystem::path& path) override {
        std::lock_guard lock(mutex);
        return files.contains(path_key(path));
    }

    std::uint64_t file_size(const std::filesystem::path& path) override {
        std::lock_guard lock(mutex);
        const auto found = files.find(path_key(path));
        if (found == files.end()) { throw std::runtime_error("missing fake file"); }
        return found->second.size();
    }

    void read_exact(const std::filesystem::path& path, std::span<std::byte> bytes) override {
        std::lock_guard lock(mutex);
        const auto found = files.find(path_key(path));
        if (found == files.end() || found->second.size() != bytes.size()) {
            throw std::runtime_error("invalid fake read");
        }
        std::copy(found->second.begin(), found->second.end(), bytes.begin());
        events.push_back("read_manifest");
    }

    void write_bytes_durable(const std::filesystem::path& path,
                             std::span<const std::byte> bytes) override {
        std::lock_guard lock(mutex);
        if (fail_manifest_write) { throw std::runtime_error("injected manifest write failure"); }
        files[path_key(path)] = std::vector<std::byte>(bytes.begin(), bytes.end());
        events.push_back("write_manifest_durable");
    }

    void write_payloads_durable(const std::filesystem::path& path,
                                std::span<const CheckpointPayload> payloads,
                                std::span<const std::uint64_t> offsets,
                                std::uint64_t total_bytes) override {
        if (fail_payload_write) { throw std::runtime_error("injected payload write failure"); }
        if (total_bytes > 1ULL << 20) {
            throw std::runtime_error("fake payload unexpectedly large");
        }
        std::vector<std::byte> data(static_cast<std::size_t>(total_bytes));
        for (std::size_t index = 0; index < payloads.size(); ++index) {
            std::copy(payloads[index].bytes.begin(), payloads[index].bytes.end(),
                      data.begin() + static_cast<std::ptrdiff_t>(offsets[index]));
        }
        std::lock_guard lock(mutex);
        files[path_key(path)] = std::move(data);
        events.push_back("write_payload_durable");
    }

    void atomic_replace_durable(const std::filesystem::path& source,
                                const std::filesystem::path& destination) override {
        std::lock_guard lock(mutex);
        const std::string source_key      = path_key(source);
        const std::string destination_key = path_key(destination);
        const bool manifest_publish       = destination.filename() == "checkpoint.manifest.v1";
        if (manifest_publish && fail_manifest_publish) {
            throw std::runtime_error("injected manifest publication failure");
        }
        const auto found = files.find(source_key);
        if (found == files.end()) { throw std::runtime_error("missing fake replacement source"); }
        files[destination_key] = std::move(found->second);
        files.erase(found);
        events.push_back(manifest_publish ? "publish_manifest_durable" : "publish_payload_durable");
    }

    bool remove_file(const std::filesystem::path& path) noexcept override {
        std::lock_guard lock(mutex);
        if (fail_remove) { return false; }
        files.erase(path_key(path));
        events.push_back("remove");
        return true;
    }

    void copy_range(const std::filesystem::path& path, std::uint64_t offset,
                    std::span<std::byte> destination) {
        std::lock_guard lock(mutex);
        const auto found = files.find(path_key(path));
        if (found == files.end() || offset > found->second.size() ||
            destination.size() > found->second.size() - offset) {
            throw std::runtime_error("fake DirectStorage read exceeds file");
        }
        std::copy_n(found->second.begin() + static_cast<std::ptrdiff_t>(offset), destination.size(),
                    destination.begin());
    }

    std::vector<std::byte> file_with_suffix(std::string_view suffix) const {
        std::lock_guard lock(mutex);
        for (const auto& [path, bytes] : files) {
            if (path.ends_with(suffix)) { return bytes; }
        }
        return {};
    }

    std::size_t count_suffix(std::string_view suffix) const {
        std::lock_guard lock(mutex);
        return static_cast<std::size_t>(
            std::count_if(files.begin(), files.end(),
                          [&](const auto& entry) { return entry.first.ends_with(suffix); }));
    }

    void replace_file_with_suffix(std::string_view suffix, std::vector<std::byte> bytes) {
        std::lock_guard lock(mutex);
        for (auto& [path, current] : files) {
            if (path.ends_with(suffix)) {
                current = std::move(bytes);
                return;
            }
        }
        throw std::runtime_error("fake file suffix not found");
    }

    mutable std::mutex mutex;
    std::map<std::string, std::vector<std::byte>> files;
    std::vector<std::string> events;
    std::atomic<int> lock_count{0};
    std::atomic<std::uint32_t> observed_lock_timeout_ms{0};
    bool fail_payload_write    = false;
    bool fail_manifest_write   = false;
    bool fail_manifest_publish = false;
    bool fail_remove           = false;
};

class FakeReadQueue final : public CheckpointReadQueue {
public:
    explicit FakeReadQueue(std::shared_ptr<FakeFileSystem> file_system)
        : file_system(std::move(file_system)) {}

    bool available() const noexcept override { return is_available; }

    std::string_view unavailable_reason() const noexcept override { return reason; }

    class Completion final : public CheckpointReadCompletion {
    public:
        Completion(FakeReadQueue& owner, std::filesystem::path path,
                   std::span<const CheckpointReadRequest> requests)
            : owner(owner), path(std::move(path)), requests(requests.begin(), requests.end()) {}

        void wait() override {
            if (finished) { return; }
            finished = true;
            ++owner.wait_count;
            if (owner.fail_wait) {
                throw std::runtime_error("injected DirectStorage completion failure");
            }
            for (const CheckpointReadRequest& request : requests) {
                owner.file_system->copy_range(path, request.file_offset, request.destination);
            }
            if (owner.corrupt_after_read && !requests.empty() &&
                !requests.front().destination.empty()) {
                requests.front().destination.front() ^= std::byte{0xff};
            }
        }

    private:
        FakeReadQueue& owner;
        std::filesystem::path path;
        std::vector<CheckpointReadRequest> requests;
        bool finished = false;
    };

    std::unique_ptr<CheckpointReadCompletion>
    submit(const std::filesystem::path& path,
           std::span<const CheckpointReadRequest> requests) override {
        ++submit_count;
        if (fail_submit) { throw std::runtime_error("injected DirectStorage submission failure"); }
        return std::make_unique<Completion>(*this, path, requests);
    }

    std::shared_ptr<FakeFileSystem> file_system;
    std::atomic<int> submit_count{0};
    std::atomic<int> wait_count{0};
    bool is_available       = true;
    bool fail_submit        = false;
    bool fail_wait          = false;
    bool corrupt_after_read = false;
    std::string reason;
};

DirectStorageCheckpointConfig config(std::string_view name, std::uint64_t max_bytes = 1ULL << 20) {
    return {std::filesystem::path("/virtual") / std::string(name), max_bytes};
}

void stage_and_commit(DirectStorageCheckpointBackend& backend, const Fixture& value) {
    backend.stage(value.manifest, value.payloads, value.key);
    backend.commit(value.key);
}

bool image_matches(const CheckpointImage& image, const Fixture& value) {
    if (image.payloads.size() != value.payloads.size()) { return false; }
    for (std::size_t index = 0; index < image.payloads.size(); ++index) {
        if (image.payloads[index].kind != value.payloads[index].kind ||
            image.payloads[index].bytes != value.payloads[index].bytes) {
            return false;
        }
    }
    return image.manifest.generation == value.manifest.generation;
}

int test_transaction_and_completion() {
    auto file_system = std::make_shared<FakeFileSystem>();
    auto queue       = std::make_shared<FakeReadQueue>(file_system);
    DirectStorageCheckpointBackend backend(config("transaction"), file_system, queue);
    const Fixture value = fixture();
    int failures        = 0;

    const CheckpointBackendCapabilities capabilities = backend.capabilities();
    failures += check(capabilities.atomic_replace && capabilities.durable_flush &&
                          !capabilities.direct_to_device,
                      "backend capabilities misreported the frozen host-load contract");
    backend.stage(value.manifest, value.payloads, value.key);
    failures += check(file_system->count_suffix("checkpoint.manifest.v1") == 0,
                      "stage published the authoritative manifest before commit");
    failures +=
        check(file_system->events.size() >= 2 &&
                  file_system->events[file_system->events.size() - 2] == "write_payload_durable" &&
                  file_system->events.back() == "write_manifest_durable",
              "stage did not durably write payload then canonical manifest");
    backend.commit(value.key);

    const std::vector<std::byte> manifest = file_system->file_with_suffix("checkpoint.manifest.v1");
    failures += check(!manifest.empty() && sha256(manifest) == value.key.manifest_sha256,
                      "published manifest is not the independently hashable canonical encoding");
    const std::vector<std::byte> payload = file_system->file_with_suffix(".payload");
    failures +=
        check(payload.size() == 4104 &&
                  std::all_of(payload.begin() + 4, payload.begin() + 4096,
                              [](std::byte byte) { return byte == std::byte{0}; }) &&
                  std::equal(value.payloads[1].bytes.begin(), value.payloads[1].bytes.end(),
                             payload.begin() + 4096),
              "payload file did not preserve DirectStorage alignment without hidden schema");
    failures += check(backend.committed_generation() == value.manifest.generation,
                      "commit did not advance the durable generation");

    const CheckpointImage loaded = backend.load(value.expectation);
    failures +=
        check(image_matches(loaded, value), "DirectStorage completion returned wrong bytes");
    failures += check(queue->submit_count == 1 && queue->wait_count == 1,
                      "load returned without awaiting its DirectStorage completion");
    return failures;
}

int test_commit_failure_and_trusted_abort() {
    auto file_system = std::make_shared<FakeFileSystem>();
    auto queue       = std::make_shared<FakeReadQueue>(file_system);
    DirectStorageCheckpointBackend backend(config("commit-failure"), file_system, queue);
    const Fixture prior = fixture(21);
    const Fixture next  = fixture(22);
    stage_and_commit(backend, prior);
    const std::vector<std::byte> prior_manifest =
        file_system->file_with_suffix("checkpoint.manifest.v1");

    backend.stage(next.manifest, next.payloads, next.key);
    file_system->fail_manifest_publish = true;
    int failures                       = 0;
    failures += check(throws([&] { backend.commit(next.key); }),
                      "manifest publication failure was ignored");
    failures += check(file_system->file_with_suffix("checkpoint.manifest.v1") == prior_manifest &&
                          backend.committed_generation() == prior.manifest.generation,
                      "failed commit displaced the prior authoritative checkpoint");
    file_system->fail_manifest_publish = false;
    backend.commit(next.key);
    failures += check(backend.committed_generation() == next.manifest.generation &&
                          image_matches(backend.load(next.expectation), next),
                      "manifest publication retry did not reuse the durable payload");

    const Fixture third = fixture(23);
    backend.stage(third.manifest, third.payloads, third.key);
    CheckpointStageKey untrusted = third.key;
    ++untrusted.generation;
    backend.abort(untrusted);
    backend.commit(third.key);
    failures += check(backend.committed_generation() == third.manifest.generation,
                      "mismatched abort key removed the owned staged transaction");
    return failures;
}

int test_stage_cleanup_stale_and_single_flight() {
    auto file_system = std::make_shared<FakeFileSystem>();
    auto queue_a     = std::make_shared<FakeReadQueue>(file_system);
    auto queue_b     = std::make_shared<FakeReadQueue>(file_system);
    DirectStorageCheckpointBackend backend_a(config("single-flight"), file_system, queue_a);
    DirectStorageCheckpointBackend backend_b(config("single-flight"), file_system, queue_b);
    const Fixture first  = fixture(31);
    const Fixture second = fixture(32);
    int failures         = 0;

    file_system->fail_manifest_write = true;
    failures += check(throws([&] { backend_a.stage(first.manifest, first.payloads, first.key); }),
                      "stage manifest write failure was ignored");
    failures += check(file_system->count_suffix(".stage") == 0,
                      "throwing stage left private staging state behind");
    file_system->fail_manifest_write = false;

    backend_a.stage(first.manifest, first.payloads, first.key);
    failures +=
        check(throws([&] { backend_b.stage(second.manifest, second.payloads, second.key); }),
              "two backend instances staged concurrently for one directory");
    backend_a.commit(first.key);
    failures += check(backend_b.committed_generation() == first.manifest.generation,
                      "committed generation was not shared across backend instances");
    failures += check(throws([&] { backend_b.stage(first.manifest, first.payloads, first.key); }),
                      "stale committed generation was accepted by another instance");
    return failures;
}

int test_orphan_cleanup_and_corrupt_manifest_recovery() {
    auto file_system                                 = std::make_shared<FakeFileSystem>();
    auto queue                                       = std::make_shared<FakeReadQueue>(file_system);
    const DirectStorageCheckpointConfig value_config = config("recovery");
    const Fixture first                              = fixture(35);
    {
        DirectStorageCheckpointBackend backend(value_config, file_system, queue);
        stage_and_commit(backend, first);
    }

    const std::filesystem::path root    = std::filesystem::absolute(value_config.directory);
    const std::filesystem::path staging = root / ".ninfer-checkpoint-staging-v1";
    {
        std::lock_guard lock(file_system->mutex);
        file_system->files[path_key(staging / "orphan.payload.stage")] = {std::byte{1}};
        file_system->files[path_key(root / ("checkpoint-0000000000000001-" + std::string(64, 'a') +
                                            ".payload"))]              = {std::byte{2}};
    }

    int failures = 0;
    {
        DirectStorageCheckpointBackend cleaned(value_config, file_system, queue);
        failures += check(file_system->count_suffix(".stage") == 0 &&
                              file_system->count_suffix(".payload") == 1 &&
                              file_system->observed_lock_timeout_ms == value_config.lock_timeout_ms,
                          "construction did not clean bounded checkpoint-owned orphans");
    }

    std::vector<std::byte> corrupt = file_system->file_with_suffix("checkpoint.manifest.v1");
    corrupt.front() ^= std::byte{0xff};
    file_system->replace_file_with_suffix("checkpoint.manifest.v1", std::move(corrupt));
    DirectStorageCheckpointBackend recovered(value_config, file_system, queue);
    failures += check(throws([&] { (void)recovered.load(first.expectation); }),
                      "corrupt committed manifest became loadable");
    const Fixture replacement = fixture(36);
    stage_and_commit(recovered, replacement);
    failures += check(!file_system->file_with_suffix("checkpoint.manifest.v1.corrupt").empty() &&
                          file_system->count_suffix(".payload") == 1 &&
                          image_matches(recovered.load(replacement.expectation), replacement),
                      "trusted stage did not quarantine and replace a corrupt manifest");

    {
        std::lock_guard lock(file_system->mutex);
        file_system->files[path_key(staging / "undeletable.payload.stage")] = {std::byte{3}};
    }
    file_system->fail_remove = true;
    failures += check(
        throws([&] { DirectStorageCheckpointBackend blocked(value_config, file_system, queue); }),
        "cleanup failure was silently ignored");
    file_system->fail_remove = false;
    return failures;
}

int test_preallocation_rejections() {
    auto file_system = std::make_shared<FakeFileSystem>();
    auto queue       = std::make_shared<FakeReadQueue>(file_system);
    DirectStorageCheckpointBackend backend(config("preallocation"), file_system, queue);
    const Fixture value = fixture(41);
    stage_and_commit(backend, value);
    int failures = 0;

    Fixture stale = fixture(40);
    failures +=
        check(throws([&] { (void)backend.load(stale.expectation); }) && queue->submit_count == 0,
              "stale generation reached DirectStorage allocation/submission");

    CheckpointExpectation wrong_identity = value.expectation;
    ++wrong_identity.manifest.identity.token_count;
    wrong_identity.manifest_sha256 = checkpoint_manifest_sha256(wrong_identity.manifest);
    failures +=
        check(throws([&] { (void)backend.load(wrong_identity); }) && queue->submit_count == 0,
              "wrong token identity reached DirectStorage allocation/submission");

    auto bounded_queue = std::make_shared<FakeReadQueue>(file_system);
    DirectStorageCheckpointBackend bounded(config("preallocation", 4096), file_system,
                                           bounded_queue);
    failures += check(throws([&] { (void)bounded.load(value.expectation); }) &&
                          bounded_queue->submit_count == 0,
                      "configured payload bound was checked after DirectStorage submission");

    const std::vector<std::byte> canonical =
        file_system->file_with_suffix("checkpoint.manifest.v1");
    std::vector<std::byte> corrupted_manifest = canonical;
    corrupted_manifest.front() ^= std::byte{1};
    file_system->replace_file_with_suffix("checkpoint.manifest.v1", std::move(corrupted_manifest));
    failures +=
        check(throws([&] { (void)backend.load(value.expectation); }) && queue->submit_count == 0,
              "corrupt manifest reached DirectStorage payload submission");
    file_system->replace_file_with_suffix("checkpoint.manifest.v1", canonical);
    return failures;
}

class RecordingQuiescence final : public CheckpointQuiescence {
public:
    CheckpointPauseToken pause_admission() override {
        ++pause_count;
        return CheckpointPauseToken(77);
    }

    void drain_transactions(const CheckpointPauseToken&) override { ++drain_count; }

    void fence_device(const CheckpointPauseToken&) override { ++fence_count; }

    void resume_admission(const CheckpointPauseToken&) noexcept override { ++resume_count; }

    int pause_count  = 0;
    int drain_count  = 0;
    int fence_count  = 0;
    int resume_count = 0;
};

class RecordingRestorer final : public CheckpointRestorer {
public:
    void apply(const CheckpointImage&) override { ++apply_count; }

    int apply_count = 0;
};

int test_corruption_revalidated_by_coordinator() {
    auto file_system = std::make_shared<FakeFileSystem>();
    auto queue       = std::make_shared<FakeReadQueue>(file_system);
    DirectStorageCheckpointBackend backend(config("corruption"), file_system, queue);
    const Fixture value = fixture(51);
    stage_and_commit(backend, value);
    queue->corrupt_after_read = true;

    RecordingQuiescence quiescence;
    RecordingRestorer restorer;
    std::mutex operation_mutex;
    CheckpointCoordinator coordinator(quiescence, backend, operation_mutex);
    int failures = 0;
    failures += check(throws([&] { (void)coordinator.restore(value.expectation, restorer); }),
                      "corrupt DirectStorage bytes passed coordinator revalidation");
    failures += check(queue->wait_count == 1 && quiescence.pause_count == 0 &&
                          quiescence.drain_count == 0 && quiescence.fence_count == 0 &&
                          quiescence.resume_count == 0 && restorer.apply_count == 0,
                      "corrupt bytes paused or mutated the runtime before rejection");
    return failures;
}

int test_explicit_queue_failures() {
    int failures = 0;
    {
        auto file_system = std::make_shared<FakeFileSystem>();
        auto queue       = std::make_shared<FakeReadQueue>(file_system);
        DirectStorageCheckpointBackend backend(config("completion-failure"), file_system, queue);
        const Fixture value = fixture(61);
        stage_and_commit(backend, value);
        queue->fail_wait = true;
        failures += check(throws([&] { (void)backend.load(value.expectation); }) &&
                              queue->submit_count == 1 && queue->wait_count == 1,
                          "DirectStorage completion failure was hidden");
    }
    {
        auto file_system          = std::make_shared<FakeFileSystem>();
        auto queue                = std::make_shared<FakeReadQueue>(file_system);
        queue->is_available       = false;
        queue->reason             = "IDStorageQueue3 creation failed";
        const std::string message = thrown_message([&] {
            DirectStorageCheckpointBackend unavailable(config("unavailable"), file_system, queue);
        });
        failures +=
            check(message.find("Windows DirectStorage 1.3 is unavailable") != std::string::npos &&
                      message.find(queue->reason) != std::string::npos,
                  "DirectStorage capability failure was not explicit");
    }
    return failures;
}

} // namespace

int main() {
    int failures = 0;
    failures += test_transaction_and_completion();
    failures += test_commit_failure_and_trusted_abort();
    failures += test_stage_cleanup_stale_and_single_flight();
    failures += test_orphan_cleanup_and_corrupt_manifest_recovery();
    failures += test_preallocation_rejections();
    failures += test_corruption_revalidated_by_coordinator();
    failures += test_explicit_queue_failures();
    if (failures == 0) {
        std::cout << "DirectStorage checkpoint backend tests passed\n";
        return 0;
    }
    std::cerr << failures << " DirectStorage checkpoint backend tests failed\n";
    return 1;
}
