#include "runtime/platform/linux/io_uring_checkpoint_backend.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace ninfer::runtime;

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

class TempDirectory {
public:
    TempDirectory() {
        const char* configured = std::getenv("NINFER_CHECKPOINT_TEST_ROOT");
        const auto base = configured == nullptr ? std::filesystem::temp_directory_path()
                                                 : std::filesystem::path(configured);
        std::filesystem::create_directories(base);
        std::string pattern = (base / "ninfer-read-queue-test-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created == nullptr) { throw std::runtime_error(std::strerror(errno)); }
        path = created;
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

struct Failure {
    std::error_code code;
    std::string message;
    bool contract = false;

    bool operator==(const Failure&) const = default;
};

template <class Action>
Failure failure(Action action) {
    try {
        action();
    } catch (const std::system_error& error) {
        return {error.code(), error.what(), false};
    } catch (const CheckpointContractError& error) {
        return {{}, error.what(), true};
    }
    throw std::runtime_error("read queue accepted an operation that must fail");
}

void test_poisoned_queue(const std::filesystem::path& root, bool attribution_failure) {
    const auto payload = root / "payload";
    // Two chunks exercise the existing partial-submission fault and ring teardown.
    const std::vector<std::byte> bytes(4U * 1024U * 1024U + 1U, std::byte{0x6d});
    {
        std::ofstream output(payload, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        output.close();
        require(bool(output), "could not write read-queue payload");
    }
    auto queue = make_io_uring_checkpoint_read_queue(root);
    require(queue->available() && queue->unavailable_reason().empty(),
            "new read queue is unavailable");
    std::vector<std::byte> restored(bytes.size());
    const ContinuationCheckpointReadRequest request{.file_offset = 0, .destination = restored};
    const Failure missing = failure([&] { (void)queue->submit(root / "missing", {&request, 1}); });
    require(missing.code == std::errc::no_such_file_or_directory && queue->available(),
            "an ordinary file-open failure poisoned the ring or lost its OS code");
    queue->submit(payload, {&request, 1})->wait();
    require(restored == bytes, "healthy queue did not read the original payload");

    if (attribution_failure) {
        io_uring_checkpoint_test_fail_next_completion_key();
    } else {
        io_uring_checkpoint_test_fail_next_submitted_batch();
    }
    auto completion = queue->submit(payload, {&request, 1});
    // This can be queued before the worker observes the fault or refused after it.
    std::vector<std::byte> next_bytes(bytes.size(), std::byte{0x37});
    const ContinuationCheckpointReadRequest next_request{.file_offset = 0,
                                                         .destination = next_bytes};
    std::unique_ptr<ContinuationCheckpointReadCompletion> next;
    Failure next_failure;
    try {
        next = queue->submit(payload, {&next_request, 1});
    } catch (const std::system_error& error) {
        next_failure = {error.code(), error.what(), false};
    } catch (const CheckpointContractError& error) {
        next_failure = {{}, error.what(), true};
    }

    const Failure original = failure([&] { completion->wait(); });
    if (attribution_failure) {
        require(original.contract &&
                    original.message.find("unknown completion key") != std::string::npos,
                "attribution failure did not retain its contract diagnostic");
    } else {
        require(original.code == std::errc::io_error,
                "partial-submission failure lost EIO across the worker");
    }
    require(!queue->available() && !queue->unavailable_reason().empty(),
            "a closed ring still advertised an available read queue");
    if (next) { next_failure = failure([&] { next->wait(); }); }
    require(next_failure == original,
            "a queued or racing submission did not retain the original ring failure");
    require(std::all_of(next_bytes.begin(), next_bytes.end(),
                        [](std::byte value) { return value == std::byte{0x37}; }),
            "a poisoned read queue wrote a later batch");
    require(failure([&] { (void)queue->submit(payload, {&request, 1}); }) == original,
            "later submission lost the original ring failure");
    require(failure([&] { (void)queue->submit(root / "missing", {&request, 1}); }) == original,
            "poisoned queue reopened a file instead of reporting its ring failure");
    queue.reset();

    auto replacement = make_io_uring_checkpoint_read_queue(root);
    replacement->submit(payload, {&request, 1})->wait();
    require(replacement->available() && restored == bytes,
            "a newly created queue could not recover after the old ring failed");
}

void test_handle_allocation_failure(const std::filesystem::path& root) {
    const auto payload = root / "allocation-payload";
    const std::vector<std::byte> bytes(4096, std::byte{0x6d});
    {
        std::ofstream output(payload, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        output.close();
        require(bool(output), "could not write allocation-failure payload");
    }
    auto queue = make_io_uring_checkpoint_read_queue(root);
    std::vector<std::byte> destination(bytes.size());
    const ContinuationCheckpointReadRequest request{0, destination};
    io_uring_checkpoint_test_pause_worker(true);
    io_uring_checkpoint_test_fail_next_handle_allocation();
    bool allocation_failed = false;
    try {
        (void)queue->submit(payload, {&request, 1});
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    } catch (...) {
        io_uring_checkpoint_test_pause_worker(false);
        throw;
    }
    // Logically retire/reuse caller memory before allowing any queued worker to run.
    std::fill(destination.begin(), destination.end(), std::byte{0x37});
    io_uring_checkpoint_test_pause_worker(false);
    queue.reset(); // join, including the unreturned batch on the unfixed implementation
    require(allocation_failed, "handle allocation fault was not exercised");
    require(std::all_of(destination.begin(), destination.end(),
                        [](std::byte value) { return value == std::byte{0x37}; }),
            "submission failure left a worker writing retired caller memory");
    auto replacement = make_io_uring_checkpoint_read_queue(root);
    replacement->submit(payload, {&request, 1})->wait();
    require(destination == bytes, "read did not recover after handle allocation failure");
}

void test_delayed_teardown(const std::filesystem::path& root) {
    const auto payload = root / "delayed-payload";
    {
        std::ofstream output(payload, std::ios::binary);
        const std::string bytes(4096, '\x6d');
        output.write(bytes.data(), bytes.size());
        output.close();
        require(bool(output), "could not write delayed teardown payload");
    }
    const auto evidence = io_uring_checkpoint_test_delayed_teardown(payload);
    std::cout << "accepted=" << evidence.accepted << " retired=" << evidence.retired
              << " pending_at_failure=" << evidence.pending_at_failure
              << " poisoned_buffer_unchanged=" << evidence.poisoned_buffer_unchanged << '\n';
    require(evidence.accepted == 2 && evidence.pending_at_failure == 2,
            "delayed teardown did not exercise accepted outstanding operations");
    require(evidence.retired == evidence.accepted,
            "fatal reset released buffers without retiring every accepted operation by CQE");
    require(evidence.poisoned_buffer_unchanged,
            "delayed I/O modified the retired poisoned buffer");
}

} // namespace

int main(int argc, char** argv) {
    try {
        TempDirectory root;
        if (argc == 2 && std::string_view(argv[1]) == "teardown") {
            test_delayed_teardown(root.path);
            return 0;
        }
        const auto capability = probe_io_uring_checkpoint_capability(root.path);
        if (!capability.available) {
            std::cout << "SKIP: native io_uring/O_DIRECT unavailable: " << capability.reason << '\n';
            return 77;
        }
        if (argc == 2 && std::string_view(argv[1]) == "handle-allocation") {
            test_handle_allocation_failure(root.path);
            return 0;
        }
        test_poisoned_queue(root.path, false);
        test_poisoned_queue(root.path, true);
        std::cout << "io_uring read queue failure and recovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
