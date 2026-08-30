from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/qualification/qualify_rtx3090.py"
SPEC = importlib.util.spec_from_file_location("qualify_rtx3090", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class QualificationOrchestratorTests(unittest.TestCase):
    def command(self, state: Path, fixture: Path) -> list[str]:
        return [
            sys.executable,
            str(SCRIPT),
            "--source-root",
            str(ROOT),
            "--state-dir",
            str(state),
            "--builder",
            "builder.invalid",
            "--target",
            "target.invalid",
            "--builder-vcpkg",
            "C:/build/vcpkg.cmake",
            "--builder-vcpkg-installed",
            "C:/build/vcpkg-installed",
            "--neutral-runtime-release-root",
            "C:/release/neutral",
            "--model-path",
            "C:/models/qwen.ninfer",
            "--long-fixture",
            str(fixture),
            "--omp-root",
            "C:/acceptance/omp",
            "--dry-run",
        ]

    def test_dry_run_enumerates_complete_no_effect_lane(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "fixture.json"
            fixture.write_text("[]\n", encoding="utf-8")
            result = subprocess.run(
                self.command(root / "state", fixture),
                cwd=ROOT,
                check=True,
                capture_output=True,
                text=True,
            )
            receipt = json.loads(result.stdout)
            self.assertEqual(receipt["status"], "dry_run")
            self.assertEqual(tuple(receipt["phases"]), MODULE.PHASES)
            self.assertIn("neutral_build", receipt["phases"])
            self.assertIn("restore", receipt["phases"])

    def test_checkpoint_rejects_orchestrator_revision_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "fixture.json"
            fixture.write_text("[]\n", encoding="utf-8")
            command = self.command(root / "state", fixture)
            subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
            state_path = root / "state" / "state.json"
            state = json.loads(state_path.read_text(encoding="utf-8"))
            state["orchestrator_sha256"] = "0" * 64
            state_path.write_text(json.dumps(state), encoding="utf-8")
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("checkpoint belongs to another source", result.stderr)

    def test_directory_scan_detects_dynamic_private_home(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "safe.bin").write_bytes(b"public bytes")
            (root / "unsafe.bin").write_bytes(b"C:\\Users\\builder\\source")
            previous = MODULE.os.environ.get("USERPROFILE")
            MODULE.os.environ["USERPROFILE"] = r"C:\Users\builder"
            try:
                receipt = MODULE.scan_directory(root)
            finally:
                if previous is None:
                    MODULE.os.environ.pop("USERPROFILE", None)
                else:
                    MODULE.os.environ["USERPROFILE"] = previous
            self.assertEqual(receipt["files"], 2)
            self.assertGreater(receipt["findings"], 0)

    def test_directory_scan_accepts_neutral_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "runtime.dll").write_bytes(b"path=C:\\build\\runtime")
            receipt = MODULE.scan_directory(root)
            self.assertEqual(receipt["findings"], 0)


if __name__ == "__main__":
    unittest.main()
