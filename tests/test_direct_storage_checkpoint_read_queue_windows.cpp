#if !defined(_WIN32)
#    error "This test requires native Windows"
#endif

#include "core/direct_storage_engine.h"
#include "runtime/windows/direct_storage_checkpoint_read_queue.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace ninfer::runtime;
using namespace ninfer::runtime::windows;

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << message << '\n';
    return 1;
}

bool rejected(ContinuationCheckpointReadQueue& queue, const std::filesystem::path& path) {
    std::array<std::byte, 4> bytes{};
    const ContinuationCheckpointReadRequest request{.file_offset = 0, .destination = bytes};
    try {
        std::unique_ptr<ContinuationCheckpointReadCompletion> completion =
            queue.submit(path, std::span(&request, 1));
        completion->wait();
        return false;
    } catch (const ContinuationCheckpointReadError&) { return true; }
}

bool capability_error(const std::string& message) {
    return message.find("factory is unavailable") != std::string::npos ||
           message.find("no DXGI adapter") != std::string::npos ||
           message.find("failed to create the CUDA-matched D3D12 device") != std::string::npos;
}

} // namespace

int main() {
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() /
        ("ninfer-directstorage-queue-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path root = base / "root";
    const std::filesystem::path outside = base / "outside.bin";
    std::filesystem::create_directories(root);
    const std::filesystem::path payload = root / "payload.bin";
    const std::array<unsigned char, 8> expected{0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87};
    std::ofstream(payload, std::ios::binary)
        .write(reinterpret_cast<const char*>(expected.data()), expected.size());
    std::ofstream(outside, std::ios::binary)
        .write(reinterpret_cast<const char*>(expected.data()), expected.size());

    std::shared_ptr<ContinuationCheckpointReadQueue> queue;
    if (!ninfer::core::DirectStorageEngine::instance().available()) {
        std::filesystem::remove_all(base);
        std::cout << "SKIP: legacy DirectStorage engine is unavailable\n";
        return 77;
    }
    try {
        queue = make_direct_storage_checkpoint_read_queue(
            std::filesystem::path(root.native() + L"\\"), 30'000);
    } catch (const ContinuationCheckpointReadError& error) {
        std::filesystem::remove_all(base);
        if (capability_error(error.what())) {
            std::cout << "SKIP: " << error.what() << '\n';
            return 77;
        }
        std::cerr << error.what() << '\n';
        return 1;
    }

    int failures = 0;
    failures += check(queue && queue->available() &&
                          queue->backend_name() == "directstorage-1.3",
                      "DirectStorage queue did not report its native capability");
    std::array<std::byte, 8> restored{};
    const ContinuationCheckpointReadRequest request{.file_offset = 0, .destination = restored};
    try {
        std::unique_ptr<ContinuationCheckpointReadCompletion> completion =
            queue->submit(payload, std::span(&request, 1));
        completion->wait();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ++failures;
    }
    failures += check(std::equal(restored.begin(), restored.end(),
                                 reinterpret_cast<const std::byte*>(expected.data())),
                      "DirectStorage queue restored the wrong bytes");
    failures += check(rejected(*queue, outside),
                      "DirectStorage queue accepted a path outside its configured root");

    const std::filesystem::path link = root / "reparse.bin";
    if (CreateSymbolicLinkW(link.c_str(), outside.c_str(), 0)) {
        failures += check(rejected(*queue, link),
                          "DirectStorage queue followed a reparse-point payload");
    }

    queue.reset();
    std::filesystem::remove_all(base);
    if (failures == 0) {
        std::cout << "DirectStorage checkpoint read queue tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
