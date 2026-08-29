#if defined(_WIN32)

#    ifndef NOMINMAX
#        define NOMINMAX
#    endif

#    include "runtime/windows/direct_storage_checkpoint_backend.h"

#    include <d3d12.h>
#    include <dxgi1_6.h>
#    include <dstorage.h>
#    include <windows.h>
#    include <wrl/client.h>

#    include <cuda_runtime.h>

#    include <algorithm>
#    include <array>
#    include <cstddef>
#    include <cstdint>
#    include <cstring>
#    include <cwchar>
#    include <exception>
#    include <filesystem>
#    include <limits>
#    include <memory>
#    include <mutex>
#    include <span>
#    include <sstream>
#    include <string>
#    include <utility>
#    include <vector>

namespace ninfer::runtime::windows {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kWindowsIoChunkBytes = 32ULL << 20;

[[noreturn]] void fail_windows(const std::string& operation, std::uint64_t code) {
    std::ostringstream message;
    message << operation << " (0x" << std::hex << code << ')';
    throw CheckpointContractError(message.str());
}

void require_hresult(HRESULT result, const std::string& operation) {
    if (FAILED(result)) { fail_windows(operation, static_cast<std::uint32_t>(result)); }
}

void require_cuda(cudaError_t result, const std::string& operation) {
    if (result != cudaSuccess) {
        throw CheckpointContractError(operation + ": " + cudaGetErrorString(result));
    }
}

class UniqueHandle {
public:
    UniqueHandle() = default;

    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}

    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&)            = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) { reset(other.release()); }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE release() noexcept {
        const HANDLE value = handle_;
        handle_            = INVALID_HANDLE_VALUE;
        return value;
    }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) { CloseHandle(handle_); }
        handle_ = handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

UniqueHandle open_new_durable_file(const std::filesystem::path& path) {
    UniqueHandle handle(CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!handle.valid()) {
        fail_windows("failed to create private checkpoint staging file", GetLastError());
    }
    return handle;
}

void write_all(HANDLE handle, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(bytes.size(), kWindowsIoChunkBytes));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data(), chunk, &written, nullptr) || written != chunk) {
            fail_windows("failed to write checkpoint staging bytes", GetLastError());
        }
        bytes = bytes.subspan(written);
    }
}

class WindowsDirectoryLock final : public CheckpointDirectoryLock {
public:
    WindowsDirectoryLock(const std::filesystem::path& directory, std::uint32_t timeout_ms) {
        const std::filesystem::path lock_path = directory / ".checkpoint.lock";
        handle_.reset(CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_HIDDEN, nullptr));
        if (!handle_.valid()) {
            fail_windows("failed to open checkpoint directory lock", GetLastError());
        }
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        for (;;) {
            if (LockFileEx(handle_.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1,
                           0, &overlapped_)) {
                break;
            }
            const DWORD error = GetLastError();
            if (error != ERROR_LOCK_VIOLATION) {
                fail_windows("failed to acquire checkpoint directory lock", error);
            }
            if (GetTickCount64() >= deadline) {
                throw CheckpointContractError(
                    "timed out acquiring checkpoint directory single-flight lock");
            }
            Sleep(10);
        }
        locked_ = true;
    }

    ~WindowsDirectoryLock() override {
        if (locked_) { UnlockFileEx(handle_.get(), 0, 1, 0, &overlapped_); }
    }

private:
    UniqueHandle handle_;
    OVERLAPPED overlapped_{};
    bool locked_ = false;
};

class WindowsCheckpointFileSystem final : public CheckpointFileSystem {
public:
    // DirectStorage 1.3 queues are read-only. Save publication therefore uses write-through Win32
    // files; restore payloads below are the operations submitted to IDStorageQueue3.
    void ensure_directory(const std::filesystem::path& path) override {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (error || attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw CheckpointContractError(
                "failed to create checkpoint directory: " +
                (error ? error.message() : std::string("not a plain directory")));
        }
    }

    std::unique_ptr<CheckpointDirectoryLock> lock_directory(const std::filesystem::path& path,
                                                            std::uint32_t timeout_ms) override {
        return std::make_unique<WindowsDirectoryLock>(path, timeout_ms);
    }

