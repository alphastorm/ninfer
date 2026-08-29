#include "runtime/platform/linux/io_uring_checkpoint_backend.h"

#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace ninfer::runtime;

int check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }
    return 0;
}

bool throws_checkpoint(const std::function<void()>& action) {
    try {
        action();
    } catch (const CheckpointContractError&) { return true; }
    return false;
}

std::string thrown_checkpoint_message(const std::function<void()>& action) {
    try {
        action();
    } catch (const CheckpointContractError& error) { return error.what(); }
    return {};
}

CheckpointDigest digest(std::uint8_t seed) {
    CheckpointDigest value{};
    for (std::size_t index = 0; index < value.size(); ++index) {
        value[index] = static_cast<std::uint8_t>(seed + index * 17U);
    }
    return value;
}

class TempDirectory {
public:
    TempDirectory() {
        const char* configured           = std::getenv("NINFER_CHECKPOINT_TEST_ROOT");
        const std::filesystem::path base = configured == nullptr
                                               ? std::filesystem::path("/tmp")
                                               : std::filesystem::path(configured);
        std::filesystem::create_directories(base);
        std::string pattern = (base / "ninfer-io-uring-test-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = ::mkdtemp(writable.data());
        if (created == nullptr) { throw std::runtime_error(std::strerror(errno)); }
        path_ = created;
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempDirectory(const TempDirectory&)            = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

struct Fixture {
    CheckpointManifestV1 manifest;
    std::vector<CheckpointPayload> payloads;
    CheckpointStageKey key;
    CheckpointExpectation expectation;
};

Fixture fixture(std::uint64_t generation,
                std::initializer_list<std::pair<CheckpointPayloadKind, std::size_t>> shapes) {
    Fixture result;
    result.manifest.magic                       = kCheckpointMagic;
    result.manifest.schema_version              = kCheckpointSchemaVersion;
    result.manifest.journal_version             = kCheckpointJournalVersion;
    result.manifest.generation                  = generation;
    result.manifest.identity.model              = digest(1);
    result.manifest.identity.runtime_source     = digest(2);
    result.manifest.identity.deployment_profile = digest(3);
    result.manifest.identity.layout             = digest(4);
    result.manifest.identity.token_count        = 73;
    result.manifest.identity.context_capacity   = 4096;

    std::uint8_t seed = 11;
    for (const auto& [kind, size] : shapes) {
        CheckpointPayload payload;
        payload.kind = kind;
        payload.bytes.resize(size);
        for (std::size_t index = 0; index < size; ++index) {
            payload.bytes[index] = static_cast<std::byte>((seed + index * 29U) & 0xffU);
        }
        result.manifest.payloads.push_back(
            CheckpointPayloadDescriptor{kind, size, sha256(payload.bytes)});
        result.payloads.push_back(std::move(payload));
        seed = static_cast<std::uint8_t>(seed + 37U);
    }

    const CheckpointDigest manifest_digest = checkpoint_manifest_sha256(result.manifest);
    result.key         = {kCheckpointJournalVersion, generation, manifest_digest};
    result.expectation = {result.manifest, manifest_digest};
    return result;
}

void save(IoUringCheckpointBackend& backend, const Fixture& checkpoint) {
    backend.stage(checkpoint.manifest, checkpoint.payloads, checkpoint.key);
    try {
        backend.commit(checkpoint.key);
    } catch (...) {
        backend.abort(checkpoint.key);
        throw;
    }
}

bool images_equal(const CheckpointImage& image, const Fixture& expected) {
    if (checkpoint_manifest_sha256(image.manifest) != expected.expectation.manifest_sha256 ||
        image.payloads.size() != expected.payloads.size()) {
        return false;
    }
    for (std::size_t index = 0; index < image.payloads.size(); ++index) {
        if (image.payloads[index].kind != expected.payloads[index].kind ||
            image.payloads[index].bytes != expected.payloads[index].bytes) {
            return false;
        }
    }
    return true;
}

std::string generation_name(const Fixture& checkpoint) {
    std::string generation = std::to_string(checkpoint.manifest.generation);
    if (generation.size() < 20) {
        generation.insert(generation.begin(), 20 - generation.size(), '0');
    }
    return "generation-" + generation + "-" + sha256_hex(checkpoint.key.manifest_sha256);
}

std::filesystem::path payload_path(const TempDirectory& root, const Fixture& checkpoint,
                                   std::size_t descriptor_index = 0) {
    const unsigned kind =
        static_cast<unsigned>(checkpoint.manifest.payloads[descriptor_index].kind);
    return root.path() / generation_name(checkpoint) / ("payload-" + std::to_string(kind));
}

std::size_t count_prefixed_entries(const std::filesystem::path& root, std::string_view prefix) {
    std::size_t count = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with(prefix)) { ++count; }
    }
    return count;
}

void overwrite_byte(const std::filesystem::path& path, off_t offset) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) { throw std::runtime_error(std::strerror(errno)); }
    std::byte value{};
    if (::pread(fd, &value, 1, offset) != 1) {
        const int error = errno;
        ::close(fd);
        throw std::runtime_error(std::strerror(error));
    }
    value ^= std::byte{0x80};
    if (::pwrite(fd, &value, 1, offset) != 1 || ::fsync(fd) != 0) {
        const int error = errno;
        ::close(fd);
        throw std::runtime_error(std::strerror(error));
    }
    ::close(fd);
}

