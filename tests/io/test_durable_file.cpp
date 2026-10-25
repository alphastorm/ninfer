#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include "core/io/durable_file.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

namespace {
enum class Mutation { None, Reparse, FileIndex, Volume };
struct PublicationTrace {
    bool active = false, mutated = false, delete_denied = false;
    Mutation mutation = Mutation::None;
    std::filesystem::path directory, redirect;
    BY_HANDLE_FILE_INFORMATION identity{};
    unsigned file_flushes = 0, parent_flushes = 0;
} publication_trace;

template <class Function> Function kernel_function(const char* name) {
    const auto address = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
    if (!address) throw std::runtime_error("missing native durable test API");
    return reinterpret_cast<Function>(address);
}

void install_junction(const std::filesystem::path& directory,
                      const std::filesystem::path& destination) {
    struct MountPoint {
        DWORD tag;
        WORD bytes, reserved, substitute_offset, substitute_length, print_offset, print_length;
        wchar_t names[1];
    };
    const auto substitute = L"\\??\\" + destination.native();
    const auto substitute_bytes = substitute.size() * sizeof(wchar_t);
    const auto print_bytes = destination.native().size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(offsetof(MountPoint, names) + substitute_bytes + print_bytes +
                                  2 * sizeof(wchar_t));
    auto* point = reinterpret_cast<MountPoint*>(buffer.data());
    point->tag = IO_REPARSE_TAG_MOUNT_POINT;
    point->bytes = static_cast<WORD>(buffer.size() - 8);
    point->substitute_length = static_cast<WORD>(substitute_bytes);
    point->print_offset = static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
    point->print_length = static_cast<WORD>(print_bytes);
    std::memcpy(point->names, substitute.c_str(), substitute_bytes);
    std::memcpy(reinterpret_cast<std::byte*>(point->names) + point->print_offset,
                destination.c_str(), print_bytes);
    const HANDLE handle = CreateFileW(directory.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(), "open swap-test directory");
    DWORD returned = 0;
    const auto result = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    const auto error = GetLastError();
    CloseHandle(handle);
    if (!result) throw std::system_error(error, std::system_category(), "install swap-test junction");
}

BOOL WINAPI inspect_directory(HANDLE handle, BY_HANDLE_FILE_INFORMATION* info) {
    static const auto call =
        kernel_function<decltype(&GetFileInformationByHandle)>("GetFileInformationByHandle");
    const auto result = call(handle, info);
    auto& trace = publication_trace;
    if (result && trace.active && trace.file_flushes &&
        info->dwVolumeSerialNumber == trace.identity.dwVolumeSerialNumber &&
        info->nFileIndexHigh == trace.identity.nFileIndexHigh &&
        info->nFileIndexLow == trace.identity.nFileIndexLow) {
        if (trace.mutation == Mutation::FileIndex) {
            info->nFileIndexLow ^= 1;
            trace.mutated = true;
        } else if (trace.mutation == Mutation::Volume) {
            info->dwVolumeSerialNumber ^= 1;
            trace.mutated = true;
        }
    }
    return result;
}

BOOL WINAPI publication_flush(HANDLE handle) {
    static const auto call = kernel_function<decltype(&FlushFileBuffers)>("FlushFileBuffers");
    static const auto inspect =
        kernel_function<decltype(&GetFileInformationByHandle)>("GetFileInformationByHandle");
    const auto result = call(handle);
    auto& trace = publication_trace;
    if (result && trace.active) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!inspect(handle, &info)) throw std::runtime_error("inspect publication flush failed");
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (trace.file_flushes != 1)
                throw std::runtime_error("parent flush preceded staged-file flush");
            ++trace.parent_flushes;
        } else {
            ++trace.file_flushes;
            const HANDLE forbidden = CreateFileW(trace.directory.c_str(), DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            trace.delete_denied =
                forbidden == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION;
            if (forbidden != INVALID_HANDLE_VALUE) CloseHandle(forbidden);
            if (trace.mutation == Mutation::Reparse) {
                install_junction(trace.directory, trace.redirect);
                trace.mutated = true;
            }
        }
    }
    return result;
}

