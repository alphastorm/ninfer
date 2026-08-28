from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
RELEASE = ROOT / "packaging/windows/qwen38-3090-omp-v0.2"
PWSH = shutil.which("pwsh")


class WindowsReleaseContractTests(unittest.TestCase):
    def test_pinned_release_and_authenticated_c1_defaults(self) -> None:
        spec = json.loads((RELEASE / "release-spec.json").read_text(encoding="utf-8"))
        config = json.loads((RELEASE / "server-config.json").read_text(encoding="utf-8"))
        self.assertEqual(spec["source"]["lineage_base_sha"], "c467349e375d6aa76afca63c0042bbc0869549aa")
        self.assertEqual(spec["build_profile"], "omp-v0.2.0-rtx3090")
        self.assertEqual(spec["gpu"]["cuda_architecture"], "sm_86")
        self.assertEqual(spec["gpu"]["compute_capability"], "8.6")
        self.assertEqual(
            spec["model"],
            {
                "repository": "neroued/Qwen3.8-27B-NInfer",
                "revision": "18dfc887423fa5aabf3cb56fac41490e462b3fab",
                "filename": "qwen3_8_27b.ninfer",
                "sha256": "eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e",
                "bytes": 18210531328,
            },
        )
        self.assertEqual(
            spec["lifecycle"]["state_pointers"],
            ["prepared_release", "active_release", "previous_release"],
        )
        self.assertEqual(spec["network"]["allowed_listen_host_classes"], ["loopback", "tailscale-ipv4"])
        self.assertEqual(spec["network"]["authentication"], "required-api-key-file")
        self.assertEqual(config["listen"]["host"], "127.0.0.1")
        self.assertEqual(config["authentication"]["mode"], "required-api-key-file")
        self.assertEqual(config["engine"]["max_context"], 65536)
        self.assertEqual(config["engine"]["kv_capacity"], "auto")
        self.assertEqual(config["engine"]["kv_dtype"], "int8")
        self.assertEqual(config["engine"]["max_concurrency"], 1)
        self.assertEqual(config["speculative"], {"backend": "mtp", "draft_tokens": 3})

    def test_release_sources_contain_no_old_hardware_or_cache_identity(self) -> None:
        forbidden = ("4090", "DirectStorage", "directstorage", "legacy-managed-copy")
        for path in RELEASE.iterdir():
            if path.suffix not in {".ps1", ".json"}:
                continue
            text = path.read_text(encoding="utf-8")
            for marker in forbidden:
                self.assertNotIn(marker, text, f"{path.name} contains {marker}")
        package_source = (ROOT / "tools/release/package.py").read_text(encoding="utf-8")
        for name in (
            "Install-Release.ps1",
            "Control-Release.ps1",
            "New-QualificationReceipt.ps1",
            "release-spec.json",
        ):
            self.assertIn(name, package_source)


@unittest.skipUnless(PWSH, "pwsh is not installed")
class PowerShellWindowsReleaseTests(unittest.TestCase):
    def run_script(self, script: Path, *arguments: str, timeout: int = 600) -> dict[str, object]:
        assert PWSH is not None
        result = subprocess.run(
            [
                PWSH,
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-File",
                str(script),
                *arguments,
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
        self.assertEqual(result.returncode, 0, f"{result.stdout}\n{result.stderr}")
        lines = [line for line in result.stdout.splitlines() if line.strip()]
        self.assertTrue(lines, "PowerShell regression emitted no receipt")
        value = json.loads(lines[-1])
        self.assertEqual(value["status"], "passed")
        return value

    def test_transaction_lifecycle(self) -> None:
        value = self.run_script(
            ROOT / "tests/test_windows_release_lifecycle.ps1",
            "-InstallerPath",
            str(RELEASE / "Install-Release.ps1"),
            "-ControllerPath",
            str(RELEASE / "Control-Release.ps1"),
        )
        self.assertEqual(value["injected_failures"], 10)
        self.assertEqual(value["interrupted_repairs"], 2)
        self.assertEqual(value["restart_model_rehash_calls"], 0)
        self.assertEqual(value["uninstalls"], 1)

    def test_deterministic_assets_and_receipts(self) -> None:
        value = self.run_script(
            ROOT / "tests/test_windows_release_assets.ps1",
            "-PackageBuilderPath",
            str(RELEASE / "New-Package.ps1"),
            "-ReceiptConstructorPath",
            str(RELEASE / "New-QualificationReceipt.ps1"),
            "-ServerConfigPath",
            str(RELEASE / "server-config.json"),
            "-SourceRoot",
            str(ROOT),
            "-PythonExecutable",
            sys.executable,
        )
        self.assertEqual(value["deterministic_packages"], 2)
        self.assertEqual(value["public_listen_rejections"], 1)
        self.assertEqual(value["secret_free_receipts"], 3)


if __name__ == "__main__":
    unittest.main()