class ScopedFileSizeLimit {
public:
    explicit ScopedFileSizeLimit(rlim_t bytes) {
        if (::getrlimit(RLIMIT_FSIZE, &original_limit_) != 0) {
            throw std::runtime_error(std::strerror(errno));
        }
        if (::sigaction(SIGXFSZ, nullptr, &original_action_) != 0) {
            throw std::runtime_error(std::strerror(errno));
        }
        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        ::sigemptyset(&ignored.sa_mask);
        if (::sigaction(SIGXFSZ, &ignored, nullptr) != 0) {
            throw std::runtime_error(std::strerror(errno));
        }
        rlimit limited   = original_limit_;
        limited.rlim_cur = std::min(bytes, original_limit_.rlim_max);
        if (limited.rlim_cur < bytes || ::setrlimit(RLIMIT_FSIZE, &limited) != 0) {
            ::sigaction(SIGXFSZ, &original_action_, nullptr);
            throw std::runtime_error("unable to establish deterministic RLIMIT_FSIZE");
        }
        active_ = true;
    }

    ~ScopedFileSizeLimit() {
        if (!active_) { return; }
        ::setrlimit(RLIMIT_FSIZE, &original_limit_);
        ::sigaction(SIGXFSZ, &original_action_, nullptr);
    }

    ScopedFileSizeLimit(const ScopedFileSizeLimit&)            = delete;
    ScopedFileSizeLimit& operator=(const ScopedFileSizeLimit&) = delete;

private:
    rlimit original_limit_{};
    struct sigaction original_action_{};
    bool active_ = false;
};

int test_unavailable_capability() {
    int failures = 0;
    TempDirectory parent;
    const std::filesystem::path missing = parent.path() / "does-not-exist";
    const IoUringCheckpointCapability missing_capability =
        probe_io_uring_checkpoint_capability(missing);
    failures += check(!missing_capability.available && !missing_capability.reason.empty(),
                      "a missing checkpoint root must report an explicit unavailable capability");
    failures += check(throws_checkpoint([&] { IoUringCheckpointBackend backend(missing); }),
                      "backend construction must fail when storage capability is unavailable");

    if (std::filesystem::is_directory("/dev/shm")) {
        std::string pattern = "/dev/shm/ninfer-io-uring-unavailable-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        if (char* path = ::mkdtemp(writable.data())) {
            const std::filesystem::path temporary(path);
            const IoUringCheckpointCapability capability =
                probe_io_uring_checkpoint_capability(temporary);
            failures += check(!capability.available && !capability.reason.empty(),
                              "tmpfs must not claim local-NVMe checkpoint readiness");
            std::filesystem::remove_all(temporary);
        }
    }
    return failures;
}