struct PublicationScope {
    explicit PublicationScope(const std::filesystem::path& directory,
                              Mutation mutation = Mutation::None,
                              const std::filesystem::path& redirect = {}) {
        publication_trace = {};
        const HANDLE handle = CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("open publication trace directory failed");
        const auto result = inspect_directory(handle, &publication_trace.identity);
        CloseHandle(handle);
        if (!result) throw std::runtime_error("capture publication trace identity failed");
        publication_trace.directory = directory;
        publication_trace.redirect = redirect;
        publication_trace.mutation = mutation;
        publication_trace.active = true;
    }
    ~PublicationScope() { publication_trace.active = false; }
};
}

// Follow the native artifact-test IAT convention: only this executable interposes.
// Every flush/query reaches the real OS; identity faults alter only the returned test data.
extern "C" {
BOOL (WINAPI* __imp_GetFileInformationByHandle)(HANDLE, BY_HANDLE_FILE_INFORMATION*) =
    &inspect_directory;
BOOL (WINAPI* __imp_FlushFileBuffers)(HANDLE) = &publication_flush;
}
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
namespace {
bool trace_sync = false, interrupt_sync = false, wrong_sync_target = false;
std::size_t successful_syncs = 0;
std::array<std::uint64_t, 2> synced_inodes{};
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int descriptor) {
    if (trace_sync && interrupt_sync) {
        interrupt_sync = false;
        errno = EINTR;
        return -1;
    }
    const auto result = __real_fsync(descriptor);
    if (trace_sync && !result) {
        struct stat status{};
        if (::fstat(descriptor, &status) || !S_ISDIR(status.st_mode) ||
            successful_syncs >= synced_inodes.size())
            wrong_sync_target = true;
        else synced_inodes[successful_syncs] = status.st_ino;
        ++successful_syncs;
    }
    return result;
}
#endif

