from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
QUALIFICATION_ROOT = ROOT / "docs" / "qualification"
QUALIFICATION_PATH = QUALIFICATION_ROOT / "qwen3.8-27b-rtx-4090-v0.1.json"
RELEASE_ROOT = ROOT / "packaging" / "windows" / "qwen38-4090-v0.1"
RELEASE_SPEC_PATH = RELEASE_ROOT / "release-spec.json"
ASSET_RECEIPT_PATH = QUALIFICATION_ROOT / "receipts" / "qwen3.8-27b-rtx-4090-assets.json"
GOLDEN_RECEIPT_PATH = (
    QUALIFICATION_ROOT / "receipts" / "qwen3.8-27b-rtx-4090-golden-equivalent.json"
)
LIFECYCLE_RECEIPT_PATH = (
    QUALIFICATION_ROOT / "receipts" / "qwen3.8-27b-rtx-4090-lifecycle.json"
)
STATE_SECURITY_RECEIPT_PATH = (
    QUALIFICATION_ROOT / "receipts" / "qwen3.8-27b-rtx-4090-state-security.json"
)
FAILED_DELETE_RECEIPT_PATH = (
    QUALIFICATION_ROOT / "receipts" / "qwen3.8-27b-rtx-4090-failed-delete.json"
)
UPGRADE_ACL_RECEIPT_PATH = (
    QUALIFICATION_ROOT / "receipts" / "qwen3.8-27b-rtx-4090-upgrade-rollback-acl.json"
)
PROTOCOL_RECEIPT_PATH = (
    QUALIFICATION_ROOT / "receipts" / "qwen3.8-27b-rtx-4090-protocol.json"
)
LONG_RESTART_RECEIPT_PATH = (
    QUALIFICATION_ROOT / "receipts" / "qwen3.8-27b-rtx-4090-long-restart.json"
)
PERFORMANCE_RECEIPT_PATH = (
    QUALIFICATION_ROOT / "receipts" / "qwen3.8-27b-rtx-4090-performance.json"
)
MEMBER_BINDING_RECEIPT_PATH = (
    QUALIFICATION_ROOT / "receipts" / "qwen3.8-27b-rtx-4090-asset-member-binding.json"
)
PROTECTED_LOG_RECEIPT_PATH = (
    QUALIFICATION_ROOT / "receipts" / "qwen3.8-27b-rtx-4090-protected-request-log.json"
)
STATUS_TIMING_RECEIPT_PATH = (
    QUALIFICATION_ROOT / "receipts" / "qwen3.8-27b-rtx-4090-status-timing.json"
)
STALE_REVIEW_CLOSURE = (
    QUALIFICATION_ROOT / "receipts" / "qwen3.8-27b-rtx-4090-review-closure.json"
)
RELEASE_ID = "qwen38-4090-v0.1"


