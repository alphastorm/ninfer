#include "core/io/file.h"

#include <bit>
#include <limits>
#include <stdexcept>

namespace ninfer::io {
namespace {
void position(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(INT64_MAX))
        throw std::invalid_argument("file offset exceeds signed 64-bit range");
}

bool valid_handle(FileView file) noexcept {
#ifdef _WIN32
    return file.native_handle != -1;
#else
    return file.native_handle >= 0;
#endif
}
} // namespace

const std::error_category& read_error_category() noexcept {
#ifdef _WIN32
    return std::system_category();
#else
    return std::generic_category();
#endif
}

void validate_direct_read(FileView file, std::uint64_t offset, std::span<std::byte> destination) {
    if (!valid_handle(file) || file.mode != ReadMode::Direct ||
        !std::has_single_bit(file.memory_alignment) ||
        !std::has_single_bit(file.offset_alignment) || offset % file.offset_alignment ||
        destination.size() % file.offset_alignment ||
        (!destination.empty() &&
         (!destination.data() ||
          reinterpret_cast<std::uintptr_t>(destination.data()) % file.memory_alignment)) ||
        destination.size() > static_cast<std::size_t>(INT64_MAX))
        throw std::invalid_argument("invalid direct read range or alignment");
    position(offset);
    if (destination.size() > static_cast<std::uint64_t>(INT64_MAX) - offset)
        throw std::invalid_argument("direct read end exceeds signed 64-bit range");
}

DirectReadWindow direct_read_window(FileView file, std::uint64_t offset, std::size_t bytes) {
    if (!valid_handle(file) || file.mode != ReadMode::Direct || !bytes ||
        !std::has_single_bit(file.offset_alignment) || offset > file.bytes ||
        bytes > file.bytes - offset)
        throw std::invalid_argument("invalid direct read window");
    const auto alignment = file.offset_alignment;
    const auto prefix = offset % alignment;
    if (bytes > std::numeric_limits<std::size_t>::max() - prefix - (alignment - 1))
        throw std::overflow_error("direct read window size overflow");
    const auto required = bytes + prefix;
    const auto count = (required + alignment - 1) / alignment * alignment;
    position(offset - prefix);
    if (count > static_cast<std::uint64_t>(INT64_MAX) - (offset - prefix))
        throw std::overflow_error("direct read window end exceeds signed 64-bit range");
    return {offset - prefix, count, prefix, required};
}

} // namespace ninfer::io
