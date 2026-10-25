#include "core/io/read_queue.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ninfer::io {
namespace {
[[noreturn]] void fail(const char* operation, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}
constexpr ULONG_PTR wake_key = 1;
} // namespace

struct DirectReadQueue::Impl {
    struct Slot {
        OVERLAPPED operation{};
        DirectRead request{};
        bool busy = false;
    };
    struct Packet {
        OVERLAPPED* operation;
        DWORD bytes;
        DWORD error;
    };

    HANDLE port = nullptr;
    bool closed = false;
    bool signalled = false;
    std::uint32_t capacity = 0;
    std::size_t pending = 0;
    std::vector<FileView> files;
    std::vector<HANDLE> handles;
    std::vector<MemorySpan> buffers;
    std::vector<Slot> slots;
    std::vector<std::uint32_t> prepared;
    std::vector<Packet> immediate;
    std::optional<Packet> ready;

    ~Impl() {
        for (const auto file : handles) CloseHandle(file);
        if (port) CloseHandle(port);
    }

    std::optional<Packet> dequeue(DWORD timeout) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* operation = nullptr;
        const BOOL ok = GetQueuedCompletionStatus(port, &bytes, &key, &operation, timeout);
        const auto error = ok ? ERROR_SUCCESS : GetLastError();
        if (operation) return Packet{operation, bytes, error == ERROR_HANDLE_EOF ? 0 : error};
        if (ok && key == wake_key) {
            signalled = true;
            return {};
        }
        if (!ok && error == WAIT_TIMEOUT) return {};
        fail("dequeue direct read completion", error);
    }

    ReadCompletion finish(Packet packet) {
        // OVERLAPPED is the first member of the stable, standard-layout slot.
        auto* slot = reinterpret_cast<Slot*>(packet.operation);
        if (slot < slots.data() || slot >= slots.data() + slots.size() || !slot->busy)
            throw std::logic_error("unmatched direct read completion");
        const auto index = static_cast<std::uint32_t>(slot - slots.data());
        const ReadCompletion result{slot->request.tag, index, packet.error ? 0U : packet.bytes,
                                    static_cast<int>(packet.error)};
        slot->busy = false;
        --pending;
        return result;
    }
};

DirectReadQueue::DirectReadQueue(std::span<const FileView> files,
                                 std::span<const MemorySpan> buffers, std::uint32_t capacity)
    : impl_(std::make_unique<Impl>()) {
    if (!capacity || capacity > buffers.size() || files.empty() ||
        files.size() > std::numeric_limits<int>::max() || buffers.size() > 65535)
        throw std::invalid_argument("invalid direct read queue capacity");
    auto& p = *impl_;
    p.capacity = capacity;
    p.files.assign(files.begin(), files.end());
    p.buffers.assign(buffers.begin(), buffers.end());
    p.slots.resize(buffers.size());
    p.prepared.reserve(capacity);
    p.immediate.reserve(capacity);
    p.handles.reserve(files.size());
    for (const auto file : files)
        if (file.mode != ReadMode::Direct || file.native_handle == -1)
            throw std::invalid_argument("read queue requires direct files");
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        const auto buffer = buffers[i];
        validate_memory(buffer);
        if (buffer.device != Device::cpu() || !buffer.bytes || buffer.bytes > MAXDWORD)
            throw std::invalid_argument("read queue requires nonempty Host buffers");
        for (const auto file : files)
            validate_direct_read(file, 0, {static_cast<std::byte*>(buffer.data), buffer.bytes});
        const auto begin = reinterpret_cast<std::uintptr_t>(buffer.data);
        for (std::size_t j = 0; j < i; ++j) {
            const auto other = reinterpret_cast<std::uintptr_t>(buffers[j].data);
            if (begin < other + buffers[j].bytes && other < begin + buffer.bytes)
                throw std::invalid_argument("read queue buffers overlap");
        }
    }
    p.port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!p.port) fail("create direct read completion port");
    for (const auto file : files) {
        // An IOCP association cannot be removed from a file object. ReOpenFile creates an
        // independent object without resolving a path, so multiple queues can borrow one file.
        const HANDLE owned = ReOpenFile(reinterpret_cast<HANDLE>(file.native_handle), GENERIC_READ,
                                         FILE_SHARE_READ, FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING);
        if (owned == INVALID_HANDLE_VALUE) fail("reopen direct read file");
        p.handles.push_back(owned);
        if (!CreateIoCompletionPort(owned, p.port, 0, 0)) fail("associate direct read file");
        if (!SetFileCompletionNotificationModes(owned, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS))
            fail("configure direct read completion notifications");
    }
}

