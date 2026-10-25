#include "core/io/file.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <utility>

namespace ninfer::io {
namespace {
HANDLE handle(FileView file) noexcept { return reinterpret_cast<HANDLE>(file.native_handle); }

[[noreturn]] void fail(const char* operation, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

struct Event {
    HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() { if (!value) fail("create file read event"); }
    ~Event() { CloseHandle(value); }
};

std::size_t read_once(FileView file, std::uint64_t offset, std::span<std::byte> destination,
                      Event& event) {
    OVERLAPPED operation{};
    operation.Offset = static_cast<DWORD>(offset);
    operation.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    // A synchronous helper may borrow a file already associated with an IOCP queue. The low
    // event bit prevents its private completion from entering that queue.
    operation.hEvent = reinterpret_cast<HANDLE>(reinterpret_cast<std::uintptr_t>(event.value) | 1U);
    if (!ResetEvent(event.value)) fail("reset file read event");
    DWORD received = 0;
    const BOOL started = ReadFile(handle(file), destination.data(),
                                  static_cast<DWORD>(destination.size()), &received, &operation);
    if (!started) {
        const auto error = GetLastError();
        if (error == ERROR_HANDLE_EOF) return 0;
        if (error != ERROR_IO_PENDING) fail("read file", error);
        if (!GetOverlappedResult(handle(file), &operation, &received, TRUE)) {
            const auto completion_error = GetLastError();
            if (completion_error == ERROR_HANDLE_EOF) return 0;
            fail("complete file read", completion_error);
        }
    }
    return received;
}
} // namespace

ReadOnlyFile::ReadOnlyFile(const std::filesystem::path& path, ReadMode mode) {
    if (mode != ReadMode::Buffered && mode != ReadMode::Direct)
        throw std::invalid_argument("invalid file read mode");
    const auto flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED |
                       (mode == ReadMode::Direct ? FILE_FLAG_NO_BUFFERING : 0);
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, flags, nullptr);
    if (file == INVALID_HANDLE_VALUE) fail("open immutable file");
    view_.native_handle = reinterpret_cast<std::intptr_t>(file);
    view_.mode = mode;
    try {
        FILE_STANDARD_INFO info{};
        if (GetFileType(file) != FILE_TYPE_DISK)
            fail("expected regular file", ERROR_INVALID_DATA);
        if (!GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof(info)))
            fail("inspect immutable file");
        if (info.Directory || info.EndOfFile.QuadPart < 0)
            fail("expected regular file", ERROR_INVALID_DATA);
        view_.bytes = static_cast<std::uint64_t>(info.EndOfFile.QuadPart);
        if (mode == ReadMode::Direct) {
            FILE_STORAGE_INFO storage{};
            FILE_ALIGNMENT_INFO alignment{};
            if (!GetFileInformationByHandleEx(file, FileStorageInfo, &storage, sizeof(storage)))
                fail("query direct file sector alignment");
            if (!GetFileInformationByHandleEx(file, FileAlignmentInfo, &alignment,
                                              sizeof(alignment)))
                fail("query direct file memory alignment");
            view_.offset_alignment = storage.LogicalBytesPerSector;
            view_.memory_alignment = std::max<std::size_t>(
                storage.PhysicalBytesPerSectorForAtomicity,
                static_cast<std::size_t>(alignment.AlignmentRequirement) + 1U);
            view_.memory_alignment = std::max(view_.memory_alignment, view_.offset_alignment);
            if (!std::has_single_bit(view_.memory_alignment) ||
                !std::has_single_bit(view_.offset_alignment))
                fail("unsupported direct file alignment", ERROR_NOT_SUPPORTED);
        }
    } catch (...) {
        CloseHandle(file);
        view_.native_handle = -1;
        throw;
    }
}

ReadOnlyFile::~ReadOnlyFile() {
    if (view_.native_handle != -1) CloseHandle(handle(view_));
}

ReadOnlyFile::ReadOnlyFile(ReadOnlyFile&& other) noexcept
    : view_(std::exchange(other.view_, {})) {}

ReadOnlyFile& ReadOnlyFile::operator=(ReadOnlyFile&& other) noexcept {
    if (this != &other) {
        if (view_.native_handle != -1) CloseHandle(handle(view_));
        view_ = std::exchange(other.view_, {});
    }
    return *this;
}

void ReadOnlyFile::read_exact(std::uint64_t offset, std::span<std::byte> destination) const {
    if (view_.mode != ReadMode::Buffered || view_.native_handle == -1)
        throw std::invalid_argument("read_exact requires a buffered file");
    if (offset > view_.bytes || destination.size() > view_.bytes - offset)
        fail("read exceeds file length", ERROR_HANDLE_EOF);
    if (destination.empty()) return;
    Event event;
    while (!destination.empty()) {
        const auto count = std::min<std::size_t>(destination.size(), 64U * 1024U * 1024U);
        const auto received = read_once(view_, offset, destination.first(count), event);
        if (!received) fail("unexpected EOF", ERROR_HANDLE_EOF);
        offset += received;
        destination = destination.subspan(received);
    }
}

std::size_t ReadOnlyFile::read_direct(std::uint64_t offset,
                                      std::span<std::byte> destination) const {
    validate_direct_read(view_, offset, destination);
    if (destination.empty()) return 0;
    Event event;
    std::size_t total = 0;
    const auto alignment = std::max(view_.memory_alignment, view_.offset_alignment);
    const auto maximum = std::size_t{MAXDWORD} / alignment * alignment;
    if (!maximum) fail("direct read alignment exceeds native request size", ERROR_NOT_SUPPORTED);
    while (!destination.empty()) {
        const auto count = std::min(destination.size(), maximum);
        const auto received = read_once(view_, offset, destination.first(count), event);
        total += received;
        if (received != count) break;
        offset += received;
        destination = destination.subspan(received);
    }
    return total;
}

} // namespace ninfer::io
