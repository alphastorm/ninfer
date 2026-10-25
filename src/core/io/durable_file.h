#pragma once

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string_view>

namespace ninfer::io {

// Every existing directory component is opened without following symlinks/reparse points.
// Staging creation is exclusive, with owner-only POSIX permissions. Open failures throw;
// the caller owns the returned stream and must close it. Only regular files are accepted.
[[nodiscard]] std::FILE* open_binary_write(const std::filesystem::path& path);
[[nodiscard]] std::FILE* open_binary_read(const std::filesystem::path& path);
void create_directories(const std::filesystem::path& path);
void read_exact(std::FILE* file, std::span<std::byte> bytes);
void write_exact(std::FILE* file, std::span<const std::byte> bytes);
// Flushes stdio and fsync/FlushFileBuffers. No durability acknowledgement precedes this.
void flush_file(std::FILE* file);
// Source and destination parents are pinned, with no symlink/reparse traversal. Cross-volume
// moves fail rather than copy. Windows uses REPLACE_EXISTING | WRITE_THROUGH.
void replace_path(const std::filesystem::path& source, const std::filesystem::path& destination);
// POSIX fsync(directory). On Windows this is deliberately a no-op: the supported native
// durability policy is flushed files plus write-through replacement, not directory fsync.
void sync_directory(const std::filesystem::path& path);
[[nodiscard]] std::string_view directory_sync_mode() noexcept;

} // namespace ninfer::io