DirectReadQueue::~DirectReadQueue() {
    try { close(); drain(); }
    catch (...) { std::terminate(); }
}

ReadAdmission DirectReadQueue::try_enqueue(DirectRead request) {
    auto& p = *impl_;
    if (p.closed) return ReadAdmission::Closed;
    if (p.pending == p.capacity) return ReadAdmission::Full;
    if (request.file_index >= p.files.size() || request.buffer_index >= p.buffers.size() ||
        !request.bytes)
        throw std::invalid_argument("invalid direct read request");
    auto& slot = p.slots[request.buffer_index];
    const auto buffer = p.buffers[request.buffer_index];
    if (slot.busy || request.bytes > buffer.bytes)
        throw std::invalid_argument("direct read buffer is busy or too small");
    validate_direct_read(p.files[request.file_index], request.file_offset,
                         {static_cast<std::byte*>(buffer.data), request.bytes});
    slot.operation = {};
    slot.operation.Offset = static_cast<DWORD>(request.file_offset);
    slot.operation.OffsetHigh = static_cast<DWORD>(request.file_offset >> 32U);
    slot.request = request;
    slot.busy = true;
    p.prepared.push_back(request.buffer_index);
    ++p.pending;
    return ReadAdmission::Accepted;
}

void DirectReadQueue::submit() {
    auto& p = *impl_;
    for (const auto index : p.prepared) {
        auto& slot = p.slots[index];
        const auto& request = slot.request;
        DWORD bytes = 0;
        const BOOL started = ReadFile(p.handles[request.file_index], p.buffers[index].data,
                                       request.bytes, &bytes, &slot.operation);
        const auto error = started ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_IO_PENDING) continue;
        // Inline success is explicitly excluded from IOCP notifications. A failed submission
        // still belongs to the accepted request and must return exactly one completion.
        p.immediate.push_back({&slot.operation, bytes, error == ERROR_HANDLE_EOF ? 0 : error});
    }
    p.prepared.clear();
}

std::size_t DirectReadQueue::poll(std::span<ReadCompletion> output) {
    auto& p = *impl_;
    std::size_t count = 0;
    while (count < output.size()) {
        if (!p.immediate.empty()) {
            output[count++] = p.finish(p.immediate.back());
            p.immediate.pop_back();
        } else if (p.ready) {
            output[count++] = p.finish(*p.ready);
            p.ready.reset();
        } else {
            auto packet = p.dequeue(0);
            if (!packet) break;
            output[count++] = p.finish(*packet);
        }
    }
    return count;
}

ReadWake DirectReadQueue::wait() {
    auto& p = *impl_;
    if (std::exchange(p.signalled, false)) return ReadWake::Signalled;
    if (p.ready || !p.immediate.empty()) return ReadWake::Completion;
    p.ready = p.dequeue(INFINITE);
    if (std::exchange(p.signalled, false)) return ReadWake::Signalled;
    return ReadWake::Completion;
}

void DirectReadQueue::wake() {
    if (!PostQueuedCompletionStatus(impl_->port, 0, wake_key, nullptr)) fail("wake direct read queue");
}

void DirectReadQueue::close() { impl_->closed = true; submit(); }

void DirectReadQueue::drain() {
    submit();
    std::array<ReadCompletion, 128> completions;
    while (impl_->pending)
        if (!poll(completions)) (void)wait();
}

std::size_t DirectReadQueue::outstanding() const noexcept { return impl_->pending; }

} // namespace ninfer::io
