from __future__ import annotations

import importlib.util
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "new_sbom",
    ROOT / "packaging" / "windows" / "qwen38-4090-v0.1" / "new_sbom.py",
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ReleaseSbomTest(unittest.TestCase):
    def test_inventory_is_deterministic_and_normalizes_windows_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "release.zip"
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("release\\bin\\ninfer-serve.exe", b"server")
                output.writestr("release\\release-manifest.json", b"{}")
            arguments = {
                "release_tag": "v0.2.0-qwen38-4090-beta.1",
                "source_commit": "a" * 40,
                "created": "2026-08-28T21:05:12Z",
            }
            first = MODULE.build_document(archive, **arguments)
            second = MODULE.build_document(archive, **arguments)
            self.assertEqual(first, second)
            self.assertEqual(first["spdxVersion"], "SPDX-2.3")
            self.assertEqual(
                [item["fileName"] for item in first["files"]],
                [
                    "./release/bin/ninfer-serve.exe",
                    "./release/release-manifest.json",
                ],
            )
            self.assertEqual(len(first["packages"][0]["hasFiles"]), 2)

    def test_duplicate_normalized_member_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "release.zip"
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("release\\file.txt", b"one")
                output.writestr("release/file.txt", b"two")
            with self.assertRaisesRegex(ValueError, "duplicate normalized member"):
                MODULE.build_document(
                    archive,
                    release_tag="v0.2.0-qwen38-4090-beta.1",
                    source_commit="a" * 40,
                    created="2026-08-28T21:05:12Z",
                )


if __name__ == "__main__":
    unittest.main()
