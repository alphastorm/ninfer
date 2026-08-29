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


def powershell_code_without_literals(text: str) -> str:
    text = re.sub(r'(?ms)^@".*?^"@', "", text)
    text = re.sub(r"(?ms)^@'.*?^'@", "", text)
    text = re.sub(r"'(?:''|[^'])*'", "", text)
    text = re.sub(r'"[^"\n]*"', "", text)
    return re.sub(r"(?m)#.*$", "", text)


class ReleaseAuthorityTest(unittest.TestCase):
    def test_public_authorities_have_one_eligible_identity_lineage(self) -> None:
        self.assertFalse(STALE_REVIEW_CLOSURE.exists())
        qualification = load(QUALIFICATION_PATH)
        spec = load(RELEASE_SPEC_PATH)
        assets = load(ASSET_RECEIPT_PATH)
        golden = load(GOLDEN_RECEIPT_PATH)
        lifecycle = load(LIFECYCLE_RECEIPT_PATH)
        state_security = load(STATE_SECURITY_RECEIPT_PATH)

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
            "Control-GpuOwner.ps1",
            "Control-Release.ps1",
            "Install-Release.ps1",
            "Protect-StateRoot.ps1",
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
            r"(?:TestMode|Mock|Fixture|Fault|Inject(?:ed|ion)?|Bypass|"
            r"Simulat(?:e|ed|ion)|Harness|test[_-]?mode|mock[_-]|fixture[_-]|"
            r"fault[_-]|inject[_-]|bypass[_-]|simulat[_-]|harness[_-]|"
            r"NINFER_[A-Z0-9_]*(?:TEST|MOCK|FAULT|INJECT|BYPASS|SIMULAT|HARNESS))"
        )
        for path in scripts:
            source = powershell_code_without_literals(path.read_text(encoding="utf-8"))
            self.assertIsNone(hook.search(source), str(path))


if __name__ == "__main__":
    unittest.main()
