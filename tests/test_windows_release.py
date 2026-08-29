from __future__ import annotations

import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
RELEASE = ROOT / "packaging/windows/qwen38-3090-omp-v0.2"
PWSH = shutil.which("powershell.exe" if sys.platform == "win32" else "pwsh")


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
        self.assertEqual(
            config["session_checkpoint"],
            {"enabled": True, "quota_mib": 65536, "staging_mib": 256},
        )
        self.assertNotIn("persistent_cache", config)
        self.assertEqual(
            spec["qualification"]["required_gates"],
            [
                "gpu-only-thermal-envelope",
                "sm86-driver-hardware-identity",
                "authenticated-real-client-protocol",
                "advertised-context-retrieval",
                "process-restart-session-continuation",
                "omp-client-end-to-end",
                "bounded-gpu-performance-at-qualified-cap",
                "gaming-drain-restart-rollback",
            ],
        )
        self.assertEqual(
            spec["lifecycle"]["gpu_owner_controller_protocol"]["bundled_default"],
            "Control-GpuOwner.ps1",
        )
        self.assertEqual(
            spec["lifecycle"]["gpu_owner_controller_protocol"]["state_protection_helper"],
            "Protect-StateRoot.ps1",
        )
        self.assertEqual(
            spec["lifecycle"]["gpu_owner_controller_protocol"]["qualified_power_limit_w"],
            300,
        )

    def test_controller_server_options_match_runtime(self) -> None:
        controller = (RELEASE / "Control-Release.ps1").read_text(encoding="utf-8")
        start = controller.index("$serverArguments =")
        end = controller.index("$argumentLine =", start)
        controller_flags = set(re.findall(r"'(--[a-z0-9-]+)'", controller[start:end]))
        parser = (ROOT / "src/serve/serve_options.cpp").read_text(encoding="utf-8")
        runtime_flags = set(re.findall(r'arg == "(--[a-z0-9-]+)"', parser))
        self.assertEqual(controller_flags - runtime_flags, set())
        self.assertTrue(
            {
                "--api-key-file",
                "--reasoning-effort",
                "--session-checkpoint-dir",
                "--session-checkpoint-quota-mib",
                "--session-checkpoint-staging-mib",
            }.issubset(controller_flags)
        )
        self.assertTrue(
            {"--disk-cache", "--disk-cache-dir", "--disk-cache-gb", "--no-ui"}.isdisjoint(
                controller_flags
            )
        )

    def test_streaming_continuation_identity_reaches_encoder(self) -> None:
        source = (ROOT / "src/serve/responses_http.cpp").read_text(encoding="utf-8")
        self.assertIn(
            "stream->previous_response_id  = request.previous_response_id;", source
        )
        self.assertNotIn(
            "stream->previous_response_id  = std::move(request.previous_response_id);", source
        )

    def test_checkpoint_restore_evicts_idle_warmup_lane(self) -> None:
        source = (ROOT / "src/runtime/engine/concurrent_executor.h").read_text(
            encoding="utf-8"
        )
        start = source.index("restore_session(")
        end = source.index("[[nodiscard]] RuntimeStats runtime_stats()", start)
        restore = source[start:end]
        self.assertIn("instance_.program->evict_retained_lane(lane);", restore)
        self.assertNotIn(
            "slots_[lane] != nullptr || instance_.program->has_retained_lane(lane)",
            restore,
        )

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
            "Control-GpuOwner.ps1",
            "New-QualificationReceipt.ps1",
            "agent_protocol.py",
            "release-spec.json",
        ):
            self.assertIn(name, package_source)

    def test_public_installer_and_examples_preserve_security_boundaries(self) -> None:
        installer = (RELEASE / "Install-Release.ps1").read_text(encoding="utf-8")
        controller = (RELEASE / "Control-Release.ps1").read_text(encoding="utf-8")
        gpu_owner = (RELEASE / "Control-GpuOwner.ps1").read_text(encoding="utf-8")
        state_protection = (RELEASE / "Protect-StateRoot.ps1").read_text(encoding="utf-8")
        guide = (ROOT / "docs/rtx-3090-windows.md").read_text(encoding="utf-8")

        self.assertNotIn("NINFER_INSTALL_TEST_MODE", installer)
        self.assertIn("$script:InstallTestMode = $false", installer)
        self.assertNotIn("[switch]$InternalSourceTestMode", installer)
        self.assertIn("Control-GpuOwner.ps1", installer)
        self.assertIn("Initialize-NInferProtectedStateRoot", installer)
        self.assertNotIn("':(OI)(CI)M'", installer)
        self.assertNotIn("':(OI)(CI)RX'", installer)
        self.assertGreaterEqual(controller.count("server executable"), 2)
        self.assertGreaterEqual(controller.count("server config"), 2)
        self.assertIn(r"NInfer\qwen38-3090-gpu-owner", gpu_owner)
        self.assertIn("qualifiedPowerLimitW = 300", gpu_owner)
        self.assertIn("Assert-NInferNoReparseTree", state_protection)
        self.assertIn("SetOwner($administrators)", state_protection)
        self.assertNotIn("Get-Content .\\SHA256SUMS", guide)
        self.assertIn("components.ninfer_variants", guide)

        packaged_readme = (ROOT / "README.md").read_text(encoding="utf-8")
        command_blocks = re.findall(
            r"```(?:powershell|bat)\n(.*?)```", guide + "\n" + packaged_readme, re.DOTALL
        )
        serving_blocks = [block for block in command_blocks if "ninfer-serve.exe" in block]
        self.assertGreaterEqual(len(serving_blocks), 2)
        for block in serving_blocks:
            self.assertIn("--api-key-file", block)


@unittest.skipUnless(PWSH, "pwsh is not installed")
class PowerShellWindowsReleaseTests(unittest.TestCase):
    def run_script(self, script: Path, *arguments: str, timeout: int = 600) -> dict[str, object]:
        assert PWSH is not None
        with tempfile.TemporaryDirectory() as directory:
            stdout_path = Path(directory) / "stdout.txt"
            stderr_path = Path(directory) / "stderr.txt"
            with stdout_path.open("w", encoding="utf-8") as stdout, stderr_path.open(
                "w", encoding="utf-8"
            ) as stderr:
                result = subprocess.run(
                    [
                        PWSH,
                        "-NoLogo",
                        "-NoProfile",
                        "-NonInteractive",
                        *(["-ExecutionPolicy", "Bypass"] if sys.platform == "win32" else []),
                        "-File",
                        str(script),
                        *arguments,
                    ],
                    cwd=ROOT,
                    stdin=subprocess.DEVNULL,
                    text=True,
                    stdout=stdout,
                    stderr=stderr,
                    timeout=timeout,
                )
            stdout_text = stdout_path.read_text(encoding="utf-8", errors="replace")
            stderr_text = stderr_path.read_text(encoding="utf-8", errors="replace")
        self.assertEqual(result.returncode, 0, f"{stdout_text}\n{stderr_text}")
        lines = [line for line in stdout_text.splitlines() if line.strip()]
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
