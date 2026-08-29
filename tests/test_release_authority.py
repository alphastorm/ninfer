from __future__ import annotations

import hashlib
import json
from pathlib import Path
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


class ReleaseAuthorityTest(unittest.TestCase):
    def test_public_authorities_have_one_eligible_identity_lineage(self) -> None:
        self.assertFalse(STALE_REVIEW_CLOSURE.exists())
        qualification = load(QUALIFICATION_PATH)
        spec = load(RELEASE_SPEC_PATH)
        assets = load(ASSET_RECEIPT_PATH)
        golden = load(GOLDEN_RECEIPT_PATH)

        self.assertEqual(qualification["release_id"], RELEASE_ID)
        self.assertEqual(qualification["status"], "passed")
        self.assertIs(qualification["release_eligible"], True)
        self.assertIs(qualification["beta_support_qualified"], True)
        self.assertEqual(spec["release_id"], RELEASE_ID)
        self.assertEqual(spec["deployment_profile"], "qwen38-4090-v0.1")

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
            sha256(GOLDEN_RECEIPT_PATH),
            spec["qualified_source_evidence"]["golden_equivalent_receipt_sha256"],
        )

        package = qualification["package"]
        self.assertEqual(package["sha256"], artifacts["package_sha256"])
        self.assertEqual(package["sha256"], assets["package"]["sha256"])
        self.assertEqual(package["sbom"]["sha256"], artifacts["sbom_sha256"])
        self.assertEqual(package["sbom"]["sha256"], assets["sbom"]["sha256"])
        self.assertEqual(
            package["generic_gpu_owner_controller_sha256"],
            artifacts["generic_gpu_owner_controller_sha256"],
        )
        self.assertEqual(
            package["generic_gpu_owner_controller_sha256"],
            assets["published_gpu_owner_controller"]["sha256"],
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

    def test_published_installer_has_no_ambient_test_mode_input(self) -> None:
        text = (RELEASE_ROOT / "Install-Release.ps1").read_text(encoding="utf-8")
        self.assertNotIn("NINFER_INSTALL_TEST_MODE", text)
        self.assertEqual(text.count("$script:InstallTestMode = $false"), 1)


if __name__ == "__main__":
    unittest.main()
