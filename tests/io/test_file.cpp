#include "core/io/file.h"
#include "core/buffer.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#endif

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <utility>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template <class F> void rejects(F operation) {
    try { operation(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("invalid file operation accepted");
}
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        std::random_device random;
        for (;;) {
            root = std::filesystem::temp_directory_path() / ("ninfer-file-" + std::to_string(random()));
            if (std::filesystem::create_directory(root)) break;
        }
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};
void run() {
    using namespace ninfer;
    using namespace ninfer::io;
    Fixture fixture;
    const auto path = fixture.root / std::filesystem::path(u8"sparse-\u03bb.dat");
#ifdef _WIN32
    const auto sparse = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(sparse != INVALID_HANDLE_VALUE, "create sparse fixture failed");
    DWORD returned = 0;
    const auto set_sparse = DeviceIoControl(sparse, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                                            &returned, nullptr);
    CloseHandle(sparse);
    require(set_sparse, "mark sparse fixture failed");
#endif
    constexpr std::uint64_t offset = (std::uint64_t{1} << 32U) + 4096;
    std::array<std::byte, 123> expected;
    for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = std::byte(i);
    {
        std::ofstream stream(path, std::ios::binary);
        stream.seekp(static_cast<std::streamoff>(offset));
        stream.write(reinterpret_cast<const char*>(expected.data()), expected.size());
        stream.close();
        require(bool(stream), "write sparse fixture failed");
    }
    ReadOnlyFile buffered(path);
    require(buffered.bytes() == offset + expected.size(), "file length lost high 32 bits");
    std::array<std::byte, 123> actual{};
    buffered.read_exact(offset, actual);
    require(actual == expected, "explicit 64-bit buffered read offset differs");
    buffered.read_exact(buffered.bytes(), {});
    rejects([&] { buffered.read_exact(buffered.bytes(), actual); });
    auto moved = std::move(buffered);
    rejects([&] { buffered.read_exact(0, actual); });
    moved.read_exact(offset, actual);
    require(actual == expected, "move lost file ownership");
    ReadOnlyFile direct(path, ReadMode::Direct);
    const auto view = direct.view();
    const auto alignment = std::max(view.memory_alignment, view.offset_alignment);
    const auto window = direct_read_window(view, offset, expected.size());
    HostBuffer memory(window.bytes, alignment);
    const auto count = direct.read_direct(window.offset,
                                          {static_cast<std::byte*>(memory.data()), window.bytes});
    require(count >= window.required, "short final direct block lost required bytes");
    const auto* bytes = static_cast<const std::byte*>(memory.data()) + window.prefix;
    require(std::equal(expected.begin(), expected.end(), bytes), "64-bit direct offset differs");
    rejects([&] { (void)direct.read_direct(1, {static_cast<std::byte*>(memory.data()), window.bytes}); });
    rejects([&] { direct.read_exact(offset, actual); });
    rejects([&] { (void)direct_read_window(view, direct.bytes(), 1); });
    rejects([&] { ReadOnlyFile directory(fixture.root); });
    const auto empty = fixture.root / "empty";
    std::ofstream(empty, std::ios::binary).close();
    ReadOnlyFile zero(empty);
    zero.read_exact(0, {});
    require(zero.bytes() == 0, "empty file has nonzero length");
    std::cout << "OK immutable file: Unicode, 64-bit offsets, EOF, move, direct alignment\n";
}
} // namespace
int main() {
    try { run(); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
