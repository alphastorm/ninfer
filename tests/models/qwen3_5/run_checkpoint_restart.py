#!/usr/bin/env python3
"""Compare fragment identity, then export/import in separate real model processes."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    if not os.environ.get("NINFER_TEST_ARTIFACT"):
        print("SKIP: NINFER_TEST_ARTIFACT is required")
        return 77
    if len(sys.argv) not in (2, 3):
        raise SystemExit("usage: run_checkpoint_restart.py BINARY [none|mtp|dflash2]")
    binary = str(Path(sys.argv[1]).resolve())
    backend = sys.argv[2] if len(sys.argv) == 3 else "dflash2"
    with tempfile.TemporaryDirectory(prefix="ninfer-checkpoint-restart-") as root:
        checkpoint = str(Path(root) / "checkpoint")
        for stage in ("identity", "export", "import"):
            destination = str(Path(root) / "identity") if stage == "identity" else checkpoint
            result = subprocess.run([binary, stage, destination, backend], check=False)
            if result.returncode:
                return result.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
