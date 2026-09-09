"""Contracts of the shared native Windows lifecycle tree and its lane specifications."""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import unittest

from tools.release import package as release_package

ROOT = Path(__file__).resolve().parents[1]
WINDOWS = ROOT / "packaging/windows"
LANES = sorted(path for path in (WINDOWS / "lanes").iterdir() if path.is_dir())
SHARED_SCRIPTS = (
    "Install-Release.ps1",
    "Control-Release.ps1",
    "Control-GpuOwner.ps1",
    "Protect-StateRoot.ps1",
    "New-Package.ps1",
)


def is_ancestor(ancestor: str, descendant: str = "HEAD") -> bool:
    return subprocess.run(
        ["git", "-C", str(ROOT), "merge-base", "--is-ancestor", ancestor, descendant],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ).returncode == 0


class SharedLifecycleTreeTests(unittest.TestCase):
    def test_controller_server_options_match_runtime(self) -> None:
        controller = (WINDOWS / "Control-Release.ps1").read_text(encoding="utf-8")
        start = controller.index("$serverArguments =")
        end = controller.index("$argumentLine =", start)
        controller_flags = set(re.findall(r"'(--[a-z0-9-]+)'", controller[start:end]))
        parser = (ROOT / "src/serve/serve_options.cpp").read_text(encoding="utf-8")
        runtime_flags = set(re.findall(r'arg == "(--[a-z0-9-]+)"', parser))
        self.assertEqual(controller_flags - runtime_flags, set())
        self.assertTrue({
            "--api-key-file", "--binary-sha256", "--artifact-sha256", "--config-sha256",
            "--deployment-profile", "--device-state-slots", "--host-state-slots",
            "--max-private-continuations", "--session-checkpoint-dir",
            "--session-checkpoint-quota-mib", "--session-checkpoint-staging-mib",
        }.issubset(controller_flags))
        # Lane-branch options mainline never had.
        self.assertTrue({"--reasoning-effort", "--disk-cache", "--prompt-cache", "--no-ui",
                         "--wddm-evictable-budget"}.isdisjoint(controller_flags))

    def test_shared_scripts_carry_no_lane_literals(self) -> None:
        """One tree serves every lane: GPU names, architectures, power figures, release ids, and
        task names come from the lane specification, never from the scripts."""
        forbidden = re.compile(r"3090|4090|5090|sm_8[69]|sm_120|\b(?:300|370|450)\s*W\b|omp-v0\.2|NInfer-Qwen38-[0-9]{4}")
        for name in SHARED_SCRIPTS + ("agent_protocol.py",):
            text = (WINDOWS / name).read_text(encoding="utf-8")
            hits = {match.group(0) for match in forbidden.finditer(text)}
            self.assertEqual(hits, set(), f"{name} carries lane literals {sorted(hits)}")

    def test_packaged_lifecycle_inventory_is_one_set(self) -> None:
        packager_files = set(release_package._WINDOWS_LIFECYCLE_FILES)
        installer = (WINDOWS / "Install-Release.ps1").read_text(encoding="utf-8")
        start = installer.index("$lifecycleBin = ")
        installed = set(re.findall(r"'((?:Control|Install|Protect)-[A-Za-z]+\.ps1)'", installer[start:start + 600]))
        self.assertEqual(installed, packager_files)
        for name in packager_files | {"agent_protocol.py", "RELEASE_NOTES.md"}:
            self.assertTrue((WINDOWS / name).is_file(), name)
        self.assertTrue((ROOT / "tools/smoke/serve_contract.py").is_file())


class LaneSpecificationTests(unittest.TestCase):
    def test_every_lane_declares_a_consistent_identity(self) -> None:
        self.assertGreaterEqual(len(LANES), 2)
        models = set()
        for lane_dir in LANES:
            lane = lane_dir.name
            with self.subTest(lane=lane):
                spec = json.loads((lane_dir / "release-spec.json").read_text(encoding="utf-8"))
                config = json.loads((lane_dir / "server-config.json").read_text(encoding="utf-8"))
                self.assertRegex(lane, r"^rtx[0-9]{4}$")
                number = lane[3:]
                self.assertEqual(spec["artifact_type"], "ninfer_windows_release_spec")
                self.assertEqual(spec["schema_version"], 3)
                self.assertEqual(spec["lane"], lane)
                version = spec["release_version"]
                self.assertEqual(spec["release_id"], f"qwen38-{number}-native-v{version}")
                self.assertEqual(spec["deployment_profile"], spec["release_id"])
                self.assertEqual(spec["build_profile"], f"native-v{version}-{lane}")
                self.assertTrue(spec["platform"].endswith(f"-{lane}"))
                self.assertEqual(spec["product_prefix"], f"ninfer-{lane}-native")
                gpu = spec["gpu"]
                self.assertEqual(gpu["cuda_architecture"], "sm_" + gpu["cmake_cuda_architecture"])
                self.assertEqual(gpu["compute_capability"].replace(".", ""), gpu["cmake_cuda_architecture"])
                self.assertIn(number, gpu["name"])
                self.assertEqual(spec["lifecycle"]["task_name"], f"NInfer-Qwen38-{number}-Native")
                self.assertEqual(spec["lifecycle"]["state_root_name"], f"qwen38-{number}-native")
                owner = spec["lifecycle"]["gpu_owner_controller_protocol"]
                self.assertGreater(owner["owner_power_limit_w"], 0)
                low, high = owner["prior_power_limit_range_w"]
                self.assertTrue(low <= owner["owner_power_limit_w"] <= high)
                if owner["qualified_power_limit_w"] is not None:
                    self.assertLess(owner["qualified_power_limit_w"], owner["owner_power_limit_w"])
                    self.assertGreaterEqual(owner["managed_power_envelope_w"], owner["qualified_power_limit_w"])
                self.assertTrue(is_ancestor(spec["source"]["upstream_base_sha"]))
                self.assertTrue(is_ancestor(spec["source"]["lineage_base_sha"]))
                models.add(json.dumps(spec["model"], sort_keys=True))

                self.assertEqual(config["schema_version"], 3)
                self.assertEqual(config["release_id"], spec["release_id"])
                self.assertEqual(config["deployment_profile"], spec["deployment_profile"])
                self.assertEqual(config["listen"]["host"], "127.0.0.1")
                self.assertEqual(config["listen"]["port"], spec["lifecycle"]["listen_port"])
                self.assertEqual(config["authentication"]["mode"], "required-api-key-file")
                self.assertEqual(config["engine"]["max_concurrency"], 1)
                self.assertEqual(config["engine"]["kv_dtype"], "int8")
                self.assertEqual(config["speculative"]["backend"], "mtp")
                self.assertTrue(config["session_checkpoint"]["enabled"])
                self.assertGreaterEqual(config["context_cache"]["device_state_slots"], 1)
                self.assertNotIn("persistent_cache", config)
                self.assertNotIn("effort", config["reasoning"])
        self.assertEqual(len(models), 1, "every lane pins the same model artifact")


if __name__ == "__main__":
    unittest.main()
