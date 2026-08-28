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

} // namespace

int main() {
    try {
        int failures = 0;
        failures += test_native_round_trip_and_corruption();
        failures += test_native_lock_timeout();
        failures += test_native_reparse_staging_rejection();
        if (failures == 0) {
            std::cout << "Windows DirectStorage native checkpoint tests passed\n";
            return 0;
        }
        std::cerr << failures << " Windows DirectStorage native checkpoint tests failed\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
