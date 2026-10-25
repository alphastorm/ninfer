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
// moves fail rather than copy. Windows flushes the staged file, renames relative to the
// pinned destination with NtSetInformationFile(FileRenameInformationEx), then flushes that
// directory handle. The native policy requires NTFS on Windows 10 1709 or newer.
void replace_path(const std::filesystem::path& source, const std::filesystem::path& destination);
// Removes a regular file, final link, or complete directory tree through anchored handles.
// Ancestor links are rejected; final links are unlinked, never traversed. Returns false if
// missing, otherwise true after deletion (and POSIX parent fsync). Other failures throw.
[[nodiscard]] bool remove_tree(const std::filesystem::path& path);
// POSIX fsync(directory). On Windows this is deliberately a no-op: replace_path owns the
// native staged-file/anchored-rename/parent-handle flush sequence.
void sync_directory(const std::filesystem::path& path);
[[nodiscard]] std::string_view directory_sync_mode() noexcept;

} // namespace ninfer::io