int test_transaction_and_trusted_abort() {
    int failures = 0;
    TempDirectory root;
    IoUringCheckpointBackend backend(root.path());
    const Fixture checkpoint =
        fixture(1, {{CheckpointPayloadKind::StateImage, 5003},
                    {CheckpointPayloadKind::MainKv, 4U * 1024U * 1024U + 9001U}});

    const CheckpointBackendCapabilities capabilities = backend.capabilities();
    failures += check(capabilities.atomic_replace && capabilities.durable_flush &&
                          !capabilities.direct_to_device,
                      "the backend must not misreport O_DIRECT as storage-to-device I/O");
    backend.stage(checkpoint.manifest, checkpoint.payloads, checkpoint.key);
    failures += check(!std::filesystem::exists(root.path() / "manifest"),
                      "stage must not publish the authoritative manifest");
    failures += check(count_prefixed_entries(root.path(), ".stage-") == 1,
                      "stage must create exactly one transactional generation");
    const int contender = ::open((root.path() / ".checkpoint.lock").c_str(), O_RDWR | O_CLOEXEC);
    if (contender < 0) { throw std::runtime_error(std::strerror(errno)); }
    errno = 0;
    failures += check(::flock(contender, LOCK_EX | LOCK_NB) != 0 &&
                          (errno == EWOULDBLOCK || errno == EAGAIN),
                      "stage must hold the cross-instance single-flight lock through commit/abort");
    ::close(contender);

    CheckpointStageKey wrong_key = checkpoint.key;
    wrong_key.manifest_sha256[0] ^= 0xffU;
    backend.abort(wrong_key);
    failures += check(count_prefixed_entries(root.path(), ".stage-") == 1,
                      "an untrusted abort key must not remove active staging state");

    backend.commit(checkpoint.key);
    failures += check(std::filesystem::exists(root.path() / "manifest"),
                      "commit must publish the authoritative manifest last");
    failures += check(count_prefixed_entries(root.path(), ".stage-") == 0,
                      "commit must consume the staging generation");
    failures += check(count_prefixed_entries(root.path(), "generation-") == 1,
                      "commit must leave exactly one authoritative generation");
    failures +=
        check(backend.committed_generation() == 1, "commit must advance the backend generation");
    failures += check(images_equal(backend.load(checkpoint.expectation), checkpoint),
                      "a committed logical-length checkpoint must round-trip exactly");

    const Fixture aborted = fixture(
        2, {{CheckpointPayloadKind::StateImage, 4097}, {CheckpointPayloadKind::MainKv, 8191}});
    backend.stage(aborted.manifest, aborted.payloads, aborted.key);
    backend.abort(aborted.key);
    failures += check(count_prefixed_entries(root.path(), ".stage-") == 0,
                      "trusted abort must remove its transaction");
    failures += check(images_equal(backend.load(checkpoint.expectation), checkpoint),
                      "aborting a newer stage must preserve the prior committed checkpoint");
    return failures;
}

int test_cross_instance_stale_generation() {
    int failures = 0;
    TempDirectory root;
    IoUringCheckpointBackend first(root.path());
    IoUringCheckpointBackend second(root.path());
    const Fixture current = fixture(2, {{CheckpointPayloadKind::StateImage, 6001}});
    save(first, current);
    failures += check(second.committed_generation() == 2,
                      "committed generation must refresh across backend instances");

    const Fixture stale = fixture(1, {{CheckpointPayloadKind::StateImage, 7001}});
    failures +=
        check(throws_checkpoint([&] { second.stage(stale.manifest, stale.payloads, stale.key); }),
              "a pre-existing backend instance must reject a generation stale on disk");
    failures += check(count_prefixed_entries(root.path(), ".stage-") == 0,
                      "stale-generation rejection must not leave staging state");
    failures += check(images_equal(second.load(current.expectation), current),
                      "the stale-rejecting instance must observe the committed generation");

    IoUringCheckpointLimits short_lock;
    short_lock.lock_timeout_ms = 25;
    TempDirectory contended_root;
    IoUringCheckpointBackend owner(contended_root.path(), short_lock);
    IoUringCheckpointBackend observer(contended_root.path(), short_lock);
    const Fixture staged = fixture(3, {{CheckpointPayloadKind::StateImage, 4097}});
    owner.stage(staged.manifest, staged.payloads, staged.key);
    const auto started = std::chrono::steady_clock::now();
    failures += check(observer.committed_generation() == 0,
                      "contended noexcept generation query changed its cached value");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    failures += check(elapsed < std::chrono::seconds(1),
                      "contended generation query blocked beyond its configured deadline");
    failures += check(
        throws_checkpoint([&] { observer.stage(staged.manifest, staged.payloads, staged.key); }),
        "contended stage did not reject at its configured deadline");
    owner.abort(staged.key);
    return failures;
}

