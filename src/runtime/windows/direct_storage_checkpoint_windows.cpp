#if defined(_WIN32)

#include "runtime/windows/direct_storage_checkpoint_backend.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <dstorage.h>
#include <windows.h>
#include <wrl/client.h>

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
    if (FAILED(result)) {
        fail_windows(operation, static_cast<std::uint32_t>(result));
    }
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
    UniqueHandle handle(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                                        FILE_FLAG_WRITE_THROUGH,
                                    nullptr));
    if (!handle.valid()) {
        fail_windows("failed to create private checkpoint staging file", GetLastError());
    }
    return handle;
}

void write_all(HANDLE handle, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size(), kWindowsIoChunkBytes));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data(), chunk, &written, nullptr) || written != chunk) {
            fail_windows("failed to write checkpoint staging bytes", GetLastError());
        }
        bytes = bytes.subspan(written);
    }
}

class WindowsDirectoryLock final : public CheckpointDirectoryLock {
public:
    explicit WindowsDirectoryLock(const std::filesystem::path& directory) {
        const std::filesystem::path lock_path = directory / ".checkpoint.lock";
        handle_.reset(CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_HIDDEN, nullptr));
        if (!handle_.valid()) {
            fail_windows("failed to open checkpoint directory lock", GetLastError());
        }
        if (!LockFileEx(handle_.get(), LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlapped_)) {
            fail_windows("failed to acquire checkpoint directory lock", GetLastError());
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
        if (error || !std::filesystem::is_directory(path, error)) {
            throw CheckpointContractError("failed to create checkpoint directory: " +
                                          (error ? error.message() : std::string("not a directory")));
        }
    }

    std::unique_ptr<CheckpointDirectoryLock>
    lock_directory(const std::filesystem::path& path) override {
        return std::make_unique<WindowsDirectoryLock>(path);
    }

    bool file_exists(const std::filesystem::path& path) override {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { return false; }
        fail_windows("failed to inspect checkpoint file", error);
    }

    std::uint64_t file_size(const std::filesystem::path& path) override {
        UniqueHandle handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!handle.valid()) {
            fail_windows("failed to open checkpoint file for size validation", GetLastError());
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle.get(), &size) || size.QuadPart < 0) {
            fail_windows("failed to read checkpoint file size", GetLastError());
        }
        return static_cast<std::uint64_t>(size.QuadPart);
    }

    void read_exact(const std::filesystem::path& path, std::span<std::byte> bytes) override {
        UniqueHandle handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                                                           FILE_FLAG_SEQUENTIAL_SCAN,
                                        nullptr));
        if (!handle.valid()) {
            fail_windows("failed to open committed checkpoint manifest", GetLastError());
        }
        while (!bytes.empty()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                bytes.size(), std::numeric_limits<DWORD>::max()));
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

    void write_payloads_durable(
        const std::filesystem::path& path, std::span<const CheckpointPayload> payloads,
        std::span<const std::uint64_t> payload_offsets, std::uint64_t total_bytes) override {
        if (payloads.size() != payload_offsets.size()) {
            throw CheckpointContractError("checkpoint payload writer received inconsistent offsets");
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
                    const std::size_t chunk = static_cast<std::size_t>(
                        std::min<std::uint64_t>(padding, zeros.size()));
                    write_all(handle.get(), std::span(zeros).first(chunk));
                    padding -= chunk;
                    cursor += chunk;
                }
                write_all(handle.get(), payloads[index].bytes);
                cursor += payloads[index].bytes.size();
            }
            if (cursor != total_bytes) {
                throw CheckpointContractError("checkpoint payload writer produced the wrong file size");
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

    void remove_file(const std::filesystem::path& path) noexcept override {
        if (DeleteFileW(path.c_str())) { return; }
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { return; }
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
    cudaStream_t cuda_stream            = nullptr;
    std::uint64_t fence_value           = 0;

    ~DirectStorageState() {
        if (cuda_stream != nullptr) {
            cudaStreamSynchronize(cuda_stream);
            cudaStreamDestroy(cuda_stream);
        }
        if (cuda_fence != nullptr) { cudaDestroyExternalSemaphore(cuda_fence); }
        if (queue) { queue->Close(); }
    }
};

bool drain_d3d_fence(DirectStorageState& state, std::uint64_t value) noexcept {
    std::uint64_t completed = state.fence->GetCompletedValue();
    if (completed == std::numeric_limits<std::uint64_t>::max()) { return false; }
    if (completed >= value) { return true; }
    UniqueHandle event(CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS));
    if (event.valid() && SUCCEEDED(state.fence->SetEventOnCompletion(value, event.get())) &&
        WaitForSingleObject(event.get(), INFINITE) == WAIT_OBJECT_0) {
        completed = state.fence->GetCompletedValue();
        return completed != std::numeric_limits<std::uint64_t>::max() && completed >= value;
    }

    // Event setup failure must not release a buffer still targeted by DMA. Polling is only a
    // no-fail safety drain; device removal is terminal and guarantees the queue cannot keep writing.
    for (;;) {
        completed = state.fence->GetCompletedValue();
        if (completed == std::numeric_limits<std::uint64_t>::max()) { return false; }
        if (completed >= value) { return true; }
        Sleep(1);
    }
}