def load(path: Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise AssertionError(f"JSON authority is not an object: {path}")
    return value


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def iter_strings(value: object):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for key, child in value.items():
            yield str(key)
            yield from iter_strings(child)
    elif isinstance(value, list):
        for child in value:
            yield from iter_strings(child)


def powershell_identifiers_without_literals(text: str) -> str:
    text = re.sub(r'(?ms)^@".*?^"@', "", text)
    text = re.sub(r"(?ms)^@'.*?^'@", "", text)
    text = re.sub(r"'(?:''|[^'])*'", "", text)
    text = re.sub(r'"[^"\n]*"', "", text)
    return re.sub(r"(?m)#.*$", "", text)


def powershell_predicates(text: str) -> str:
    text = re.sub(r"(?m)#.*$", "", text)
    predicates: list[str] = []
    for match in re.finditer(r"(?i)\b(?:if|elseif|while|until|switch|for|param)\s*\(", text):
        start = match.start()
        position = match.end() - 1
        depth = 0
        quote: str | None = None
        escaped = False
        while position < len(text):
            character = text[position]
            if escaped:
                escaped = False
            elif ord(character) == 96:
                escaped = True
            elif quote is not None:
                if character == quote:
                    quote = None
            elif character in ("'", '"'):
                quote = character
            elif character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
                if depth == 0:
                    predicates.append(text[start : position + 1])
                    break
            position += 1
    return "\n".join(predicates)


class ReleaseAuthorityTest(unittest.TestCase):
    def test_public_authorities_have_one_eligible_identity_lineage(self) -> None:
        self.assertFalse(STALE_REVIEW_CLOSURE.exists())
        qualification = load(QUALIFICATION_PATH)
        spec = load(RELEASE_SPEC_PATH)
        assets = load(ASSET_RECEIPT_PATH)
        golden = load(GOLDEN_RECEIPT_PATH)
        lifecycle = load(LIFECYCLE_RECEIPT_PATH)
        state_security = load(STATE_SECURITY_RECEIPT_PATH)
        failed_delete = load(FAILED_DELETE_RECEIPT_PATH)
        member_binding = load(MEMBER_BINDING_RECEIPT_PATH)
        protected_log = load(PROTECTED_LOG_RECEIPT_PATH)
        status_timing = load(STATUS_TIMING_RECEIPT_PATH)

        expected_qualification_fields = {
            "artifact_type",
            "schema_version",
            "status",
            "qualification_level",
            "beta_support_qualified",
            "release_eligible",
            "release_id",
            "release_version",
            "qualification_target",
            "source",
            "identity",
            "package",
            "golden_equivalent",
            "live_evidence",
            "release_gates",
            "state_security",
            "authority",
            "restoration",
            "publication",
            "stable_release_performed",
            "stable_promotion_performed",
            "permanent_route_mutation",
            "decision",
        }
        self.assertEqual(set(qualification), expected_qualification_fields)

        self.assertEqual(qualification["release_id"], RELEASE_ID)
        self.assertEqual(qualification["status"], "passed")
        self.assertIs(qualification["release_eligible"], True)
        self.assertIs(qualification["beta_support_qualified"], True)
        self.assertEqual(spec["release_id"], RELEASE_ID)
        self.assertEqual(spec["deployment_profile"], "qwen38-4090-v0.1")
        authority = qualification["authority"]
        spec_authority = spec["qualification_authority"]
        self.assertEqual(authority["role"], "external-final-qualification-authority")
        self.assertIs(authority["supersedes_package_candidate_status"], True)
        self.assertEqual(authority["superseded_status"], "candidate_ready")
        self.assertEqual(authority["binding"], "SHA256SUMS-bound-sidecar")
        self.assertEqual(
            authority["sidecar_filename"], spec_authority["external_sidecar_filename"]
        )
        self.assertEqual(
            spec_authority["in_archive_status"], "candidate-only-not-release-eligible"
        )
        self.assertEqual(
            spec_authority["external_artifact_type"],
            "ninfer_public_windows_release_qualification",
        )
        self.assertEqual(spec_authority["external_required_status"], "passed")
        self.assertIs(spec_authority["external_required_release_eligible"], True)

        source = qualification["source"]
        spec_source = spec["source"]
        self.assertEqual(source["qualified_commit"], spec_source["qualified_source_head"])
        self.assertEqual(source["source_archive_sha256"], spec_source["source_archive_sha256"])
        self.assertRegex(source["package_source_commit"], r"^[0-9a-f]{40}$")
        self.assertRegex(source["package_source_archive_sha256"], r"^[0-9a-f]{64}$")

        identity = qualification["identity"]
        artifacts = spec["qualified_artifacts"]
        for field in ("binary_sha256", "config_sha256", "deployment_profile"):
            self.assertEqual(identity[field], artifacts[field])
        self.assertEqual(identity["model_artifact_sha256"], spec["model"]["sha256"])
        self.assertEqual(golden["identity"]["binary_sha256"], identity["binary_sha256"])
        self.assertEqual(golden["identity"]["config_sha256"], identity["config_sha256"])
        self.assertEqual(
            golden["identity"]["model_artifact_sha256"], identity["model_artifact_sha256"]
        )
        self.assertEqual(golden["identity"]["deployment_profile"], identity["deployment_profile"])
        self.assertEqual(golden["identity"]["patch_stack_sha"], source["qualified_commit"])
        self.assertEqual(
            sha256(GOLDEN_RECEIPT_PATH),
            qualification["golden_equivalent"]["receipt_sha256"],
        )
        self.assertEqual(
            sha256(LIFECYCLE_RECEIPT_PATH),
            qualification["release_gates"]["L"]["release_lifecycle_receipt_sha256"],
        )
        self.assertEqual(
            sha256(STATE_SECURITY_RECEIPT_PATH),
            qualification["release_gates"]["L"]["state_security_receipt_sha256"],
        )
        self.assertEqual(lifecycle["evidence_class"], "instrumented-unshipped-installer-lifecycle")
        self.assertIs(lifecycle["exact_shipped_installer_executed"], False)
        self.assertEqual(lifecycle["published_installer_callable_test_hooks"], 0)
        self.assertEqual(
            lifecycle["state_protection_evidence_class"], "exact-shipped-helper-real-acl"
        )
        self.assertIs(lifecycle["state_protection_stubbed"], False)
        self.assertIs(lifecycle["real_acl_evidence_included"], True)
        self.assertEqual(
            state_security["evidence_class"],
            "exact-shipped-state-helper-and-installer-prewrite-real-acl",
        )
        self.assertIs(state_security["exact_shipped_installer_executed"], True)
        self.assertIs(state_security["exact_shipped_installer_full_install"], False)
        l_gate = qualification["release_gates"]["L"]
        self.assertEqual(
            sha256(FAILED_DELETE_RECEIPT_PATH), l_gate["native_failed_delete_receipt_sha256"]
        )
        self.assertEqual(
            sha256(UPGRADE_ACL_RECEIPT_PATH),
            l_gate["real_upgrade_rollback_acl_receipt_sha256"],
        )
        self.assertEqual(
            sha256(MEMBER_BINDING_RECEIPT_PATH),
            l_gate["asset_member_binding_receipt_sha256"],
        )
        self.assertEqual(
            sha256(PROTECTED_LOG_RECEIPT_PATH),
            l_gate["protected_request_log_receipt_sha256"],
        )
        self.assertEqual(
            sha256(STATUS_TIMING_RECEIPT_PATH),
            l_gate["populated_state_status_timing_receipt_sha256"],
        )
        self.assertEqual(failed_delete["status"], "passed")
        self.assertEqual(failed_delete["runtime_source_commit"], source["qualified_commit"])
        self.assertEqual(failed_delete["binary_sha256"], identity["binary_sha256"])
        self.assertIn("foreign-session LRU publication", " ".join(failed_delete["contracts"]))
        self.assertIn("quota rejection", " ".join(failed_delete["contracts"]))
        self.assertIn("not secure erasure", failed_delete["logical_delete_residual"])
        self.assertIn("cannot resurrect", " ".join(failed_delete["contracts"]))
        self.assertIn("rolls back to the old pointer", " ".join(failed_delete["contracts"]))
        self.assertEqual(member_binding["status"], "passed")
        self.assertEqual(member_binding["package_sha256"], qualification["package"]["sha256"])
        self.assertEqual(len(member_binding["zip_script_bindings"]), 7)
        self.assertEqual(member_binding["outer_checksum_entries"], 14)
        self.assertIs(member_binding["outer_checksum_self_reference"], False)
        self.assertEqual(
            member_binding["new_package"]["package_source_sha256"],
            member_binding["new_package"]["standalone_sha256"],
        )
        self.assertEqual(protected_log["status"], "passed")
        self.assertIs(protected_log["state_tree_protection_verified"], True)
        self.assertIs(protected_log["owner_state_root_under_release_state"], True)
        self.assertEqual(status_timing["status"], "passed")
        self.assertEqual(status_timing["source_commit"], source["qualified_commit"])
        self.assertEqual(status_timing["binary_sha256"], identity["binary_sha256"])
        self.assertEqual(status_timing["populated_file_count"], 2048)
        self.assertEqual(status_timing["populated_logical_bytes"], 2 * 1024**3)
        self.assertIs(status_timing["worst_case_quota_file_count_claim"], False)
        self.assertIs(status_timing["quota_full_generation_topology_measured"], False)
        self.assertLessEqual(
            status_timing["status_elapsed_seconds"], status_timing["maximum_status_seconds"]
        )
        self.assertEqual(state_security["null_dacl_rejections"], 1)
        self.assertEqual(state_security["null_dacl_effective_read_observations"], 1)
        self.assertEqual(state_security["nvidia_smi_evidence_class"], "exact-trusted-absolute-query")
        self.assertEqual(state_security["default_gpu_owner_state_mutations"], 0)
        self.assertEqual(state_security["gpu_power_limit_mutations"], 0)
        self.assertEqual(state_security["trusted_absolute_nvidia_smi_scripts"], 3)
        self.assertEqual(state_security["bare_nvidia_smi_command_invocations"], 0)
        self.assertIs(state_security["literal_predicate_hook_scan"], True)
        self.assertGreaterEqual(len(lifecycle["substituted_components"]), 6)
        self.assertIn("Get-FileHash", lifecycle["substituted_functions"])
        self.assertIn("Protect-StateRoot.ps1", lifecycle["exact_unsubstituted_components"])
        live = qualification["live_evidence"]
        self.assertIs(live["prior_runtime_evidence_reused"], False)
        self.assertEqual(sha256(PROTOCOL_RECEIPT_PATH), live["protocol"]["receipt_sha256"])
        self.assertEqual(
            sha256(LONG_RESTART_RECEIPT_PATH),
            live["checkpoint_restart"]["receipt_sha256"],
        )
        self.assertEqual(
            sha256(PERFORMANCE_RECEIPT_PATH), live["performance"]["receipt_sha256"]
        )

        package = qualification["package"]
        self.assertEqual(package["sha256"], assets["package"]["sha256"])
        self.assertEqual(package["sbom"]["sha256"], assets["sbom"]["sha256"])
        self.assertEqual(
            package["generic_gpu_owner_controller_sha256"],
            artifacts["generic_gpu_owner_controller_sha256"],
        )
        self.assertEqual(
            package["generic_gpu_owner_controller_sha256"],
            assets["published_gpu_owner_controller"]["sha256"],
        )
        self.assertEqual(
            package["state_protection_sha256"],
            artifacts["state_protection_sha256"],
        )
        self.assertEqual(
            package["state_protection_sha256"],
            assets["published_state_protection"]["sha256"],
        )

        asset_map = {entry["filename"]: entry for entry in assets["assets"]}
        for filename in (
            "Compare-MtpQualification.ps1",
            "Control-GpuOwner.ps1",
            "Control-Release.ps1",
            "Install-Release.ps1",
            "Invoke-Qualification.ps1",
            "New-Package.ps1",
            "New-QualificationReceipt.ps1",
            "Protect-StateRoot.ps1",
            "package-build-receipt.json",
            "ninfer-4090-qwen38-v0.1.0-win-x64-release-manifest.json",
            "ninfer-4090-qwen38-v0.1.0-win-x64-source.tar.gz",
            "ninfer-4090-qwen38-v0.1.0-win-x64.spdx.json",
            "ninfer-4090-qwen38-v0.1.0-win-x64.zip",
            "ninfer-4090-qwen38-v0.1.0-win-x64-qualification.json",
            "SHA256SUMS",
        ):
            self.assertIn(filename, asset_map)

        public_authorities = []
        for path in QUALIFICATION_ROOT.rglob("*.json"):
            value = load(path)
            if value.get("artifact_type") == "ninfer_public_windows_release_qualification":
                public_authorities.append(path.resolve())
            if value.get("release_id") == RELEASE_ID and "release_eligible" in value:
                self.assertIs(value["release_eligible"], True, str(path))
        self.assertEqual(public_authorities, [QUALIFICATION_PATH.resolve()])

        publication = qualification["publication"]
        self.assertEqual(publication["private_identifier_scan"], "passed")
        self.assertEqual(publication["staged_asset_findings"], 0)
        self.assertEqual(publication["tracked_source_findings"], 0)
        self.assertIs(publication["public_release_performed"], False)
        disclosure_patterns = (
            re.compile(r"(?:[A-Za-z]:\\Users\\|/Users/|/home/)", re.IGNORECASE),
            re.compile(r"\b(?:nyc|sf)-[a-z0-9-]+\b", re.IGNORECASE),
            re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b"),
        )
        for value in iter_strings(qualification):
            for pattern in disclosure_patterns:
                self.assertIsNone(pattern.search(value), value)

    def test_published_installer_has_no_callable_test_or_fault_hook(self) -> None:
        text = (RELEASE_ROOT / "Install-Release.ps1").read_text(encoding="utf-8")
        for forbidden in (
            "InstallTestMode",
            "NINFER_TEST_INSTALL",
            "Invoke-InstallFault",
            "NInferSimulatedInterruption",
        ):
            self.assertNotIn(forbidden, text)

    def test_private_windows_path_survives_json_roundtrip_for_disclosure_scan(self) -> None:
        value = json.loads(json.dumps({"path": r"C:\Users\Example\receipt.json"}))
        pattern = re.compile(r"[A-Za-z]:\\Users\\", re.IGNORECASE)
        self.assertTrue(any(pattern.search(text) for text in iter_strings(value)))

    def test_every_published_powershell_script_has_no_hook_vocabulary(self) -> None:
        scripts = sorted(RELEASE_ROOT.glob("*.ps1"))
        self.assertEqual(
            {path.name for path in scripts},
            {
                "Compare-MtpQualification.ps1",
                "Control-GpuOwner.ps1",
                "Control-Release.ps1",
                "Install-Release.ps1",
                "Invoke-Qualification.ps1",
                "New-Package.ps1",
                "New-QualificationReceipt.ps1",
                "Protect-StateRoot.ps1",
            },
        )
        hook = re.compile(
            r"(?:\b(?:TestMode|Mock|Fixture|Fault|Bypass|Harness)\b|"
            r"\bInject(?:ed|ion)?\b|\bSimulat(?:e|ed|ion)\b|"
            r"test[_-]?mode|mock[_-]|fixture[_-]|"
            r"fault[_-]|inject[_-]|bypass[_-]|simulat[_-]|harness[_-]|"
            r"NINFER_[A-Z0-9_]*(?:TEST|MOCK|FAULT|INJECT|BYPASS|SIMULAT|HARNESS))",
            re.IGNORECASE,
        )
        for path in scripts:
            source = path.read_text(encoding="utf-8")
            self.assertIsNone(
                hook.search(powershell_identifiers_without_literals(source)), str(path)
            )
            self.assertIsNone(hook.search(powershell_predicates(source)), str(path))

    def test_hook_scan_keeps_string_literals_in_executable_predicates(self) -> None:
        source = "if ($env:NINFER_TEST_MODE -eq 'fixture') { Write-Output forbidden }"
        self.assertIn("NINFER_TEST_MODE", powershell_predicates(source))
        self.assertIn("'fixture'", powershell_predicates(source))


if __name__ == "__main__":
    unittest.main()
