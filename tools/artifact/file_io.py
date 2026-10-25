"""Explicit-offset artifact I/O, durable writeback and Linux-only page-cache hints."""

from __future__ import annotations

import os
import mmap

IO_CHUNK_BYTES = 8 * 1024 * 1024
WRITEBACK_BYTES = 64 * 1024 * 1024
_PAGE_BYTES = mmap.PAGESIZE

if os.name == "nt":
    import ctypes
    from ctypes import wintypes
    import msvcrt

    class _Overlapped(ctypes.Structure):
        _fields_ = [
            ("Internal", ctypes.c_size_t),
            ("InternalHigh", ctypes.c_size_t),
            ("Offset", wintypes.DWORD),
            ("OffsetHigh", wintypes.DWORD),
            ("hEvent", wintypes.HANDLE),
        ]

    _kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    for _name in ("ReadFile", "WriteFile"):
        _function = getattr(_kernel, _name)
        _function.argtypes = [
            wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(_Overlapped),
        ]
        _function.restype = wintypes.BOOL

    def _operation(offset: int, count: int) -> _Overlapped:
        if offset < 0 or count < 0:
            raise ValueError("artifact I/O requires nonnegative offsets and sizes")
        if offset > (1 << 63) - 1 or count > 0xFFFFFFFF:
            raise OverflowError("artifact I/O exceeds native offset or request limits")
        return _Overlapped(Offset=offset & 0xFFFFFFFF, OffsetHigh=offset >> 32)

    def read_at(fd: int, count: int, offset: int) -> bytes:
        """Read raw bytes at an explicit offset on a synchronous CRT file handle."""
        operation = _operation(offset, count)
        handle = msvcrt.get_osfhandle(fd)
        if not count:
            return b""
        buffer = ctypes.create_string_buffer(count)
        received = wintypes.DWORD()
        if not _kernel.ReadFile(handle, buffer, count, ctypes.byref(received),
                                ctypes.byref(operation)):
            error = ctypes.get_last_error()
            if error == 38:  # ERROR_HANDLE_EOF
                return b""
            raise ctypes.WinError(error)
        return ctypes.string_at(buffer, received.value)

    def write_at(fd: int, data: bytes | memoryview, offset: int) -> int:
        """Write raw bytes at an explicit offset, never a shared seek/write pair."""
        view = memoryview(data).cast("B")
        operation = _operation(offset, len(view))
        handle = msvcrt.get_osfhandle(fd)
        if not view:
            return 0
        if view.readonly:
            backing = bytes(view)
            buffer = ctypes.c_char_p(backing)
        else:
            buffer = (ctypes.c_char * len(view)).from_buffer(view)
        written = wintypes.DWORD()
        if not _kernel.WriteFile(handle, buffer, len(view), ctypes.byref(written),
                                 ctypes.byref(operation)):
            raise ctypes.WinError(ctypes.get_last_error())
        return written.value
else:
    def read_at(fd: int, count: int, offset: int) -> bytes:
        return os.pread(fd, count, offset)

    def write_at(fd: int, data: bytes | memoryview, offset: int) -> int:
        return os.pwrite(fd, data, offset)


def discard_cached_pages(fd: int, offset: int = 0, count: int | None = None) -> None:
    # This is an advisory Linux cache policy, not a read/write or durability fallback.
    if os.name == "nt":
        return
    if count is None:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    elif count > 0:
        begin = offset // _PAGE_BYTES * _PAGE_BYTES
        end = (offset + count + _PAGE_BYTES - 1) // _PAGE_BYTES * _PAGE_BYTES
        os.posix_fadvise(fd, begin, end - begin, os.POSIX_FADV_DONTNEED)


class Writeback:
    """Bound dirty output across all open shards; release clean pages after writeback."""

    def __init__(self) -> None:
        self._bytes = 0
        self._fds: set[int] = set()

    def written(self, fd: int, count: int) -> None:
        self._fds.add(fd)
        self._bytes += count
        if self._bytes >= WRITEBACK_BYTES:
            self.flush()

    def flush(self) -> None:
        for fd in self._fds:
            if os.name == "nt":
                os.fsync(fd)
            else:
                os.fdatasync(fd)
            discard_cached_pages(fd)
        self._fds.clear()
        self._bytes = 0
