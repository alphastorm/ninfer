#!/usr/bin/env python3
"""CMake CUDA compile launcher: stable per-source symbols and temporary names."""

from pathlib import Path
import subprocess
import sys


def main():
    args = sys.argv[1:]
    if len(args) < 4 or args[0] != "--source-root" or args[2] != "--":
        raise SystemExit("usage: deterministic-nvcc.py --source-root ROOT -- [LAUNCHER...] NVCC ARGS...")
    root = Path(args[1]).resolve()
    command = args[3:]
    sources = [Path(arg) for arg in command if arg.endswith(".cu") and Path(arg).is_file()]
    if sources:
        if len(sources) != 1:
            raise SystemExit("deterministic CUDA compilation requires one source")
        try:
            relative = sources[0].resolve().relative_to(root).as_posix()
        except ValueError:
            raise SystemExit("CUDA source is outside the declared source root") from None
        # CUDA 13.1 does not reliably seed module IDs with a 64-bit numeric seed.
        # Its documented string form derives a stable 32-bit CRC instead.
        # Append nvcc options so an existing launcher (e.g. ccache) still receives
        # the compiler as its first argument. CMake supplies distinct output dirs.
        command += ["--frandom-seed=" + relative, "--objdir-as-tempdir"]
    return subprocess.run(command).returncode


if __name__ == "__main__":
    raise SystemExit(main())
