from __future__ import annotations

import hashlib
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
    def test_public_beta_receipt_schema_and_asset_binding(self) -> None:
        receipt_path = (
            ROOT
            / "docs/qualification/receipts/qwen3.8-27b-rtx-3090-v0.2.0.json"
        )
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        self.assertEqual(receipt["artifact_type"], "ninfer_rtx3090_beta_qualification")
        self.assertEqual(receipt["schema_version"], 3)
        self.assertEqual(receipt["status"], "incomplete")
        self.assertFalse(receipt["beta_qualified"])
        self.assertFalse(receipt["automatic_route_activation_allowed"])
        self.assertFalse(receipt["stable_promotion_performed"])
        self.assertFalse(receipt["production_route_activation_performed"])
        candidate = receipt["candidate"]
        runtime_source = candidate["runtime_source_commit"]
        package_source = candidate["package_source_commit"]
        self.assertRegex(runtime_source, r"^[0-9a-f]{40}$")
        self.assertRegex(package_source, r"^[0-9a-f]{40}$")
        runtime_diff = subprocess.check_output(
            [
                "git", "diff", "--name-only", runtime_source, package_source, "--",
                "src", "include", "apps", "cmake", "CMakeLists.txt", "CMakePresets.json",
            ],
            cwd=ROOT,
            text=True,
        ).strip()
        self.assertEqual(runtime_diff, "")
        self.assertFalse(candidate["runtime_source_paths_changed"])
        self.assertEqual(
            candidate["runtime_source_diff_sha256"],
            hashlib.sha256(b"").hexdigest(),
        )

        assets = receipt["release_assets"]
        expected_assets = {
            "package": "ninfer-rtx3090-omp-v0.2.0-windows-x86_64-cuda12.8-rtx3090.tar.gz",
            "source_archive": "ninfer-rtx3090-omp-v0.2.0-source.tar.gz",
            "spdx_sbom": "ninfer-rtx3090-omp-v0.2.0-windows-x86_64-cuda12.8-rtx3090.spdx.json",
            "checksums": "SHA256SUMS",
            "package_build_receipt": "package-build-receipt.json",
        }
        for name, filename in expected_assets.items():
            self.assertEqual(assets[name]["filename"], filename)
            self.assertRegex(assets[name]["sha256"], r"^[0-9a-f]{64}$")
            self.assertGreater(assets[name]["bytes"], 0)

        def sha256(path: Path) -> str:
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            return digest.hexdigest()

        source_assets = {
            "installer_sha256": RELEASE / "Install-Release.ps1",
            "lifecycle_controller_sha256": RELEASE / "Control-Release.ps1",
            "gpu_owner_controller_sha256": RELEASE / "Control-GpuOwner.ps1",
            "state_protection_helper_sha256": RELEASE / "Protect-StateRoot.ps1",
            "qualification_constructor_sha256": RELEASE / "New-QualificationReceipt.ps1",
        }
        for field, path in source_assets.items():
            self.assertEqual(assets[field], sha256(path), field)
        self.assertRegex(receipt["candidate"]["config_sha256"], r"^[0-9a-f]{64}$")

        authority = receipt["qualification_authority"]
        self.assertEqual(authority["authority"], "pending-deferred-windows-gate")
        self.assertEqual(authority["exact_package_sha256"], assets["package"]["sha256"])
        self.assertFalse(authority["supersession_performed"])
        self.assertIsNone(authority["supersedes_package_build_receipt_qualification_status"])
        self.assertIsNone(authority["supersedes_packaged_release_spec_qualification_status"])
        self.assertIsNone(authority["supersedes_source_archive_receipt_status"])
        self.assertFalse(authority["historical_build_receipts_mutated"])

        instrumented = receipt["qualification"]["windows_lifecycle_instrumented"]
        shipped = receipt["qualification"]["windows_lifecycle_shipped"]
        self.assertEqual(
            instrumented["evidence_class"], "generated-instrumented-transaction-harness"
        )
        self.assertFalse(instrumented["production_installer_executed"])
        self.assertFalse(instrumented["effective_acl_evidence"])
        self.assertEqual(shipped["status"], "not_run")
        self.assertTrue(shipped["fresh_package_gate_deferred"])
        self.assertEqual(
            shipped["deferred_reason"],
            "fresh-windows-rtx3090-unavailable-after-user-handoff",
        )
        self.assertFalse(shipped["instrumented_harness_used"])
        self.assertEqual(shipped["installer_sha256"], assets["installer_sha256"])
        security = receipt["qualification"]["windows_state_security"]
        self.assertEqual(security["status"], "not_run")
        self.assertTrue(security["fresh_package_gate_deferred"])
        security_fixture = receipt["qualification"]["windows_state_security_fixture"]
        self.assertEqual(security_fixture["status"], "passed")
        self.assertEqual(
            security_fixture["evidence_class"],
            "instrumented-function-shim-no-hardware-claim",
        )
        self.assertFalse(security_fixture["hardware_claimed"])

        disclosure = receipt["public_disclosure"]
        self.assertEqual(disclosure["policy_version"], 1)
        self.assertEqual(
            disclosure["forbidden_marker_classes"],
            ["private-fleet-identifiers", "private-home-paths", "credential-material"],
        )
        self.assertEqual(
            disclosure["tracked_source_private_marker_or_unclassified_credential_findings"],
            0,
        )
        self.assertEqual(disclosure["binary_package_private_path_findings"], 0)
        self.assertEqual(disclosure["classified_private_fleet_or_credential_findings"], 0)
        self.assertFalse(disclosure["private_fleet_projection_included"])
        self.assertEqual(disclosure["credential_values_recorded"], 0)
        self.assertRegex(disclosure["raw_scan_sha256"], r"^[0-9a-f]{64}$")

        serialized = json.dumps(receipt, sort_keys=True)
        self.assertNotIn("gpu_uuid", serialized)
        self.assertIsNone(re.search(r"(?i)(?:[A-Z]:\\\\Users\\\\|/Users/|/home/)", serialized))

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
        self.assertEqual(spec["qualification"]["status"], "hardware-pending")
        self.assertEqual(
            spec["qualification"]["status_authority"],
            "immutable-pre-hardware-package-build",
        )
        later = spec["qualification"]["later_external_beta_authority"]
        self.assertEqual(later["artifact_type"], "ninfer_rtx3090_beta_qualification")
        self.assertEqual(later["schema_version"], 3)
        self.assertTrue(later["supersedes_only_when_exact_package_sha256_matches"])
        self.assertFalse(later["mutates_packaged_history"])

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

        for marker in (
            "InstallTestMode",
            "Invoke-InstallFault",
            "NINFER_TEST_INSTALL_",
            "NInferSimulatedInterruption",
            "InternalSourceTestMode",
        ):
            self.assertNotIn(marker, installer)
        self.assertIn("Control-GpuOwner.ps1", installer)
        self.assertIn("Initialize-NInferProtectedStateRoot", installer)
        self.assertIn("Protect-NInferRetainedSecretAcls", installer)
        self.assertIn("state_root = $ownerStateRoot", installer)
        self.assertIn("-StateRoot ([string]$owner.state_root)", controller)
        self.assertIn("Assert-NInferProtectedStateRoot $StateRoot", controller)
        self.assertIn("Set-NInferProtectedFileAcl $installedKey", installer)
        self.assertNotIn("$env:USERNAME", installer)
        self.assertNotIn("':(OI)(CI)M'", installer)
        self.assertNotIn("':(OI)(CI)RX'", installer)
        self.assertGreaterEqual(controller.count("server executable"), 2)
        self.assertGreaterEqual(controller.count("server config"), 2)
        self.assertIn(r"NInfer\qwen38-3090-gpu-owner", gpu_owner)
        self.assertIn("qualifiedPowerLimitW = 300", gpu_owner)
        self.assertIn("Assert-NInferNoReparseTree", state_protection)
        self.assertIn("SetOwner($administrators)", state_protection)
        self.assertIn("CreateDirectoryW", state_protection)
        self.assertNotIn("-Recurse -Force -ErrorAction SilentlyContinue", state_protection)
        self.assertIn("grants access outside SYSTEM or Administrators", state_protection)
        self.assertIn("protected state has a NULL DACL", state_protection)
        self.assertIn("GetFolderPath('System')", installer)
        self.assertIn("GetFolderPath('System')", controller)
        self.assertIn("GetFolderPath('System')", gpu_owner)
        for source in (installer, controller, gpu_owner):
            self.assertNotIn("& nvidia-smi.exe", source)
        forbidden_hooks = (
            "InstallTestMode", "Invoke-InstallFault", "NINFER_TEST_INSTALL_",
            "NInferSimulatedInterruption", "InternalSourceTestMode", "TestBypass",
            "FaultInjection", "SimulatedFailure", "SimulatedInterruption",
        )
        for path in RELEASE.glob("*.ps1"):
            source = path.read_text(encoding="utf-8")
            for marker in forbidden_hooks:
                self.assertNotIn(marker, source, f"{path.name} contains {marker}")
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
        self.assertEqual(
            value["artifact_type"],
            "ninfer_windows_lifecycle_instrumented_regression",
        )
        self.assertEqual(value["evidence_class"], "generated-instrumented-transaction-harness")
        self.assertFalse(value["production_installer_executed"])
        self.assertFalse(value["production_state_protection_executed"])
        self.assertEqual(
            value["state_protection_evidence_class"], "generated-stub-no-acl-semantics"
        )
        self.assertEqual(value["substitution_count"], 22)
        self.assertEqual(len(value["substitution_manifest"]), 22)
        self.assertFalse(value["security_claims_included"])
        self.assertFalse(value["effective_acl_evidence"])
        self.assertRegex(value["production_installer_sha256"], r"^[0-9a-f]{64}$")
        self.assertRegex(value["instrumented_installer_sha256"], r"^[0-9a-f]{64}$")
        self.assertNotEqual(
            value["production_installer_sha256"], value["instrumented_installer_sha256"]
        )

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