    std::vector<std::filesystem::path> list_regular_files(const std::filesystem::path& path,
                                                          std::size_t max_matches,
                                                          PathPredicate predicate) override {
        std::vector<std::filesystem::path> files;
        std::error_code error;
        for (std::filesystem::directory_iterator iterator(path, error), end; iterator != end;
             iterator.increment(error)) {
            if (error) {
                throw CheckpointContractError("failed to enumerate checkpoint directory: " +
                                              error.message());
            }
            const std::filesystem::file_status status = iterator->symlink_status(error);
            if (error) {
                throw CheckpointContractError("failed to inspect checkpoint directory entry: " +
                                              error.message());
            }
            if (std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
                if (predicate == nullptr || !predicate(iterator->path())) { continue; }
                if (files.size() == max_matches) {
                    throw CheckpointContractError(
                        "checkpoint cleanup match count exceeds its configured bound");
                }
                files.push_back(iterator->path());
            }
        }
        if (error) {
            throw CheckpointContractError("failed to enumerate checkpoint directory: " +
                                          error.message());
        }
        return files;
    }

    bool file_exists(const std::filesystem::path& path) override {
        WIN32_FIND_DATAW data{};
        const HANDLE search = FindFirstFileW(path.c_str(), &data);
        if (search != INVALID_HANDLE_VALUE) {
            FindClose(search);
            return (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { return false; }
        fail_windows("failed to inspect checkpoint file", error);
    }

    std::uint64_t file_size(const std::filesystem::path& path) override {
        UniqueHandle handle(
            CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!handle.valid()) {
            fail_windows("failed to open checkpoint file for size validation", GetLastError());
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle.get(), &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw CheckpointContractError("checkpoint file size target is not a plain file");
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle.get(), &size) || size.QuadPart < 0) {
            fail_windows("failed to read checkpoint file size", GetLastError());
        }
        return static_cast<std::uint64_t>(size.QuadPart);
    }

    void read_exact(const std::filesystem::path& path, std::span<std::byte> bytes) override {
        UniqueHandle handle(CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!handle.valid()) {
            fail_windows("failed to open committed checkpoint manifest", GetLastError());
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle.get(), &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw CheckpointContractError("committed checkpoint manifest is not a plain file");
        }
        while (!bytes.empty()) {
            const DWORD chunk = static_cast<DWORD>(
                std::min<std::size_t>(bytes.size(), std::numeric_limits<DWORD>::max()));
            DWORD read = 0;
            if (!ReadFile(handle.get(), bytes.data(), chunk, &read, nullptr)) {
                fail_windows("failed to read committed checkpoint manifest", GetLastError());
            }
            if (read == 0) {
                throw CheckpointContractError("committed checkpoint manifest ended early");
            }
            bytes = bytes.subspan(read);
        }
    }

    void write_bytes_durable(const std::filesystem::path& path,
                             std::span<const std::byte> bytes) override {
        UniqueHandle handle;
        try {
            handle = open_new_durable_file(path);
            write_all(handle.get(), bytes);
            if (!FlushFileBuffers(handle.get())) {
                fail_windows("failed to flush checkpoint staging file", GetLastError());
            }
            handle.reset();
        } catch (...) {
            handle.reset();
            DeleteFileW(path.c_str());
            throw;
        }
    }

    void write_payloads_durable(const std::filesystem::path& path,
                                std::span<const CheckpointPayload> payloads,
                                std::span<const std::uint64_t> payload_offsets,
                                std::uint64_t total_bytes) override {
        if (payloads.size() != payload_offsets.size()) {
            throw CheckpointContractError(
                "checkpoint payload writer received inconsistent offsets");
        }

        UniqueHandle handle;
        try {
            handle = open_new_durable_file(path);
            std::array<std::byte, 4096> zeros{};
            std::uint64_t cursor = 0;
            for (std::size_t index = 0; index < payloads.size(); ++index) {
                if (payload_offsets[index] < cursor) {
                    throw CheckpointContractError("checkpoint payload offsets are not monotonic");
                }
                std::uint64_t padding = payload_offsets[index] - cursor;
                while (padding != 0) {
                    const std::size_t chunk =
                        static_cast<std::size_t>(std::min<std::uint64_t>(padding, zeros.size()));
                    write_all(handle.get(), std::span(zeros).first(chunk));
                    padding -= chunk;
                    cursor += chunk;
                }
                write_all(handle.get(), payloads[index].bytes);
                cursor += payloads[index].bytes.size();
            }
            if (cursor != total_bytes) {
                throw CheckpointContractError(
                    "checkpoint payload writer produced the wrong file size");
            }
            if (!FlushFileBuffers(handle.get())) {
                fail_windows("failed to flush checkpoint payload staging file", GetLastError());
            }
            handle.reset();
        } catch (...) {
            handle.reset();
            DeleteFileW(path.c_str());
            throw;
        }
    }

    void atomic_replace_durable(const std::filesystem::path& source,
                                const std::filesystem::path& destination) override {
        if (!MoveFileExW(source.c_str(), destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            fail_windows("failed to durably publish checkpoint file", GetLastError());
        }
    }

    bool remove_file(const std::filesystem::path& path) noexcept override {
        if (DeleteFileW(path.c_str())) { return true; }
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
};

struct DirectStorageState {
    std::mutex mutex;
    ComPtr<IDXGIFactory6> dxgi_factory;
    ComPtr<ID3D12Device> d3d_device;
    ComPtr<ID3D12Fence> fence;
    ComPtr<IDStorageFactory> factory;
    ComPtr<IDStorageQueue3> queue;
    ComPtr<IDStorageStatusArray> status;
    cudaExternalSemaphore_t cuda_fence = nullptr;
    cudaStream_t cuda_stream           = nullptr;
    std::uint64_t fence_value          = 0;

    ~DirectStorageState() {
        if (cuda_stream != nullptr) {
            cudaStreamSynchronize(cuda_stream);
            cudaStreamDestroy(cuda_stream);
        }
        if (cuda_fence != nullptr) { cudaDestroyExternalSemaphore(cuda_fence); }
        if (queue) { queue->Close(); }
    }
};

bool drain_d3d_fence(DirectStorageState& state, std::uint64_t value,
                     std::uint32_t timeout_ms) noexcept {
    std::uint64_t completed = state.fence->GetCompletedValue();
    if (completed == std::numeric_limits<std::uint64_t>::max()) { return false; }
    if (completed >= value) { return true; }
    UniqueHandle event(CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS));
    if (event.valid() && SUCCEEDED(state.fence->SetEventOnCompletion(value, event.get())) &&
        WaitForSingleObject(event.get(), timeout_ms) == WAIT_OBJECT_0) {
        completed = state.fence->GetCompletedValue();
        return completed != std::numeric_limits<std::uint64_t>::max() && completed >= value;
    }

    // Event setup failure must not release a buffer still targeted by DMA. Polling is only a
    // no-fail safety drain; device removal is terminal and guarantees the queue cannot keep
    // writing.
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        completed = state.fence->GetCompletedValue();
        if (completed == std::numeric_limits<std::uint64_t>::max()) { return false; }
        if (completed >= value) { return true; }
        if (GetTickCount64() >= deadline) { return false; }
        Sleep(1);
    }
}

std::string direct_storage_failure(DirectStorageState& state, HRESULT status) {
    HRESULT result           = status;
    const HANDLE error_event = state.queue->GetErrorEvent();
    if (error_event != nullptr && WaitForSingleObject(error_event, 0) == WAIT_OBJECT_0) {
        DSTORAGE_ERROR_RECORD record{};
        state.queue->RetrieveErrorRecord(&record);
        if (record.FailureCount != 0) { result = record.FirstFailure.HResult; }
    }
    std::ostringstream message;
    message << "DirectStorage checkpoint read failed (HRESULT=0x" << std::hex
            << static_cast<std::uint32_t>(result) << ')';
    return message.str();
}

class WindowsReadCompletion final : public ContinuationCheckpointReadCompletion {
public:
    WindowsReadCompletion(std::shared_ptr<DirectStorageState> state,
                          std::unique_lock<std::mutex> queue_lock, ComPtr<IDStorageFile> file,
                          std::uint64_t fence_value, std::uint32_t timeout_ms)
        : state_(std::move(state)), queue_lock_(std::move(queue_lock)), file_(std::move(file)),
          fence_value_(fence_value), timeout_ms_(timeout_ms) {}

