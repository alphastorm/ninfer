#if !defined(_WIN32)
#    error "This test requires native Windows"
#endif

#include "runtime/windows/direct_storage_checkpoint_backend.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace ninfer::runtime;
using namespace ninfer::runtime::windows;
using ninfer::crypto::sha256;

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

bool throws_contract(const std::function<void()>& action) {
    try {
        action();
    } catch (const CheckpointContractError&) { return true; }
    return false;
}

CheckpointDigest digest(std::uint8_t seed) {
    CheckpointDigest value{};
    for (std::size_t index = 0; index < value.size(); ++index) {
        value[index] = static_cast<std::uint8_t>(seed + index * 13U);
    }
    return value;
}

struct Fixture {
    CheckpointManifestV1 manifest;
    std::vector<CheckpointPayload> payloads;
    CheckpointStageKey key;
    CheckpointExpectation expectation;
};

Fixture fixture(std::uint64_t generation) {
    Fixture value;
    value.manifest.magic                       = kCheckpointMagic;
    value.manifest.schema_version              = kCheckpointSchemaVersion;
    value.manifest.journal_version             = kCheckpointJournalVersion;
    value.manifest.generation                  = generation;
    value.manifest.identity.model              = digest(1);
    value.manifest.identity.runtime_source     = digest(2);
    value.manifest.identity.deployment_profile = digest(3);
    value.manifest.identity.layout             = digest(4);
    value.manifest.identity.token_count        = 257;
    value.manifest.identity.context_capacity   = 4096;
    value.payloads                             = {
        {CheckpointPayloadKind::StateImage, std::vector<std::byte>(5003)},
        {CheckpointPayloadKind::MainKv, std::vector<std::byte>((1U << 20U) + 777U)},
    };
    std::uint8_t seed = 17;
    for (CheckpointPayload& payload : value.payloads) {
        for (std::size_t index = 0; index < payload.bytes.size(); ++index) {
            payload.bytes[index] = static_cast<std::byte>((seed + index * 29U) & 0xffU);
        }
        value.manifest.payloads.push_back(
            {payload.kind, payload.bytes.size(), sha256(payload.bytes)});
        seed = static_cast<std::uint8_t>(seed + 41U);
    }
    const CheckpointDigest manifest_digest = checkpoint_manifest_sha256(value.manifest);
    value.key         = {kCheckpointJournalVersion, generation, manifest_digest};
    value.expectation = {value.manifest, manifest_digest};
    return value;
}

bool image_matches(const CheckpointImage& image, const Fixture& expected) {
    if (checkpoint_manifest_sha256(image.manifest) != expected.key.manifest_sha256 ||
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

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::wstring_view label) {
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        path_ = base / (std::wstring(label) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                        std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&)            = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void flip_manifest_byte(const std::filesystem::path& root) {
    const std::filesystem::path manifest = root / "checkpoint.manifest.v1";
    std::fstream stream(manifest, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) { throw std::runtime_error("failed to open manifest for corruption test"); }
    char byte = 0;
    stream.read(&byte, 1);
    if (stream.gcount() != 1) { throw std::runtime_error("failed to read manifest byte"); }
    byte ^= static_cast<char>(0x80);
    stream.seekp(0);
    stream.write(&byte, 1);
    stream.flush();
    if (!stream) { throw std::runtime_error("failed to persist corrupt manifest byte"); }
}

int test_native_round_trip_and_corruption() {
    TemporaryDirectory root(L"ninfer-directstorage-native");
    DirectStorageCheckpointConfig config;
    config.directory            = root.path();
    config.max_checkpoint_bytes = 8ULL << 20U;
    config.lock_timeout_ms      = 250;
    config.io_timeout_ms        = 30'000;

    auto backend         = make_direct_storage_checkpoint_backend(config);
    const Fixture first  = fixture(10);
    const Fixture second = fixture(11);
    backend->stage(first.manifest, first.payloads, first.key);
    backend->commit(first.key);

    int failures = 0;
    failures += check(image_matches(backend->load(first.expectation), first),
                      "native DirectStorage round-trip changed payload bytes");

    backend.reset();
    flip_manifest_byte(root.path());
    auto recovered = make_direct_storage_checkpoint_backend(config);
    failures += check(throws_contract([&] { (void)recovered->load(first.expectation); }),
                      "native corrupt manifest became loadable");
    const Fixture lower = fixture(9);
    failures +=
        check(throws_contract([&] { recovered->stage(lower.manifest, lower.payloads, lower.key); }),
              "native corrupt manifest reset the generation floor");
    recovered->stage(second.manifest, second.payloads, second.key);
    recovered->commit(second.key);
    failures += check(image_matches(recovered->load(second.expectation), second),
                      "native replacement after corrupt quarantine failed");
    return failures;
}

int test_native_lock_timeout() {
    TemporaryDirectory root(L"ninfer-directstorage-lock");
    const std::filesystem::path staging = root.path() / ".ninfer-checkpoint-staging-v1";
    std::filesystem::create_directories(staging);
    const std::filesystem::path lock_path = root.path() / ".checkpoint.lock";
    HANDLE handle = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }
    OVERLAPPED overlapped{};
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlapped)) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        throw std::system_error(static_cast<int>(error), std::system_category());
    }

    DirectStorageCheckpointConfig config;
    config.directory       = root.path();
    config.lock_timeout_ms = 50;
    config.io_timeout_ms   = 5'000;
    const bool rejected    = throws_contract([&] {
        auto blocked = make_direct_storage_checkpoint_backend(config);
        (void)blocked;
    });
    UnlockFileEx(handle, 0, 1, 0, &overlapped);
    CloseHandle(handle);
    return check(rejected, "native foreign lock did not respect the configured deadline");
}

