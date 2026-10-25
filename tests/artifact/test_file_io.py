"""Run the same real explicit-offset byte-I/O contract on Linux and Windows."""

from concurrent.futures import ThreadPoolExecutor
import mmap
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.artifact.file_io import (
    Writeback, _PAGE_BYTES, discard_cached_pages, read_at, write_at,
)


class ArtifactFileIoTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="ninfer-artifact-io-")
        self.path = Path(self.directory.name) / "bytes-\u03bb.bin"
        self.fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_RDWR | getattr(os, "O_BINARY", 0), 0o600)

    def tearDown(self):
        os.close(self.fd)
        self.directory.cleanup()

    def test_binary_offsets_short_read_and_writeback(self):
        payload = b"\x00\r\n\x1a\xff" * 19
        self.assertEqual(write_at(self.fd, payload, 31), len(payload))
        self.assertEqual(read_at(self.fd, len(payload), 31), payload)
        self.assertEqual(read_at(self.fd, len(payload) + 10, 31), payload)
        self.assertEqual(read_at(self.fd, 1, 31 + len(payload)), b"")
        self.assertEqual(read_at(self.fd, 0, 0), b"")
        self.assertEqual(write_at(self.fd, b"", 0), 0)
        mutable = bytearray(b"mutable\x00\x1a\r\n")
        self.assertEqual(write_at(self.fd, memoryview(mutable), 0), len(mutable))
        writeback = Writeback()
        writeback.written(self.fd, len(payload))
        writeback.flush()
        discard_cached_pages(self.fd, 31, len(payload))
        self.assertEqual(read_at(self.fd, len(mutable), 0), mutable)
        self.assertEqual(_PAGE_BYTES, mmap.PAGESIZE)
        if os.name != "nt":
            self.assertEqual(_PAGE_BYTES, os.sysconf("SC_PAGE_SIZE"))
            self.assertEqual(read_at(self.fd, 4096, 0), os.pread(self.fd, 4096, 0))
        with self.assertRaises((OSError, ValueError)):
            read_at(self.fd, 1, -1)
        with self.assertRaises(OSError):
            read_at(-1, 1, 0)

    def test_shared_handle_concurrent_explicit_offsets(self):
        def transfer(index):
            payload = bytes([index, 10, 13, 26, 255]) * 137
            offset = index * 4096
            for _ in range(40):
                self.assertEqual(write_at(self.fd, payload, offset), len(payload))
                self.assertEqual(read_at(self.fd, len(payload), offset), payload)
        with ThreadPoolExecutor(max_workers=4) as team:
            list(team.map(transfer, range(4)))

    def test_sparse_offset_above_four_gib(self):
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes
            import msvcrt
            control = ctypes.WinDLL("kernel32", use_last_error=True).DeviceIoControl
            control.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                ctypes.c_void_p]
            control.restype = wintypes.BOOL
            returned = wintypes.DWORD()
            if not control(msvcrt.get_osfhandle(self.fd), 0x900C4, None, 0, None, 0,
                           ctypes.byref(returned), None):
                raise ctypes.WinError(ctypes.get_last_error())
        offset = (1 << 32) + 123
        payload = b"high offset\x00\r\n\x1a\xff"
        self.assertEqual(write_at(self.fd, payload, offset), len(payload))
        self.assertEqual(read_at(self.fd, len(payload), offset), payload)
        self.assertEqual(os.fstat(self.fd).st_size, offset + len(payload))


if __name__ == "__main__":
    unittest.main()