    ~WindowsReadCompletion() override {
        if (!done_) {
            try {
                wait();
            } catch (...) {}
        }
    }

    void wait() override {
        if (done_) { return; }

        cudaExternalSemaphoreWaitParams parameters{};
        parameters.params.fence.value    = fence_value_;
        const cudaError_t enqueue_result = cudaWaitExternalSemaphoresAsync(
            &state_->cuda_fence, &parameters, 1, state_->cuda_stream);
        cudaError_t synchronize_result = enqueue_result;
        if (enqueue_result == cudaSuccess) {
            const ULONGLONG deadline = GetTickCount64() + timeout_ms_;
            for (;;) {
                synchronize_result = cudaStreamQuery(state_->cuda_stream);
                if (synchronize_result != cudaErrorNotReady || GetTickCount64() >= deadline) {
                    break;
                }
                Sleep(1);
            }
        }
        const std::uint64_t completed_value = state_->fence->GetCompletedValue();
        bool fence_completed                = true;
        if (synchronize_result != cudaSuccess ||
            completed_value == std::numeric_limits<std::uint64_t>::max() ||
            completed_value < fence_value_) {
            // This is a safety drain, not an operational fallback: the read still fails below if
            // CUDA/D3D interop failed, but its destination cannot be released while DMA is active.
            fence_completed = drain_d3d_fence(*state_, fence_value_, timeout_ms_);
        }
        // Returning without a completed D3D fence would release host buffers that DirectStorage
        // may still target. Device loss or a fence timeout is therefore fail-stop, not recoverable.
        if (!fence_completed) { std::terminate(); }

        const HRESULT status = state_->status->GetHResult(0);
        std::string failure;
        if (FAILED(status)) {
            failure = direct_storage_failure(*state_, status);
        } else if (synchronize_result != cudaSuccess) {
            failure = std::string("CUDA failed while awaiting the shared DirectStorage fence: ") +
                      cudaGetErrorString(synchronize_result);
        }
        done_ = true;
        queue_lock_.unlock();
        if (!failure.empty()) { throw CheckpointContractError(std::move(failure)); }
    }

private:
    std::shared_ptr<DirectStorageState> state_;
    std::unique_lock<std::mutex> queue_lock_;
    ComPtr<IDStorageFile> file_;
    std::uint64_t fence_value_ = 0;
    std::uint32_t timeout_ms_  = 0;
    bool done_                 = false;
};

class WindowsDirectStorageReadQueue final : public ContinuationCheckpointReadQueue {
public:
    WindowsDirectStorageReadQueue(std::filesystem::path root, std::uint32_t timeout_ms)
        : root_(std::filesystem::weakly_canonical(std::move(root))),
          state_(std::make_shared<DirectStorageState>()), timeout_ms_(timeout_ms) {
        int cuda_device = 0;
        require_cuda(cudaGetDevice(&cuda_device), "failed to query the active CUDA device");
        cudaDeviceProp cuda_properties{};
        require_cuda(cudaGetDeviceProperties(&cuda_properties, cuda_device),
                     "failed to inspect the active CUDA device");

        require_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(&state_->dxgi_factory)),
                        "failed to create the DXGI factory for DirectStorage");
        ComPtr<IDXGIAdapter1> matched_adapter;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT result = state_->dxgi_factory->EnumAdapters1(index, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND) { break; }
            require_hresult(result, "failed to enumerate DXGI adapters for DirectStorage");
            DXGI_ADAPTER_DESC1 description{};
            require_hresult(adapter->GetDesc1(&description),
                            "failed to inspect a DXGI adapter for DirectStorage");
            if (std::memcmp(&description.AdapterLuid, cuda_properties.luid, sizeof(LUID)) == 0) {
                matched_adapter = std::move(adapter);
                break;
            }
        }
        if (!matched_adapter) {
            throw CheckpointContractError("Windows DirectStorage is unavailable: no DXGI adapter "
                                          "matches the active CUDA device");
        }

        require_hresult(D3D12CreateDevice(matched_adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                          IID_PPV_ARGS(&state_->d3d_device)),
                        "failed to create the CUDA-matched D3D12 device");
        require_hresult(state_->d3d_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                                        IID_PPV_ARGS(&state_->fence)),
                        "failed to create the shared DirectStorage fence");

        UniqueHandle shared_fence;
        HANDLE raw_shared_fence = nullptr;
        require_hresult(state_->d3d_device->CreateSharedHandle(
                            state_->fence.Get(), nullptr, GENERIC_ALL, nullptr, &raw_shared_fence),
                        "failed to export the shared DirectStorage fence");
        shared_fence.reset(raw_shared_fence);
        cudaExternalSemaphoreHandleDesc semaphore_desc{};
        semaphore_desc.type                = cudaExternalSemaphoreHandleTypeD3D12Fence;
        semaphore_desc.handle.win32.handle = shared_fence.get();
        require_cuda(cudaImportExternalSemaphore(&state_->cuda_fence, &semaphore_desc),
                     "failed to import the shared DirectStorage fence into CUDA");
        require_cuda(cudaStreamCreateWithFlags(&state_->cuda_stream, cudaStreamNonBlocking),
                     "failed to create the DirectStorage completion stream");

        require_hresult(DStorageGetFactory(IID_PPV_ARGS(&state_->factory)),
                        "Windows DirectStorage 1.3 factory is unavailable");
        state_->factory->SetDebugFlags(DSTORAGE_DEBUG_SHOW_ERRORS);
        require_hresult(state_->factory->SetStagingBufferSize(DSTORAGE_STAGING_BUFFER_SIZE_32MB),
                        "failed to configure the DirectStorage staging buffer");

        DSTORAGE_QUEUE_DESC queue_desc{};
        queue_desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        queue_desc.Capacity   = DSTORAGE_MAX_QUEUE_CAPACITY;
        queue_desc.Priority   = DSTORAGE_PRIORITY_NORMAL;
        queue_desc.Name       = "NInferCheckpointQueue";
        queue_desc.Device     = state_->d3d_device.Get();
        require_hresult(state_->factory->CreateQueue(&queue_desc, IID_PPV_ARGS(&state_->queue)),
                        "Windows DirectStorage 1.3 queue is unavailable");
        require_hresult(state_->factory->CreateStatusArray(1, "NInferCheckpointStatus",
                                                           IID_PPV_ARGS(&state_->status)),
                        "failed to create the DirectStorage completion status");
    }

    bool available() const noexcept override { return true; }

    std::string_view backend_name() const noexcept override { return "directstorage-1.3"; }

    std::string_view unavailable_reason() const noexcept override { return {}; }

    std::unique_ptr<ContinuationCheckpointReadCompletion>
    submit(const std::filesystem::path& path,
           std::span<const ContinuationCheckpointReadRequest> requests) override {
        if (requests.empty() || requests.size() > DSTORAGE_MAX_QUEUE_CAPACITY - 2) {
            throw CheckpointContractError("DirectStorage checkpoint request batch is invalid");
        }

        const DWORD source_attributes = GetFileAttributesW(path.c_str());
        if (source_attributes == INVALID_FILE_ATTRIBUTES ||
            (source_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            throw CheckpointContractError(
                "DirectStorage checkpoint payload is not a regular non-reparse file");
        }
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(path);
        if (!contains(canonical)) {
            throw CheckpointContractError(
                "DirectStorage checkpoint read escaped its configured root");
        }

        std::unique_lock queue_lock(state_->mutex);
        ComPtr<IDStorageFile> file;
        require_hresult(state_->factory->OpenFile(canonical.c_str(), IID_PPV_ARGS(&file)),
                        "DirectStorage could not open the committed checkpoint payload");
        BY_HANDLE_FILE_INFORMATION information{};
        require_hresult(file->GetFileInformation(&information),
                        "DirectStorage could not inspect the committed checkpoint payload");
        const std::uint64_t file_bytes =
            (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
            information.nFileSizeLow;

        std::vector<DSTORAGE_REQUEST> direct_storage_requests;
        direct_storage_requests.reserve(requests.size());
        for (const ContinuationCheckpointReadRequest& source : requests) {
            if (source.destination.empty() ||
                source.destination.size() > std::numeric_limits<UINT32>::max() ||
                source.file_offset > file_bytes ||
                source.destination.size() > file_bytes - source.file_offset) {
                throw CheckpointContractError(
                    "DirectStorage checkpoint request exceeds the validated payload file");
            }
            const UINT32 bytes = static_cast<UINT32>(source.destination.size());
            DSTORAGE_REQUEST request{};
            request.Options.SourceType        = DSTORAGE_REQUEST_SOURCE_FILE;
            request.Options.DestinationType   = DSTORAGE_REQUEST_DESTINATION_MEMORY;
            request.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
            request.Source.File.Source        = file.Get();
            request.Source.File.Offset        = source.file_offset;
            request.Source.File.Size          = bytes;
            request.Destination.Memory.Buffer = source.destination.data();
            request.Destination.Memory.Size   = bytes;
            request.UncompressedSize          = bytes;
            request.Name                      = "NInferCheckpointPayload";
            direct_storage_requests.push_back(request);
        }

        if (state_->fence_value == std::numeric_limits<std::uint64_t>::max()) {
            throw CheckpointContractError("DirectStorage checkpoint fence value is exhausted");
        }
        const std::uint64_t fence_value = ++state_->fence_value;
        auto completion = std::make_unique<WindowsReadCompletion>(state_, std::move(queue_lock),
                                                                  file, fence_value, timeout_ms_);
        state_->queue->EnqueueRequests(direct_storage_requests.data(),
                                       static_cast<UINT>(direct_storage_requests.size()), nullptr,
                                       0, DSTORAGE_ENQUEUE_REQUEST_FLAG_NONE);
        state_->queue->EnqueueStatus(state_->status.Get(), 0);
        state_->queue->EnqueueSignal(state_->fence.Get(), fence_value);
        state_->queue->Submit();
        return completion;
    }

private:
    [[nodiscard]] static bool same_component(const std::filesystem::path& left,
                                             const std::filesystem::path& right) noexcept {
        return ::_wcsicmp(left.c_str(), right.c_str()) == 0;
    }

    [[nodiscard]] bool contains(const std::filesystem::path& path) const noexcept {
        auto root      = root_.begin();
        auto candidate = path.begin();
        for (; root != root_.end(); ++root, ++candidate) {
            if (candidate == path.end() || !same_component(*candidate, *root)) { return false; }
        }
        return candidate != path.end();
    }

    std::filesystem::path root_;
    std::shared_ptr<DirectStorageState> state_;
    std::uint32_t timeout_ms_ = 0;
};

} // namespace

std::shared_ptr<ContinuationCheckpointReadQueue>
make_direct_storage_checkpoint_read_queue(const std::filesystem::path& root,
                                          std::uint32_t timeout_ms) {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        throw CheckpointContractError("create DirectStorage checkpoint root: " + error.message());
    }
    return std::make_shared<WindowsDirectStorageReadQueue>(root, timeout_ms);
}

std::unique_ptr<DirectStorageCheckpointBackend>
make_direct_storage_checkpoint_backend(DirectStorageCheckpointConfig config) {
    auto file_system = std::make_shared<WindowsCheckpointFileSystem>();
    auto read_queue =
        make_direct_storage_checkpoint_read_queue(config.directory, config.io_timeout_ms);
    return std::make_unique<DirectStorageCheckpointBackend>(
        std::move(config), std::move(file_system), std::move(read_queue));
}

} // namespace ninfer::runtime::windows

#endif // _WIN32
