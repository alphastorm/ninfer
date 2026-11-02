"""Portable production stop route shared by real durable runners (no server emulation)."""
from __future__ import annotations

import os
import secrets
import signal
import subprocess


class ManagedProcess:
    def __init__(self, argv: list[str], log) -> None:
        self.stop_event = None
        if os.name == "nt":
            self.stop_event = "Local\\NInfer-Serve-Stop-" + secrets.token_hex(16)
            argv = [*argv, "--stop-event", self.stop_event]
        self.child = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)

    def stop(self, timeout: int = 180) -> int:
        if self.child.poll() is None:
            if self.stop_event:
                # The server creates the event with its production ACL. Qualification must
                # have the authorized Windows token; never precreate or weaken that event.
                import ctypes
                from ctypes import wintypes
                kernel = ctypes.WinDLL("kernel32", use_last_error=True)
                kernel.OpenEventW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
                kernel.OpenEventW.restype = wintypes.HANDLE
                kernel.SetEvent.argtypes = [wintypes.HANDLE]
                kernel.SetEvent.restype = wintypes.BOOL
                kernel.CloseHandle.argtypes = [wintypes.HANDLE]
                handle = kernel.OpenEventW(0x0002, False, self.stop_event)
                if not handle:
                    raise OSError(ctypes.get_last_error(), "server-created stop event unavailable")
                try:
                    if not kernel.SetEvent(handle):
                        raise OSError(ctypes.get_last_error(), "server stop event signal failed")
                finally:
                    kernel.CloseHandle(handle)
            else:
                self.child.send_signal(signal.SIGTERM)
        try:
            return self.child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.child.kill()
            self.child.wait()
            raise RuntimeError("managed final-save deadline exceeded") from None