int test_uncertain_marker_and_foreign_entries_fail_safe() {
    int failures = 0;
    TempDirectory root;
    const std::filesystem::path foreign = root.path() / "generation-x";
    {
        const int fd = ::open(foreign.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (fd < 0) { throw std::runtime_error(std::strerror(errno)); }
        ::close(fd);
    }
    const std::filesystem::path exact_foreign =
        root.path() / ("generation-00000000000000000009-" + std::string(64, 'c'));
    std::filesystem::create_directory(exact_foreign);
    std::ofstream(exact_foreign / "foreign-entry") << "preserve";
    const Fixture current = fixture(7, {{CheckpointPayloadKind::StateImage, 5001}});
    {
        IoUringCheckpointBackend backend(root.path());
        save(backend, current);
    }
    failures += check(std::filesystem::is_regular_file(foreign) &&
                          std::filesystem::is_regular_file(exact_foreign / "foreign-entry"),
                      "orphan cleanup claimed a foreign generation-prefixed entry");
    failures += check(!std::filesystem::exists(root.path() / ".publication-uncertain"),
                      "successful commit retained its uncertainty marker");

    const std::filesystem::path marker = root.path() / ".publication-uncertain";
    {
        const int fd = ::open(marker.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (fd < 0) { throw std::runtime_error(std::strerror(errno)); }
        const std::byte value{1};
        if (::write(fd, &value, 1) != 1 || ::fsync(fd) != 0) {
            const int error = errno;
            ::close(fd);
            throw std::runtime_error(std::strerror(error));
        }
        ::close(fd);
    }
    failures += check(throws_checkpoint([&] { IoUringCheckpointBackend blocked(root.path()); }),
                      "persistent publication uncertainty was silently promoted");
    failures += check(count_prefixed_entries(root.path(), "generation-") == 3,
                      "uncertain construction deleted a prior or foreign generation entry");
    return failures;
}

int test_partial_failure_cleanup() {
    int failures = 0;
    TempDirectory root;
    IoUringCheckpointBackend backend(root.path());
    const Fixture checkpoint = fixture(
        1, {{CheckpointPayloadKind::StateImage, 1000}, {CheckpointPayloadKind::MainKv, 8192}});

    bool failed = false;
    {
        ScopedFileSizeLimit limit(4096);
        failed = throws_checkpoint(
            [&] { backend.stage(checkpoint.manifest, checkpoint.payloads, checkpoint.key); });
    }
    failures += check(failed, "an asynchronous short/failed payload write must fail stage");
    failures += check(count_prefixed_entries(root.path(), ".stage-") == 0 &&
                          count_prefixed_entries(root.path(), "generation-") == 0 &&
                          !std::filesystem::exists(root.path() / "manifest"),
                      "partial asynchronous failure must remove all transaction state");

    save(backend, checkpoint);
    failures += check(images_equal(backend.load(checkpoint.expectation), checkpoint),
                      "the backend must remain usable after fully cleaning a failed stage");
    return failures;
}

int test_partial_submit_failure_poison_and_recovery() {
    int failures = 0;
    TempDirectory root;
    const Fixture checkpoint =
        fixture(1, {{CheckpointPayloadKind::StateImage, 4U * 1024U * 1024U + 8193U}});
    {
        IoUringCheckpointBackend backend(root.path());
        io_uring_checkpoint_test_fail_next_submitted_batch();
        const std::string first_failure = thrown_checkpoint_message(
            [&] { backend.stage(checkpoint.manifest, checkpoint.payloads, checkpoint.key); });
        failures += check(first_failure.find("io_uring_enter submit") != std::string::npos,
                          "partial submit failure lost the originating EIO");
        const std::string poisoned_failure = thrown_checkpoint_message(
            [&] { backend.stage(checkpoint.manifest, checkpoint.payloads, checkpoint.key); });
        failures += check(poisoned_failure.find("backend is unusable") != std::string::npos,
                          "a failed io_uring context was reused or hid its dead state");
    }

    IoUringCheckpointBackend recovered(root.path());
    save(recovered, checkpoint);
    failures += check(images_equal(recovered.load(checkpoint.expectation), checkpoint),
                      "a new backend could not recover after failed-ring cleanup");
    return failures;
}

int test_commit_failure_preserves_prior_generation() {
    int failures = 0;
    TempDirectory root;
    IoUringCheckpointBackend backend(root.path());
    const Fixture current = fixture(1, {{CheckpointPayloadKind::StateImage, 5001}});
    save(backend, current);

    const Fixture candidate = fixture(2, {{CheckpointPayloadKind::StateImage, 6001}});
    backend.stage(candidate.manifest, candidate.payloads, candidate.key);
    const std::filesystem::path blocker =
        root.path() / (".publish-" + sha256_hex(candidate.key.manifest_sha256));
    const int blocker_fd = ::open(blocker.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (blocker_fd < 0) { throw std::runtime_error(std::strerror(errno)); }
    ::close(blocker_fd);

    failures += check(throws_checkpoint([&] { backend.commit(candidate.key); }),
                      "a manifest-publication failure must fail commit");
    backend.abort(candidate.key);
    failures += check(count_prefixed_entries(root.path(), ".stage-") == 0 &&
                          count_prefixed_entries(root.path(), "generation-") == 1 &&
                          count_prefixed_entries(root.path(), ".publish-") == 0,
                      "abort after commit failure must remove only the failed generation");
    failures += check(images_equal(backend.load(current.expectation), current),
                      "commit failure must leave the prior manifest authoritative");
    return failures;
}

int test_double_fsync_failure_persists_uncertainty() {
    int failures = 0;
    TempDirectory root;
    const Fixture current   = fixture(1, {{CheckpointPayloadKind::StateImage, 5001}});
    const Fixture candidate = fixture(2, {{CheckpointPayloadKind::StateImage, 6001}});
    {
        IoUringCheckpointBackend backend(root.path());
        save(backend, current);
        backend.stage(candidate.manifest, candidate.payloads, candidate.key);
        io_uring_checkpoint_test_fail_publication_and_rollback_fsync();
        failures += check(throws_checkpoint([&] { backend.commit(candidate.key); }),
                          "double fsync fault did not fail commit");
        backend.abort(candidate.key);
    }

    const std::filesystem::path marker = root.path() / ".publication-uncertain";
    failures += check(std::filesystem::is_regular_file(marker) &&
                          count_prefixed_entries(root.path(), "generation-") == 2,
                      "uncertain commit lost its durable marker or prior generation");
    failures += check(throws_checkpoint([&] { IoUringCheckpointBackend blocked(root.path()); }),
                      "restart silently promoted a reported-failed generation");

    std::filesystem::remove(marker);
    const int root_fd = ::open(root.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0 || ::fsync(root_fd) != 0) {
        const int error = errno;
        if (root_fd >= 0) { ::close(root_fd); }
        throw std::runtime_error(std::strerror(error));
    }
    ::close(root_fd);
    IoUringCheckpointBackend recovered(root.path());
    failures += check(images_equal(recovered.load(current.expectation), current) &&
                          count_prefixed_entries(root.path(), "generation-") == 1,
                      "operator-cleared marker did not recover the prior committed generation");
    return failures;
}

int test_marker_fsync_failure_does_not_lock_out_root() {
    int failures = 0;
    TempDirectory root;
    const Fixture current   = fixture(1, {{CheckpointPayloadKind::StateImage, 5001}});
    const Fixture candidate = fixture(2, {{CheckpointPayloadKind::StateImage, 6001}});
    {
        IoUringCheckpointBackend backend(root.path());
        save(backend, current);
        backend.stage(candidate.manifest, candidate.payloads, candidate.key);
        io_uring_checkpoint_test_fail_publication_marker_fsync();
        failures += check(throws_checkpoint([&] { backend.commit(candidate.key); }),
                          "publication-marker fsync fault did not fail commit");
        backend.abort(candidate.key);
    }
    failures += check(!std::filesystem::exists(root.path() / ".publication-uncertain"),
                      "pre-publication failure left a false uncertainty marker");
    IoUringCheckpointBackend recovered(root.path());
    failures += check(images_equal(recovered.load(current.expectation), current),
                      "pre-publication marker failure made the prior checkpoint unavailable");
    return failures;
}

int test_corruption_rejection() {
    int failures = 0;
    {
        TempDirectory root;
        IoUringCheckpointBackend backend(root.path());
        const Fixture checkpoint = fixture(1, {{CheckpointPayloadKind::StateImage, 7003}});
        save(backend, checkpoint);
        overwrite_byte(payload_path(root, checkpoint), 123);
        failures += check(throws_checkpoint([&] { backend.load(checkpoint.expectation); }),
                          "payload corruption must be rejected by its trusted digest");
    }
    {
        TempDirectory root;
        IoUringCheckpointBackend backend(root.path());
        const Fixture checkpoint = fixture(1, {{CheckpointPayloadKind::StateImage, 7003}});
        save(backend, checkpoint);
        overwrite_byte(root.path() / "manifest", 0);
        failures += check(throws_checkpoint([&] { backend.load(checkpoint.expectation); }),
                          "published manifest corruption must be rejected");
    }
    return failures;
}

int test_truncation_and_oversize_rejection() {
    int failures = 0;
    {
        TempDirectory root;
        IoUringCheckpointBackend backend(root.path());
        const Fixture checkpoint = fixture(1, {{CheckpointPayloadKind::StateImage, 7003}});
        save(backend, checkpoint);
        if (::truncate(payload_path(root, checkpoint).c_str(), 7002) != 0) {
            throw std::runtime_error(std::strerror(errno));
        }
        failures += check(throws_checkpoint([&] { backend.load(checkpoint.expectation); }),
                          "a truncated payload must be rejected before allocation/read");
    }
    {
        TempDirectory root;
        IoUringCheckpointBackend backend(root.path());
        const Fixture checkpoint = fixture(1, {{CheckpointPayloadKind::StateImage, 7003}});
        save(backend, checkpoint);
        const std::filesystem::path path = payload_path(root, checkpoint);
        const int fd                     = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd < 0) { throw std::runtime_error(std::strerror(errno)); }
        const std::byte extra{0x7f};
        if (::write(fd, &extra, 1) != 1 || ::fsync(fd) != 0) {
            const int error = errno;
            ::close(fd);
            throw std::runtime_error(std::strerror(error));
        }
        ::close(fd);
        failures += check(throws_checkpoint([&] { backend.load(checkpoint.expectation); }),
                          "an oversized payload must be rejected before allocation/read");
    }
    {
        TempDirectory root;
        IoUringCheckpointBackend backend(root.path());
        const Fixture checkpoint = fixture(1, {{CheckpointPayloadKind::StateImage, 7003}});
        save(backend, checkpoint);
        const std::filesystem::path path = root.path() / "manifest";
        const int fd                     = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd < 0) { throw std::runtime_error(std::strerror(errno)); }
        std::array<std::byte, 512> extra{};
        if (::write(fd, extra.data(), extra.size()) != static_cast<ssize_t>(extra.size()) ||
            ::fsync(fd) != 0) {
            const int error = errno;
            ::close(fd);
            throw std::runtime_error(std::strerror(error));
        }
        ::close(fd);
        failures += check(throws_checkpoint([&] { backend.load(checkpoint.expectation); }),
                          "an oversized manifest must be rejected before descriptor allocation");
    }
    return failures;
}

int test_expected_allocation_bound() {
    int failures = 0;
    TempDirectory root;
    IoUringCheckpointBackend backend(root.path(), IoUringCheckpointLimits{8192, 16384});
    const Fixture checkpoint = fixture(1, {{CheckpointPayloadKind::StateImage, 6001}});
    save(backend, checkpoint);

    CheckpointExpectation oversized      = checkpoint.expectation;
    oversized.manifest.payloads[0].bytes = 9000;
    oversized.manifest_sha256            = checkpoint_manifest_sha256(oversized.manifest);
    failures += check(throws_checkpoint([&] { backend.load(oversized); }),
                      "trusted expected sizes above configured bounds must fail before allocation");
    return failures;
}

int test_continuation_read_queue() {
    TempDirectory root;
    IoUringCheckpointBackend backend(root.path());
    const Fixture checkpoint = fixture(1, {{CheckpointPayloadKind::StateImage, 7003}});
    save(backend, checkpoint);

    std::shared_ptr<ContinuationCheckpointReadQueue> queue =
        make_io_uring_checkpoint_read_queue(root.path());
    std::vector<std::byte> restored(4097);
    const ContinuationCheckpointReadRequest request{.file_offset = 37, .destination = restored};
    std::unique_ptr<ContinuationCheckpointReadCompletion> completion =
        queue->submit(payload_path(root, checkpoint), std::span(&request, 1));
    if (!completion) { return check(false, "io_uring continuation queue returned no completion"); }
    completion->wait();
    const auto expected = std::span(checkpoint.payloads[0].bytes).subspan(37, restored.size());
    int failures        = check(std::equal(restored.begin(), restored.end(), expected.begin()),
                                "io_uring continuation queue changed an unaligned payload range");
    failures += check(throws_checkpoint([&] {
                          (void)queue->submit(root.path().parent_path() / "escaped-checkpoint",
                                              std::span(&request, 1));
                      }),
                      "io_uring continuation queue accepted a path outside its configured root");
    return failures;
}

} // namespace

