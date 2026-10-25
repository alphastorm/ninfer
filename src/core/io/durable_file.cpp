#include "core/io/durable_file.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ninfer::io {
namespace {
[[noreturn]] void fail_errno(const char* operation, int error = errno) {
    throw std::system_error(error, std::generic_category(), operation);
}

std::filesystem::path checked_path(const std::filesystem::path& path) {
    if (path.empty()) throw std::invalid_argument("empty durable path");
    for (const auto& component : path.relative_path())
        if (component == "..")
            throw std::invalid_argument("durable path contains traversal components");
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
#ifdef _WIN32
    // The native store is local and same-volume. Device/UNC namespaces and alternate streams
    // are not part of its path contract.
    const auto root = absolute.root_name().native();
    if (root.size() != 2 || root[1] != L':')
        throw std::invalid_argument("durable path requires a local drive");
    for (const auto& component : absolute.relative_path()) {
        const auto text = component.native();
        if (text.find(L':') != std::wstring::npos || text.find(L'\0') != std::wstring::npos ||
            (!text.empty() && (text.back() == L'.' || text.back() == L' ')))
            throw std::invalid_argument("durable path has an unsupported component");
    }
#else
    if (absolute.native().find('\0') != std::string::npos)
        throw std::invalid_argument("durable path contains a NUL");
#endif
    return absolute;
}

#ifdef _WIN32
[[noreturn]] void fail_windows(const char* operation, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle(const Handle&) = delete;
};

void check_directory(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)))
        fail_windows("inspect durable directory");
    if (!(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        fail_windows("durable directory is not a real directory", ERROR_CANT_ACCESS_FILE);
}

struct Parent {
    std::filesystem::path path;
    std::vector<Handle> pins;

    Parent(const std::filesystem::path& directory, bool create) {
        path = directory.root_path();
        pin();
        for (const auto& component : directory.relative_path()) {
            if (component.empty()) continue;
            path /= component;
            if (create && !CreateDirectoryW(path.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS)
                fail_windows("create durable directory");
            pin();
        }
    }

    void pin() {
        // Deny delete sharing while resolving children, preventing a checked parent from
        // being renamed or replaced by a junction before the final operation.
        const HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                           nullptr);
        if (handle == INVALID_HANDLE_VALUE) fail_windows("open durable directory");
        pins.emplace_back(handle);
        check_directory(handle);
    }
};

std::FILE* open_stream(const std::filesystem::path& input, bool write) {
    const auto path = checked_path(input);
    Parent parent(path.parent_path(), false);
    Handle file(CreateFileW(path.c_str(), write ? GENERIC_WRITE : GENERIC_READ,
                            write ? 0 : FILE_SHARE_READ, nullptr, write ? CREATE_NEW : OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) fail_windows("open durable file");
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (GetFileType(file.value) != FILE_TYPE_DISK)
        fail_windows("durable file is not regular", ERROR_INVALID_DATA);
    if (!GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &info, sizeof(info)))
        fail_windows("inspect durable file");
    if (info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        fail_windows("durable file is not a regular file", ERROR_CANT_ACCESS_FILE);
    const int descriptor = _open_osfhandle(reinterpret_cast<std::intptr_t>(file.value),
                                          _O_BINARY | (write ? _O_WRONLY : _O_RDONLY));
    if (descriptor < 0) fail_errno("attach durable file descriptor");
    file.value = INVALID_HANDLE_VALUE; // The CRT owns the handle from this point.
    auto* stream = _fdopen(descriptor, write ? "wb" : "rb");
    if (!stream) {
        const auto error = errno;
        _close(descriptor);
        fail_errno("open durable stream", error);
    }
    return stream;
}

void reject_reparse(const std::filesystem::path& path, bool allow_missing) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (allow_missing && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) return;
        fail_windows("inspect durable replacement entry", error);
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        fail_windows("durable replacement entry is a reparse point", ERROR_CANT_ACCESS_FILE);
}
#else
struct Descriptor {
    int value = -1;
    explicit Descriptor(int descriptor) : value(descriptor) {}
    ~Descriptor() { if (value >= 0) ::close(value); }
    Descriptor(Descriptor&& other) noexcept : value(std::exchange(other.value, -1)) {}
    Descriptor(const Descriptor&) = delete;
};

struct Parent {
    Descriptor directory{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};

    Parent(const std::filesystem::path& path, bool create) {
        if (directory.value < 0) fail_errno("open durable filesystem root");
        for (const auto& component : path.relative_path()) {
            if (component.empty()) continue;
            if (create && ::mkdirat(directory.value, component.c_str(), 0700) && errno != EEXIST)
                fail_errno("create durable directory");
            const int next = ::openat(directory.value, component.c_str(),
                                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0) fail_errno("open durable directory");
            ::close(directory.value);
            directory.value = next;
        }
    }
};

std::FILE* open_stream(const std::filesystem::path& input, bool write) {
    const auto path = checked_path(input);
    Parent parent(path.parent_path(), false);
    Descriptor file(::openat(parent.directory.value, path.filename().c_str(),
                              O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                                  (write ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY), 0600));
    if (file.value < 0) fail_errno("open durable file");
    struct stat info{};
    if (::fstat(file.value, &info)) fail_errno("inspect durable file");
    if (!S_ISREG(info.st_mode)) fail_errno("durable file is not regular", EINVAL);
    auto* stream = ::fdopen(file.value, write ? "wb" : "rb");
    if (!stream) fail_errno("open durable stream");
    file.value = -1;
    return stream;
}

void reject_symlink(int parent, const std::filesystem::path& filename, bool allow_missing) {
    struct stat status{};
    if (::fstatat(parent, filename.c_str(), &status, AT_SYMLINK_NOFOLLOW)) {
        if (allow_missing && errno == ENOENT) return;
        fail_errno("inspect durable replacement entry");
    }
    if (S_ISLNK(status.st_mode)) fail_errno("durable replacement entry is a symlink", ELOOP);
}
#endif
} // namespace