int test_native_reparse_staging_rejection() {
    TemporaryDirectory root(L"ninfer-directstorage-reparse");
    TemporaryDirectory target(L"ninfer-directstorage-target");
    const std::filesystem::path sentinel = target.path() / "sentinel.txt";
    std::ofstream(sentinel) << "preserve";
    const std::filesystem::path staging = root.path() / ".ninfer-checkpoint-staging-v1";
    const DWORD flags = SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (!CreateSymbolicLinkW(staging.c_str(), target.path().c_str(), flags)) {
        std::cout << "SKIP: unprivileged directory symlink unavailable for reparse test\n";
        return 0;
    }

    DirectStorageCheckpointConfig config;
    config.directory    = root.path();
    const bool rejected = throws_contract([&] {
        auto backend = make_direct_storage_checkpoint_backend(config);
        (void)backend;
    });
    return check(rejected && std::filesystem::is_regular_file(sentinel),
                 "native reparse staging path escaped cleanup ownership");
}

int test_native_read_queue_root_confinement() {
    TemporaryDirectory root(L"ninfer-directstorage-read-root");
    TemporaryDirectory outside(L"ninfer-directstorage-read-outside");
    std::vector<std::byte> expected(4097);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expected[index] = static_cast<std::byte>((index * 31U + 7U) & 0xffU);
    }
    const auto write = [&](const std::filesystem::path& path) {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(expected.data()),
                     static_cast<std::streamsize>(expected.size()));
        if (!output) { throw std::runtime_error("failed to write read-queue fixture"); }
    };
    const std::filesystem::path inside  = root.path() / "inside.bin";
    const std::filesystem::path escaped = outside.path() / "escaped.bin";
    write(inside);
    write(escaped);

    auto queue = make_direct_storage_checkpoint_read_queue(root.path(), 30'000);
    std::vector<std::byte> restored(expected.size());
    const ContinuationCheckpointReadRequest request{.file_offset = 0, .destination = restored};
    std::unique_ptr<ContinuationCheckpointReadCompletion> completion =
        queue->submit(inside, std::span(&request, 1));
    if (!completion) { return check(false, "native read queue returned no completion"); }
    completion->wait();
    int failures = check(restored == expected, "native read queue changed in-root payload bytes");
    failures +=
        check(throws_contract([&] { (void)queue->submit(escaped, std::span(&request, 1)); }),
              "native DirectStorage queue accepted an out-of-root payload");

    const std::filesystem::path reparse = root.path() / "reparse.bin";
    const DWORD flags                   = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (CreateSymbolicLinkW(reparse.c_str(), escaped.c_str(), flags)) {
        failures +=
            check(throws_contract([&] { (void)queue->submit(reparse, std::span(&request, 1)); }),
                  "native DirectStorage queue followed a payload reparse point");
    }
    return failures;
}

} // namespace

int main() {
    try {
        int failures = 0;
        failures += test_native_round_trip_and_corruption();
        failures += test_native_lock_timeout();
        failures += test_native_reparse_staging_rejection();
        failures += test_native_read_queue_root_confinement();
        if (failures == 0) {
            std::cout << "Windows DirectStorage native checkpoint tests passed\n";
            return 0;
        }
        std::cerr << failures << " Windows DirectStorage native checkpoint tests failed\n";
        return 1;
    } catch (const CheckpointContractError& error) {
        const std::string message = error.what();
        if (message.find("failed to query the active CUDA device") != std::string::npos ||
            message.find("no DXGI adapter matches the active CUDA device") != std::string::npos ||
            message.find("DirectStorage 1.3 factory is unavailable") != std::string::npos) {
            std::cout << "SKIP: native DirectStorage capability unavailable: " << message << '\n';
            return 77;
        }
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
