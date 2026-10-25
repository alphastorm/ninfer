#include "core/io/durable_file.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

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
void run() {
    Fixture fixture;
    const auto directory = fixture.root / std::filesystem::path(u8"state-\u03bb") / "nested";
    io::create_directories(directory);
    io::create_directories(directory);
    const auto current = directory / "current";
    write(current, std::byte{7});
    rejects([&] { Stream forbidden(io::open_binary_write(current), &std::fclose); });
    {
        Stream stream(io::open_binary_read(current), &std::fclose);
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