std::FILE* open_binary_write(const std::filesystem::path& path) { return open_stream(path, true); }
std::FILE* open_binary_read(const std::filesystem::path& path) { return open_stream(path, false); }

void create_directories(const std::filesystem::path& path) {
    Parent directory(checked_path(path), true);
}

void read_exact(std::FILE* file, std::span<std::byte> bytes) {
    if (!file) throw std::invalid_argument("null durable read stream");
    while (!bytes.empty()) {
        const auto count = std::fread(bytes.data(), 1, bytes.size(), file);
        if (!count) {
            if (std::ferror(file)) fail_errno("read durable stream");
            throw std::runtime_error("durable stream ended before its declared size");
        }
        bytes = bytes.subspan(count);
    }
}

void write_exact(std::FILE* file, std::span<const std::byte> bytes) {
    if (!file) throw std::invalid_argument("null durable write stream");
    while (!bytes.empty()) {
        const auto count = std::fwrite(bytes.data(), 1, bytes.size(), file);
        if (!count) fail_errno("write durable stream");
        bytes = bytes.subspan(count);
    }
}

void flush_file(std::FILE* file) {
    if (!file) throw std::invalid_argument("null durable flush stream");
    if (std::fflush(file)) fail_errno("flush durable stream");
#ifdef _WIN32
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)));
    if (handle == INVALID_HANDLE_VALUE) fail_errno("get durable stream handle");
    if (!FlushFileBuffers(handle)) fail_windows("sync durable file");
#else
    int result;
    do { result = ::fsync(::fileno(file)); } while (result < 0 && errno == EINTR);
    if (result) fail_errno("sync durable file");
#endif
}

void sync_directory(const std::filesystem::path& path) {
#ifdef _WIN32
    (void)path; // Explicit ntfs-write-through policy; Windows has no POSIX directory fsync.
#else
    Parent directory(checked_path(path), false);
    int result;
    do { result = ::fsync(directory.directory.value); } while (result < 0 && errno == EINTR);
    if (result) fail_errno("sync durable directory");
#endif
}

std::string_view directory_sync_mode() noexcept {
#ifdef _WIN32
    return "ntfs-write-through";
#else
    return "fsync";
#endif
}

void replace_path(const std::filesystem::path& source, const std::filesystem::path& destination) {
    const auto from = checked_path(source), to = checked_path(destination);
    Parent from_parent(from.parent_path(), false), to_parent(to.parent_path(), false);
#ifdef _WIN32
    if (from.root_name() != to.root_name())
        fail_windows("durable replacement crosses volumes", ERROR_NOT_SAME_DEVICE);
    reject_reparse(from, false);
    reject_reparse(to, true);
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fail_windows("replace durable path");
#else
    reject_symlink(from_parent.directory.value, from.filename(), false);
    reject_symlink(to_parent.directory.value, to.filename(), true);
    if (::renameat(from_parent.directory.value, from.filename().c_str(),
                    to_parent.directory.value, to.filename().c_str()))
        fail_errno("replace durable path");
#endif
}

} // namespace ninfer::io