std::string direct_storage_failure(DirectStorageState& state, HRESULT status) {
    HRESULT result = status;
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

class WindowsReadCompletion final : public CheckpointReadCompletion {
public:
    WindowsReadCompletion(std::shared_ptr<DirectStorageState> state,
                          std::unique_lock<std::mutex> queue_lock,
                          ComPtr<IDStorageFile> file, std::uint64_t fence_value)
        : state_(std::move(state)), queue_lock_(std::move(queue_lock)), file_(std::move(file)),
          fence_value_(fence_value) {}

    ~WindowsReadCompletion() override {
        if (!done_) {
            try {
                wait();
            } catch (...) {
            }
        }
    }

    void wait() override {
        if (done_) { return; }

        cudaExternalSemaphoreWaitParams parameters{};
        parameters.params.fence.value = fence_value_;
        const cudaError_t enqueue_result = cudaWaitExternalSemaphoresAsync(
            &state_->cuda_fence, &parameters, 1, state_->cuda_stream);
        cudaError_t synchronize_result = enqueue_result;
        if (enqueue_result == cudaSuccess) {
            synchronize_result = cudaStreamSynchronize(state_->cuda_stream);
        }
        const std::uint64_t completed_value = state_->fence->GetCompletedValue();
        bool fence_completed = true;
        if (synchronize_result != cudaSuccess ||
            completed_value == std::numeric_limits<std::uint64_t>::max() ||
            completed_value < fence_value_) {
            // This is a safety drain, not an operational fallback: the read still fails below if
            // CUDA/D3D interop failed, but its destination cannot be released while DMA is active.
            fence_completed = drain_d3d_fence(*state_, fence_value_);
        }

        const HRESULT status = fence_completed ? state_->status->GetHResult(0) :
                                                 DXGI_ERROR_DEVICE_REMOVED;
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
    bool done_                  = false;
};

class WindowsDirectStorageReadQueue final : public CheckpointReadQueue {
public:
    WindowsDirectStorageReadQueue() : state_(std::make_shared<DirectStorageState>()) {
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
            throw CheckpointContractError(
                "Windows DirectStorage is unavailable: no DXGI adapter matches the active CUDA device");
        }

        require_hresult(D3D12CreateDevice(matched_adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                          IID_PPV_ARGS(&state_->d3d_device)),
                        "failed to create the CUDA-matched D3D12 device");
        require_hresult(state_->d3d_device->CreateFence(
                            0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&state_->fence)),
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
        require_hresult(state_->factory->CreateStatusArray(
                            1, "NInferCheckpointStatus", IID_PPV_ARGS(&state_->status)),
                        "failed to create the DirectStorage completion status");
    }

    bool available() const noexcept override { return true; }
    std::string_view unavailable_reason() const noexcept override { return {}; }

    std::unique_ptr<CheckpointReadCompletion>
    submit(const std::filesystem::path& path,
           std::span<const CheckpointReadRequest> requests) override {
        if (requests.empty() || requests.size() > DSTORAGE_MAX_QUEUE_CAPACITY - 2) {
            throw CheckpointContractError("DirectStorage checkpoint request batch is invalid");
        }

        std::unique_lock queue_lock(state_->mutex);
        ComPtr<IDStorageFile> file;
        require_hresult(state_->factory->OpenFile(path.c_str(), IID_PPV_ARGS(&file)),
                        "DirectStorage could not open the committed checkpoint payload");
        BY_HANDLE_FILE_INFORMATION information{};
        require_hresult(file->GetFileInformation(&information),
                        "DirectStorage could not inspect the committed checkpoint payload");
        const std::uint64_t file_bytes =
            (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
            information.nFileSizeLow;

        std::vector<DSTORAGE_REQUEST> direct_storage_requests;
        direct_storage_requests.reserve(requests.size());
        for (const CheckpointReadRequest& source : requests) {
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
        auto completion = std::make_unique<WindowsReadCompletion>(
            state_, std::move(queue_lock), file, fence_value);
        state_->queue->EnqueueRequests(
            direct_storage_requests.data(), static_cast<UINT>(direct_storage_requests.size()),
            nullptr, 0, DSTORAGE_ENQUEUE_REQUEST_FLAG_NONE);
        state_->queue->EnqueueStatus(state_->status.Get(), 0);
        state_->queue->EnqueueSignal(state_->fence.Get(), fence_value);
        state_->queue->Submit();
        return completion;
    }

private:
    std::shared_ptr<DirectStorageState> state_;
};

} // namespace

std::unique_ptr<DirectStorageCheckpointBackend>
make_direct_storage_checkpoint_backend(DirectStorageCheckpointConfig config) {
    auto file_system = std::make_shared<WindowsCheckpointFileSystem>();
    auto read_queue  = std::make_shared<WindowsDirectStorageReadQueue>();
    return std::make_unique<DirectStorageCheckpointBackend>(
        std::move(config), std::move(file_system), std::move(read_queue));
}

} // namespace ninfer::runtime::windows

#endif // _WIN32