int main() {
    try {
        int failures = test_unavailable_capability();

        TempDirectory capability_root;
        const IoUringCheckpointCapability capability =
            probe_io_uring_checkpoint_capability(capability_root.path());
        if (!capability.available) {
            std::cout << "SKIP: native io_uring/O_DIRECT checkpoint capability unavailable: "
                      << capability.reason << '\n';
            return failures == 0 ? 77 : 1;
        }
        if (capability.environment == LinuxCheckpointEnvironment::Wsl &&
            std::filesystem::is_directory("/mnt/c")) {
            const IoUringCheckpointCapability drvfs =
                probe_io_uring_checkpoint_capability("/mnt/c");
            failures += check(!drvfs.available && drvfs.reason.find("WSL") != std::string::npos,
                              "WSL DrvFS must fail explicitly while WSL ext4 remains eligible");
        }

        failures += test_transaction_and_trusted_abort();
        failures += test_cross_instance_stale_generation();
        failures += test_uncertain_marker_and_foreign_entries_fail_safe();
        failures += test_partial_failure_cleanup();
        failures += test_partial_submit_failure_poison_and_recovery();
        failures += test_commit_failure_preserves_prior_generation();
        failures += test_double_fsync_failure_persists_uncertainty();
        failures += test_marker_fsync_failure_does_not_lock_out_root();
        failures += test_corruption_rejection();
        failures += test_truncation_and_oversize_rejection();
        failures += test_expected_allocation_bound();
        failures += test_continuation_read_queue();
        if (failures == 0) {
            const char* environment =
                capability.environment == LinuxCheckpointEnvironment::Wsl ? "WSL" : "Linux";
            std::cout << "io_uring checkpoint backend tests passed (" << environment
                      << ", memory alignment " << capability.memory_alignment
                      << ", offset alignment " << capability.offset_alignment << ")\n";
        }
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
        return 1;
    }
}
