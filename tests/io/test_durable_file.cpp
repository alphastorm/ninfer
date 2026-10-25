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

#ifndef _WIN32
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
    io::replace_path(staging, current);
    io::sync_directory(directory);
    require(!std::filesystem::exists(staging), "atomic replace retained source");
    {
        Stream stream(io::open_binary_read(current), &std::fclose);
        std::array<std::byte, 3> bytes{};
        io::read_exact(stream.get(), bytes);
        require(bytes[0] == std::byte{9}, "atomic replace did not publish new bytes");
    }
#ifdef _WIN32
    // Publication must keep strict parent pins, support a source below its destination
    // parent, and leave an existing reader on its original generation (POSIX rename).
    io::create_directories(directory / "stage-child");
    write(directory / "stage-child" / "next", std::byte{13});
    {
        Stream previous(io::open_binary_read(current), &std::fclose);
        io::replace_path(directory / "stage-child" / "next", current);
        std::array<std::byte, 3> bytes{};
        io::read_exact(previous.get(), bytes);
        require(bytes[0] == std::byte{9}, "replacement changed an existing reader's generation");
        Stream published(io::open_binary_read(current), &std::fclose);
        io::read_exact(published.get(), bytes);
        require(bytes[0] == std::byte{13}, "anchored replacement did not publish new generation");
    }
    io::create_directories(directory / "destination-child");
    write(staging, std::byte{15});
    io::replace_path(staging, directory / "destination-child" / "current");
    require(!std::filesystem::exists(staging), "cross-parent replacement retained source");
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
