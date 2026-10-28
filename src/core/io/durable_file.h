#pragma once

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string_view>

namespace ninfer::io {

#ifdef _WIN32
// Absolute extended local-drive spelling for Win32/std::filesystem metadata consumers.
// Independent of MAX_PATH opt-in policy. Rejects UNC/device namespaces, traversal and
// alternate streams; this is lexical validation only, not a substitute for anchored I/O.
// Joining a POSIX wire-relative name requires make_preferred() before Win32 metadata access.
[[nodiscard]] std::filesystem::path extended_local_path(const std::filesystem::path& path);
#endif

// Every existing directory component is opened without following symlinks/reparse points.
// Staging creation is exclusive, with owner-only POSIX permissions. Open failures throw;
// the caller owns the returned stream and must close it. Only regular files are accepted.
[[nodiscard]] std::FILE* open_binary_write(const std::filesystem::path& path);
[[nodiscard]] std::FILE* open_binary_read(const std::filesystem::path& path);
void create_directories(const std::filesystem::path& path);
// OS-reported stream errors throw std::system_error with the OS code. The Windows CRT maps
// Win32 errors onto errno lossily (ERROR_HANDLE_DISK_FULL becomes EINVAL), so a failing CRT
// call reports the Win32 code it recorded and errno only when it recorded none.
void read_exact(std::FILE* file, std::span<std::byte> bytes);
void write_exact(std::FILE* file, std::span<const std::byte> bytes);
// Flushes stdio and fsync/FlushFileBuffers. No durability acknowledgement precedes this.
void flush_file(std::FILE* file);
// Releases the stream even when closing fails; a failed close, including a deferred write
// error, throws.
void close_file(std::FILE* file);
// Source and destination parents are pinned, with no symlink/reparse traversal. Cross-volume
// moves fail rather than copy. Windows flushes the staged file, renames relative to the
// pinned destination with NtSetInformationFile(FileRenameInformationEx), then flushes that
// directory handle. Same-volume cross-parent moves and real directory sources are supported;
// directory payloads must already be flushed by the caller. Parent pins share READ/WRITE but
// deny DELETE; every pinned identity and reparse attribute is rechecked before the rename.
// The native policy requires NTFS on Windows 10 1709 or newer.
// A supplied rename_attempted becomes true immediately before the rename system call and is
// never cleared. A failure that leaves it false changed neither name; once it is true the
// destination may name the source even though the call throws.
void replace_path(const std::filesystem::path& source, const std::filesystem::path& destination,
                  bool* rename_attempted = nullptr);
// Removes a regular file, final link, or complete directory tree through anchored handles.
// Ancestor links are rejected; final links are unlinked, never traversed. Returns false if
// missing, otherwise true after deletion (and POSIX parent fsync). Other failures throw.
[[nodiscard]] bool remove_tree(const std::filesystem::path& path);
// POSIX fsync(directory). On Windows this is deliberately a no-op: replace_path owns the
// native staged-file/anchored-rename/parent-handle flush sequence.
void sync_directory(const std::filesystem::path& path);
[[nodiscard]] std::string_view directory_sync_mode() noexcept;

} // namespace ninfer::io