namespace {
namespace io = ninfer::io;
using Stream = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <class F> void rejects(F operation) {
    try { operation(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("unsafe durable operation was accepted");
}
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        std::random_device random;
        for (;;) {
            root = std::filesystem::temp_directory_path() /
                   ("ninfer-durable-" + std::to_string(random()));
            if (std::filesystem::create_directory(root)) break;
        }
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};
void write(const std::filesystem::path& path, std::byte value) {
    Stream stream(io::open_binary_write(path), &std::fclose);
    const std::array bytes{value, value, value};
    io::write_exact(stream.get(), bytes);
    io::flush_file(stream.get());
    require(std::fclose(stream.release()) == 0, "durable close failed");
}

void publish(const std::filesystem::path& from, const std::filesystem::path& to, const char* phase) {
    try { io::replace_path(from, to); }
    catch (const std::system_error& error) { throw std::system_error(error.code(), phase); }
}

void directory_publication(const std::filesystem::path& root) {
    const auto generations = root / "generations", tombstones = root / ".tombstones";
    const auto staging = generations / ".staging-generation", published = generations / "generation";
    io::create_directories(staging / "nested");
    io::create_directories(tombstones);
    write(staging / "manifest", std::byte{31});
    write(staging / "nested" / "payload", std::byte{37});
    io::sync_directory(staging);
    publish(staging, published, "same-parent directory publication");
    require(!std::filesystem::exists(staging) && std::filesystem::is_directory(published),
            "generation directory was not published");
    const auto tombstone = tombstones / "generation";
    publish(published, tombstone, "cross-parent directory tombstone");
    require(!std::filesystem::exists(published) && std::filesystem::is_directory(tombstone),
            "generation directory was not moved to its tombstone");
    {
        Stream manifest(io::open_binary_read(tombstone / "manifest"), &std::fclose);
        Stream payload(io::open_binary_read(tombstone / "nested" / "payload"), &std::fclose);
        std::array<std::byte, 3> bytes{};
        io::read_exact(manifest.get(), bytes);
        require(bytes == std::array{std::byte{31}, std::byte{31}, std::byte{31}},
                "directory publication changed its manifest");
        io::read_exact(payload.get(), bytes);
        require(bytes == std::array{std::byte{37}, std::byte{37}, std::byte{37}},
                "directory tombstone changed its nested payload");
    }
    require(io::remove_tree(tombstone), "generation tombstone cleanup failed");
}

#ifdef _WIN32
void native_publication_guards(const std::filesystem::path& root,
                               const std::filesystem::path& outside) {
    const auto parent = root / "swap-parent", staging = root / "swap-staging";
    io::create_directories(parent);
    write(staging, std::byte{21});
    {
        PublicationScope scope(parent, Mutation::Reparse, outside);
        rejects([&] { publish(staging, parent / "escaped", "reparse-swap publication"); });
        require(publication_trace.mutated && publication_trace.delete_denied &&
                    publication_trace.file_flushes == 1 && publication_trace.parent_flushes == 0,
                "publication did not refuse an in-place reparse swap after the staged-file flush");
    }
    require(std::filesystem::exists(staging) && !std::filesystem::exists(outside / "escaped"),
            "refused reparse swap consumed staging or escaped its parent");
    require(io::remove_tree(parent) && std::filesystem::exists(outside / "sentinel"),
            "removing swapped parent followed the junction");
    io::create_directories(parent);
    for (const auto mutation : {Mutation::FileIndex, Mutation::Volume}) {
        PublicationScope scope(parent, mutation);
        rejects([&] { publish(staging, parent / "escaped", "identity-change publication"); });
        require(publication_trace.mutated && publication_trace.file_flushes == 1 &&
                    publication_trace.parent_flushes == 0 && std::filesystem::exists(staging),
                "publication did not re-check the pinned parent's file index and volume");
    }
    require(io::remove_tree(staging), "identity-test staging cleanup failed");
}
#endif

#ifndef _WIN32
void interrupted_stdio() {
    struct Cookie {
        std::vector<std::byte> bytes;
        std::size_t position = 0;
        bool interrupted = false;
    } reader{{std::byte{1}, std::byte{2}, std::byte{3}}}, writer;
    const cookie_io_functions_t read_functions{
        .read = [](void* pointer, char* output, std::size_t bytes) -> ssize_t {
            auto& cookie = *static_cast<Cookie*>(pointer);
            if (!cookie.interrupted) {
                cookie.interrupted = true;
                errno = EINTR;
                return -1;
            }
            const auto count = std::min(bytes, cookie.bytes.size() - cookie.position);
            std::memcpy(output, cookie.bytes.data() + cookie.position, count);
            cookie.position += count;
            return static_cast<ssize_t>(count);
        }};
    Stream input(::fopencookie(&reader, "r", read_functions), &std::fclose);
    require(bool(input), "create interrupted read stream failed");
    std::array<std::byte, 3> actual{};
    io::read_exact(input.get(), actual);
    require(std::equal(actual.begin(), actual.end(), reader.bytes.begin()) &&
                reader.interrupted && !std::ferror(input.get()),
            "EINTR read did not complete and clear the stream error");
    const cookie_io_functions_t write_functions{
        .write = [](void* pointer, const char* input, std::size_t bytes) -> ssize_t {
            auto& cookie = *static_cast<Cookie*>(pointer);
            if (!cookie.interrupted) {
                cookie.interrupted = true;
                errno = EINTR;
                return 0;
            }
            const auto* begin = reinterpret_cast<const std::byte*>(input);
            cookie.bytes.insert(cookie.bytes.end(), begin, begin + bytes);
            return static_cast<ssize_t>(bytes);
        }};
    Stream output(::fopencookie(&writer, "w", write_functions), &std::fclose);
    require(bool(output) && std::setvbuf(output.get(), nullptr, _IONBF, 0) == 0,
            "create interrupted write stream failed");
    io::write_exact(output.get(), actual);
    require(writer.bytes == reader.bytes && writer.interrupted && !std::ferror(output.get()),
            "EINTR write did not complete and clear the stream error");
}
#endif

void run() {
#ifndef _WIN32
    interrupted_stdio();
#endif
    Fixture fixture;
#ifndef _WIN32
    struct stat root_status{}, first_status{};
    require(::stat(fixture.root.c_str(), &root_status) == 0, "stat sync fixture failed");
    trace_sync = interrupt_sync = true;
    io::create_directories(fixture.root / "publish-parent" / "generation");
    trace_sync = false;
    require(::stat((fixture.root / "publish-parent").c_str(), &first_status) == 0,
            "stat synced parent failed");
    require(!interrupt_sync && !wrong_sync_target && successful_syncs == 2 &&
                synced_inodes[0] == root_status.st_ino && synced_inodes[1] == first_status.st_ino,
            "new directory entries were not durably synced in parent order");
    successful_syncs = 0;
    trace_sync = true;
    io::create_directories(fixture.root / "publish-parent" / "generation");
    trace_sync = false;
    require(successful_syncs == 0, "existing directory traversal performed unnecessary syncs");
#endif
    const auto directory = fixture.root / std::filesystem::path(u8"state-\u03bb") / "nested";
    io::create_directories(directory);
    io::create_directories(directory);
    io::create_directories(directory / "." / "dot");
    const auto first_staging = directory / "first-staging", first = directory / "first";
    write(first_staging, std::byte{5});
    {
#ifdef _WIN32
        PublicationScope scope(directory);
#endif
        publish(first_staging, first, "first same-parent publication");
#ifdef _WIN32
        require(publication_trace.file_flushes == 1 && publication_trace.parent_flushes == 1 &&
                    publication_trace.delete_denied,
                "first publication did not preserve flush ordering and deny-DELETE pins");
#endif
    }
    require(!std::filesystem::exists(first_staging) && std::filesystem::exists(first),
            "first same-parent publication did not move staging");
    directory_publication(fixture.root);
    const auto current = directory / "current";
    write(current, std::byte{7});
    rejects([&] { Stream forbidden(io::open_binary_write(current), &std::fclose); });
    {
        Stream stream(io::open_binary_read(directory / "." / "current"), &std::fclose);
        std::array<std::byte, 3> bytes{};
        io::read_exact(stream.get(), bytes);
        require(bytes == std::array{std::byte{7}, std::byte{7}, std::byte{7}},
                "exclusive creation truncated an existing generation");
        rejects([&] { io::read_exact(stream.get(), bytes); });
    }
    const auto staging = directory / "staging";
    write(staging, std::byte{9});
    io::sync_directory(directory);
    publish(staging, current, "same-parent publication");
    io::sync_directory(directory);
    require(!std::filesystem::exists(staging), "atomic replace retained source");
    {
        Stream stream(io::open_binary_read(current), &std::fclose);
        std::array<std::byte, 3> bytes{};
        io::read_exact(stream.get(), bytes);
        require(bytes[0] == std::byte{9}, "atomic replace did not publish new bytes");
    }
#ifdef _WIN32
    // Publication must keep deny-DELETE parent pins, support a source below its destination
    // parent, and leave an existing reader on its original generation (POSIX rename).
    io::create_directories(directory / "stage-child");
    write(directory / "stage-child" / "next", std::byte{13});
    {
        Stream previous(io::open_binary_read(current), &std::fclose);
        publish(directory / "stage-child" / "next", current, "reader-preserving publication");
        std::array<std::byte, 3> bytes{};
        io::read_exact(previous.get(), bytes);
        require(bytes[0] == std::byte{9}, "replacement changed an existing reader's generation");
        Stream published(io::open_binary_read(current), &std::fclose);
        io::read_exact(published.get(), bytes);
        require(bytes[0] == std::byte{13}, "anchored replacement did not publish new generation");
    }
    io::create_directories(directory / "destination-child");
    write(staging, std::byte{15});
    publish(staging, directory / "destination-child" / "x", "single-character cross-parent publication");
    require(!std::filesystem::exists(staging), "cross-parent replacement retained source");
    {
        Stream published(io::open_binary_read(directory / "destination-child" / "x"), &std::fclose);
        std::array<std::byte, 3> bytes{};
        io::read_exact(published.get(), bytes);
        require(bytes[0] == std::byte{15}, "single-character replacement name was not published");
    }
#endif
    rejects([&] { Stream bad(io::open_binary_read(directory), &std::fclose); });
    rejects([&] { io::create_directories(directory / ".." / "escape"); });
    rejects([&] { Stream bad(io::open_binary_write(directory / ".." / "escape"), &std::fclose); });

    const auto outside = fixture.root / "outside";
    std::filesystem::create_directory(outside);
    const auto link = fixture.root / "redirect";
    std::error_code link_error;
    std::filesystem::create_directory_symlink(outside, link, link_error);
    if (link_error) throw std::system_error(link_error, "create no-follow test directory symlink");
    rejects([&] { io::create_directories(link / "escaped"); });
    rejects([&] { Stream bad(io::open_binary_write(link / "escaped"), &std::fclose); });
    require(!std::filesystem::exists(outside / "escaped"), "directory symlink escaped root");
    const auto file_link = fixture.root / "file-link";
    std::filesystem::create_symlink(current, file_link);
    rejects([&] { Stream bad(io::open_binary_read(file_link), &std::fclose); });
    rejects([&] { Stream bad(io::open_binary_write(file_link), &std::fclose); });
    write(staging, std::byte{11});
    rejects([&] { io::replace_path(staging, file_link); });
    require(std::filesystem::exists(staging), "rejected replacement consumed staging");
    write(outside / "sentinel", std::byte{42});
    // Confined same-volume cross-parent moves above remain valid. A redirected parent
    // or source link is not a valid cross-parent publication and must leave staging intact.
    rejects([&] { io::replace_path(staging, link / "escaped"); });
    rejects([&] { io::replace_path(link / "sentinel", directory / "escaped"); });
    rejects([&] { io::replace_path(link, directory / "escaped"); });
    rejects([&] { io::replace_path(file_link, directory / "escaped"); });
    require(std::filesystem::exists(staging) && !std::filesystem::exists(outside / "escaped") &&
                !std::filesystem::exists(directory / "escaped"),
            "unsafe cross-parent publication consumed staging or escaped confinement");
#ifdef _WIN32
    native_publication_guards(fixture.root, outside);
#endif
    const auto garbage = fixture.root / "garbage";
    io::create_directories(garbage / "nested");
    write(garbage / "nested" / "payload", std::byte{17});
    std::filesystem::create_directory_symlink(outside, garbage / "directory-link");
    std::filesystem::create_symlink(outside / "sentinel", garbage / "file-link");
#ifndef _WIN32
    successful_syncs = 0;
    trace_sync = true;
#endif
    const bool removed = io::remove_tree(garbage);
#ifndef _WIN32
    trace_sync = false;
    require(successful_syncs == 1 && !wrong_sync_target && synced_inodes[0] == root_status.st_ino,
            "removed tree was not durably fenced through its pinned parent");
#endif
    require(removed && !std::filesystem::exists(garbage), "anchored tree removal failed");
    require(!io::remove_tree(garbage) &&
                !io::remove_tree(fixture.root / "missing-parent" / "entry"),
            "missing removal entry was not reported missing");
    rejects([&] { (void)io::remove_tree(link / "sentinel"); });
    require(io::remove_tree(file_link) && std::filesystem::exists(current),
            "final file link removal followed its target");
    require(io::remove_tree(link) && std::filesystem::exists(outside / "sentinel"),
            "final directory link removal followed its target");
    {
        Stream sentinel(io::open_binary_read(outside / "sentinel"), &std::fclose);
        std::array<std::byte, 3> bytes{};
        io::read_exact(sentinel.get(), bytes);
        require(bytes == std::array{std::byte{42}, std::byte{42}, std::byte{42}},
                "anchored cleanup changed bytes outside the selected tree");
    }
    require(io::remove_tree(staging) && !std::filesystem::exists(staging),
            "regular file removal failed");
#ifdef _WIN32
    require(io::directory_sync_mode() == "ntfs-write-through", "wrong native directory policy");
#else
    require(io::directory_sync_mode() == "fsync", "wrong POSIX directory policy");
#endif
    std::cout << "OK durable file: dir_sync=" << io::directory_sync_mode() << '\n';
}
} // namespace

int main() {
    try { run(); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
