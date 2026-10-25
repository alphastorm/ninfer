"""Contracts of the shared native Windows lifecycle tree and its lane specifications."""

from __future__ import annotations

import copy
from decimal import Decimal
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from tools.release import package as release_package
from tools.qualification import qualify_native

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


GPU_SCAN_FILES = tuple(f"packaging/windows/{name}" for name in SHARED_SCRIPTS) + (
    "tools/qualification/qualify_native.py",
)
# Only selection may enumerate. Exact query and enclosing function are both pinned, and
# each exception can be used once: adding another unindexed probe cannot reuse a waiver.
GPU_ENUMERATIONS = {
    ("packaging/windows/Install-Release.ps1", "Assert-HostPrerequisites"):
        "--query-gpu=index,uuid,name,pci.bus_id,compute_cap,driver_version",
    ("packaging/windows/Control-GpuOwner.ps1", "Resolve-OwnerGpu"):
        "--query-gpu=uuid,pci.bus_id",
    ("tools/qualification/qualify_native.py", "gpu_uuid"):
        "--query-gpu=index,uuid,name,pci.bus_id,compute_cap,driver_version",
}


def unindexed_gpu_commands(sources: dict[str, str]) -> list[str]:
    """Inspect literal GPU queries/sets, including PowerShell embedded in Python strings.

    Match the operation, not a particular executable spelling, so a new wrapper or absolute
    nvidia-smi path gets the same fail-closed rule. Multiline argument arrays and backtick
    continuations are logical statements; all other statements must carry their own selector.
    """
    violations = []
    used_enumerations = set()
    operation = re.compile(r"--query-[a-z-]+(?:=[a-z0-9_.,]+)?|(?<![\w-])-pl\b|--power-limit\b|(?<![\w-])-q\b")
    for path, original in sources.items():
        text = re.sub(r"\x60\r?\n\s*", " ", original)
        text = re.sub(r"Invoke-NvidiaSmi\s+@\([^)]*\)",
                      lambda match: match[0].replace("\n", " "), text)
        function = ""
        for number, line in enumerate(text.splitlines(), 1):
            definition = re.match(r"\s*(?:function\s+([\w-]+)|def\s+(\w+)\()", line)
            if definition:
                function = definition[1] or definition[2]
            if line.lstrip().startswith("#"):
                continue
            for statement in line.split(";"):
                for match in operation.finditer(statement):
                    if match[0] == "-q" and not re.search(r"nvidia[-_]?smi|NvidiaSmi", statement, re.I):
                        continue  # e.g. scp -q, not an NVIDIA query.
                    if re.search(r"(?<![\w-])(?:-i|--id)(?:['\"]?\s*[,=]|\s+)", statement):
                        continue
                    key = (path, function)
                    if GPU_ENUMERATIONS.get(key) == match[0] and key not in used_enumerations:
                        used_enumerations.add(key)
                        continue
                    violations.append(f"{path}:{number}: {match[0]} in {function}")
    return violations


def ps_function(text: str, name: str) -> str:
    start = text.index(f"function {name}")
    end = text.find("\nfunction ", start + 1)
    return text[start:end if end >= 0 else len(text)]


class MultiGpuHostTests(unittest.TestCase):
    def test_no_unindexed_gpu_queries_or_sets(self) -> None:
        sources = {path: (ROOT / path).read_text(encoding="utf-8") for path in GPU_SCAN_FILES}
        self.assertEqual(unindexed_gpu_commands(sources), [])

    def test_invariant_is_red_on_f08309da(self) -> None:
        sources = {path: subprocess.run(
            ["git", "show", f"f08309da:{path}"], cwd=ROOT, capture_output=True,
            text=True, check=True,
        ).stdout for path in GPU_SCAN_FILES}
        violations = unindexed_gpu_commands(sources)
        for name in ("Control-Release.ps1", "Control-GpuOwner.ps1", "qualify_native.py"):
            self.assertTrue(any(name in item for item in violations), violations)
        with self.assertRaises(AssertionError):
            self.assertEqual(violations, [], "baseline must fail the same executable invariant")

    def test_invariant_detects_multiline_and_chained_regressions(self) -> None:
        path = "packaging/windows/Control-GpuOwner.ps1"
        for command in (
            "Invoke-NvidiaSmi @(\n '--query-gpu=power.limit',\n '--format=csv,noheader')",
            "& $absolutePath -pl 300", "nvidia-smi.exe -q -x",
            "nvidia-smi.exe -i $uuid --query-gpu=name; nvidia-smi.exe --query-gpu=power.limit",
        ):
            with self.subTest(command=command):
                self.assertTrue(unindexed_gpu_commands({path: command}))
        self.assertEqual(unindexed_gpu_commands({path: "Invoke-NvidiaSmi @(\n '-i', $uuid,\n '-pl', '300')"}), [])

    def test_selection_matches_lane_and_requires_unambiguous_uuid(self) -> None:
        a = "GPU-00000000-0000-4000-8000-000000004090"
        b = "GPU-00000000-0000-4000-8000-000000003090"
        rows = [f"0, {a}, NVIDIA GeForce RTX 4090, 00000000:17:00.0, 8.9, 580.97",
                f"1, {b}, NVIDIA GeForce RTX 3090, 00000000:65:00.0, 8.6, 580.97"]
        spec = json.loads((WINDOWS / "lanes/rtx3090/release-spec.json").read_text())
        self.assertEqual(qualify_native.select_qualified_gpu(rows, spec, None)["uuid"], b)
        self.assertEqual(qualify_native.select_qualified_gpu(rows[::-1], spec, b.upper())["index"], 1)
        for candidates, requested in (([], None), (rows, a), (rows * 2, None),
                                      ([rows[1].replace("8.6", "8.9")], b)):
            with self.subTest(rows=candidates, requested=requested), self.assertRaises(qualify_native.LaneError):
                qualify_native.select_qualified_gpu(candidates, spec, requested)
        duplicate = rows + [rows[1].replace(b, a)]
        self.assertEqual(qualify_native.select_qualified_gpu(duplicate, spec, b)["uuid"], b)
        with self.assertRaises(qualify_native.argparse.ArgumentTypeError):
            qualify_native.gpu_uuid_argument("GPU-0")
        installer = (WINDOWS / "Install-Release.ps1").read_text()
        selection = ps_function(installer, "Assert-HostPrerequisites")
        for guard in ("$qualified.Count -eq 0", "$qualified.Count -ne 1", "$Spec.gpu.name",
                      "$Spec.gpu.compute_capability", "$GpuUuid", "Env:CUDA_VISIBLE_DEVICES"):
            self.assertIn(guard, selection)
        self.assertNotIn("$Config.engine.device", selection)
        self.assertIn("gpu_pci_bus_id = [string]$selectedGpu.pci_bus_id", installer)
        self.assertIn("PSObject.Properties['gpu_pci_bus_id']", ps_function(installer, "Copy-InstalledRelease"))

    def test_launch_uses_current_index_and_child_only_device_order(self) -> None:
        controller = (WINDOWS / "Control-Release.ps1").read_text()
        identity = ps_function(controller, "Assert-SelectedGpuIdentity")
        for guard in ("-i ([string]$Release.gpu_uuid)", "$rows.Count -ne 1", "return $rowIndex",
                      "PSObject.Properties['gpu_pci_bus_id']", "$parts[2] -cne [string]$Release.gpu_name"):
            self.assertIn(guard, identity)
        self.assertNotIn("$Release.gpu_index", identity)
        run = ps_function(controller, "Invoke-Run")
        self.assertIn("$resolvedGpuIndex = Assert-SelectedGpuIdentity $release", run)
        self.assertIn("'--device', [string]$resolvedGpuIndex", run)
        self.assertNotIn("$config.engine.device", run)
        self.assertIn("$startInfo.EnvironmentVariables['CUDA_DEVICE_ORDER'] = 'PCI_BUS_ID'", run)
        self.assertNotRegex(controller, r"\$env:CUDA_DEVICE_ORDER\s*=")
        for name in ("Start-ManagedRelease", "Invoke-Run"):
            body = ps_function(controller, name)
            self.assertLess(body.index("Get-NetTCPConnection"), body.index("Acquire-GpuOwnerLease"))

    def test_harness_scopes_install_pressure_and_benchmark(self) -> None:
        harness = (ROOT / "tools/qualification/qualify_native.py").read_text()
        for contract in ("--gpu-uuid", "-GpuUuid {ps_quote(self.gpu_uuid)}",
                         "GpuUuid={ps_quote(self.gpu_uuid)}", "'--device',[string]$gpuIndex",
                         "EnvironmentVariables['CUDA_DEVICE_ORDER']='PCI_BUS_ID'",
                         "pressure server_start GPU UUID mismatch", 'gpu_metrics(release["gpu_uuid"])'):
            self.assertIn(contract, harness)
        self.assertNotIn("$c.engine.device", harness)
        self.assertNotRegex(harness, r"\$env:CUDA_DEVICE_ORDER\s*=")
        self.assertIn("Test-ScopedControllerOrQuiescent", harness)
        self.assertIn("unscoped qualification checkpoints cannot be reused", harness)
        self.assertNotIn("Stop-ScheduledTask", harness)
        captured = []
        subject = SimpleNamespace(
            state_root="C:/state", target_root="C:/qualification", gpu_uuid="GPU-00000000-0000-0000-0000-000000000001",
            lane=SimpleNamespace(model_id="model", spec=json.loads(
                (WINDOWS / "lanes/rtx3090/release-spec.json").read_text())),
            remote_json=lambda host, script, **kwargs: captured.append(script) or {},
            config=SimpleNamespace(target="not-contacted"),
        )
        qualify_native.Orchestrator.pressure_protocol(subject)
        self.assertIn(r"""'(\\*)"'""", captured[0])
        self.assertIn(r"'$1$1\"'", captured[0])

    def test_read_only_start_refusals_precede_gpu_owner_acquisition(self) -> None:
        controller = (WINDOWS / "Control-Release.ps1").read_text()
        identity = ps_function(controller, "Assert-SelectedGpuIdentity")
        self.assertIn("Test-Path Env:CUDA_VISIBLE_DEVICES", identity)
        for name in ("Start-ManagedRelease", "Invoke-Run"):
            with self.subTest(function=name):
                body = ps_function(controller, name)
                acquire = body.index("Acquire-GpuOwnerLease")
                for refusal in ("Get-State", "Get-Release", "Assert-NoPreparedRelease",
                                "Assert-CandidateLayout", "Assert-InstalledModelIdentity",
                                "Assert-SelectedGpuIdentity", "Get-NetTCPConnection"):
                    self.assertLess(body.index(refusal), acquire, refusal)
                hashes = list(re.finditer(r"Assert-FileHash[^\n]+", body))
                self.assertGreaterEqual(len(hashes), 4)
                for check in hashes:
                    self.assertLess(check.start(), acquire, check[0])
                for field in ("ninfer_sha256", "binary_sha256", "benchmark_sha256", "config_sha256"):
                    self.assertTrue(any(field in check[0] for check in hashes), field)

    def test_existing_owner_lease_preserves_implicit_uuid_selection(self) -> None:
        owner = (WINDOWS / "Control-GpuOwner.ps1").read_text()
        capture = "$statusParameters = @{} + $PSBoundParameters"
        self.assertLess(owner.index(capture), owner.index("\nResolve-OwnerGpu\n"))
        self.assertIn("$statusParameters['Action'] = 'status'", owner)
        self.assertIn("& $PSCommandPath @statusParameters", owner)
        self.assertNotRegex(owner, r"\$statusParameters\['GpuUuid'\]\s*=")
        self.assertNotRegex(owner, r"& \$PSCommandPath[^\n]*-GpuUuid")
        self.assertIn("$null -eq $bound -and -not $singleGpuSelection", ps_function(owner, "Read-State"))

    def test_gpu_metrics_never_selects_the_first_host_row(self) -> None:
        uuid = "GPU-00000000-0000-0000-0000-000000000001"
        with patch.object(qualify_native, "run", return_value=SimpleNamespace(stdout="100.5, 50, 99, 12000\n")) as run:
            self.assertEqual(qualify_native.gpu_metrics(uuid), (100.5, 50, 99, 12000))
            self.assertEqual(run.call_args.args[0][1:3], ["-i", uuid])
        for output in ("", "100, 50, 99, 12000\n200, 40, 0, 12\n"):
            with patch.object(qualify_native, "run", return_value=SimpleNamespace(stdout=output)), self.assertRaises(qualify_native.LaneError):
                qualify_native.gpu_metrics(uuid)

    def test_predecessors_emit_attributable_startup_gpu_identity(self) -> None:
        for commit in ("075d442e", "03f56e58"):
            with self.subTest(commit=commit):
                source = subprocess.run(["git", "show", f"{commit}:src/serve/request_log.cpp"],
                                        cwd=ROOT, capture_output=True, text=True, check=True).stdout
                for marker in ('"server_start"', '"server_instance_id"', '"gpu_uuid"',
                               '"serve-"', 'core::process_id()'):
                    self.assertIn(marker, source)

    def test_current_launch_attestation_precedes_every_ready_path(self) -> None:
        controller = (WINDOWS / "Control-Release.ps1").read_text()
        attestation = ps_function(controller, "Test-ServerStartGpu")
        self.assertIn("'server_start'", attestation)
        self.assertIn("PSObject.Properties['gpu_uuid']", attestation)
        self.assertIn("[string]$uuid.Value -cne $ExpectedUuid", attestation)
        run = ps_function(controller, "Invoke-Run")
        self.assertIn("foreach ($log in @($stdout, $stderr))", run)
        self.assertNotIn("@($stdout, $stderr, $requestLog)", run)
        self.assertLess(run.index("$requestLogOffset = [Int64]$logItem.Length"), run.index("[Diagnostics.Process]::Start"))
        for guard in ("$stream.Seek($StartOffset", "$stream.Length -lt $StartOffset",
                      "$CreationTicks", "$text.LastIndexOf([char]10)", "^serve-$ServerPid-", "1MB"):
            self.assertIn(guard, attestation)
        self.assertLess(run.index("Test-ServerStartGpu $requestLog"), run.index("$runtimeRecord.gpu_uuid_verified = $true"))
        self.assertIn("UtcNow.AddSeconds(600)", run)
        self.assertIn("$process.Kill()", run)
        for name in ("Wait-Ready", "Get-StatusObject", "Start-ManagedRelease"):
            self.assertIn("Assert-RuntimeGpuReady $release", ps_function(controller, name))

    def test_owner_refuses_ambiguity_and_scopes_counter_luids(self) -> None:
        owner = (WINDOWS / "Control-GpuOwner.ps1").read_text()
        self.assertIn("$rows.Count -ne 1", ps_function(owner, "Resolve-OwnerGpu"))
        count = ps_function(owner, "Get-InteractiveGpuWorkloadCount")
        for guard in ("Get-AdapterPciAddress $high $low", "$addresses[$luid] -ceq $gpuPciAddress",
                      "$selectedLuids.Count -eq 0", "$ownerPid = [int]$Matches[1]",
                      "Win32_PerfFormattedData_GPUPerformanceCounters_GPUAdapterMemory",
                      "$selectedLuids.Contains($luid)"):
            self.assertIn(guard, count)
        self.assertNotIn("$observed", count)
        self.assertIn("'NInfer.GpuAdapterAddress' -as [type]", owner)
        self.assertIn("Type = 6", owner)
        self.assertIn("D3DKMTCloseAdapter(ref close)", owner)
        self.assertIn("PSObject.Properties['gpu_uuid']", ps_function(owner, "Read-State"))
        self.assertIn("PSObject.Properties['gpu_pci_bus_id']", ps_function(owner, "Read-State"))
        self.assertIn("$state.prior_power_limit_w -gt $PriorPowerLimitMaxW", ps_function(owner, "Read-State"))
        installer = (WINDOWS / "Install-Release.ps1").read_text()
        self.assertNotIn("$oldState.gpu_owner.controller_path", installer)
        self.assertIn("Parameters.ContainsKey('GpuUuid')", installer)
        self.assertIn("legacy unscoped GPU-owner wrapper is running", installer)
        stop = installer.index("if ($oldTaskWasRunning) {", installer.index("$state = [ordered]@{"))
        self.assertLess(stop, installer.index("Copy-Item -LiteralPath $ownerControllerSource"))
        controller = (WINDOWS / "Control-Release.ps1").read_text()
        self.assertIn("Parameters.ContainsKey('GpuUuid')", ps_function(controller, "Get-GpuOwner"))
        self.assertIn("GpuUuid = [string]$release.gpu_uuid", ps_function(controller, "Invoke-GpuOwner"))


class QualificationRestorationTests(unittest.TestCase):
    def orchestrator(self, directory: Path) -> qualify_native.Orchestrator:
        subject = qualify_native.Orchestrator.__new__(qualify_native.Orchestrator)
        fixture = directory / "fixture.json"
        fixture.write_text("{}", encoding="utf-8")
        subject.config = SimpleNamespace(
            source_root=directory, state_dir=directory, long_fixture=fixture,
            incumbent_state_root="C:/incumbent", target="fake-target", builder="fake-builder",
            model_path="C:/model", gpu_uuid=None, resume=False, omp_root="C:/omp-fixture",
        )
        subject.head = subject.tool_head = "source"
        subject.lane = SimpleNamespace(
            name="rtx3090", release_id="candidate", state_root="C:/candidate",
            task_name="candidate-task", owner_power_limit_w=370, model_sha256="model-sha",
            model_id="q38-ninfer", spec={"gpu": {"name": "NVIDIA GeForce RTX 3090"}},
        )
        subject.state_path = directory / "state.json"
        subject.receipt_dir = directory / "receipts"
        subject.state = {
            "gpu_uuid": "GPU-00000000-0000-4000-8000-000000003090",
            "phases": {}, "restoration": {"status": "required"},
        }
        return subject

    def test_preflight_found_state_is_durable_before_stop_and_survives_failure(self) -> None:
        for was_running, pending_restart in ((True, False), (False, True)):
            with self.subTest(was_running=was_running, pending_restart=pending_restart), tempfile.TemporaryDirectory() as temporary:
                subject = self.orchestrator(Path(temporary))
                if pending_restart:
                    subject.state["found_incumbent"] = {
                        "incumbent_state_root": "C:/incumbent", "incumbent_was_running": True,
                    }
                # A previous window's successful restore cannot exempt this new window from cleanup.
                subject.state["phases"]["restore"] = {"status": "passed"}
                scripts = []

                def remote(host: str, script: str, **kwargs: object) -> dict:
                    scripts.append(script)
                    if len(scripts) == 1:
                        self.assertNotIn("-Action Stop", script)
                        return {"incumbent_state_root": "C:/incumbent", "incumbent_was_running": was_running,
                                "omp_models": ["ninfer-client-acceptance/q38-ninfer"]}
                    if len(scripts) == 2:
                        durable = json.loads(subject.state_path.read_text())
                        self.assertTrue(durable["found_incumbent"]["incumbent_was_running"])
                        self.assertEqual(durable["found_incumbent"]["incumbent_state_root"], "C:/incumbent")
                        self.assertNotIn("restore", durable["phases"])
                        self.assertIn("-Action Stop", script)
                        raise qualify_native.LaneError("target refused after incumbent stop")
                    self.assertIn("$incumbentRoot='C:/incumbent'", script)
                    self.assertIn("if($true){if(-not (Test-ScopedControllerOrQuiescent $incumbentRoot))", script)
                    return {"status": "passed", "process_count": 1}

                subject.remote_json = remote
                with patch.object(qualify_native, "run", return_value=SimpleNamespace(stdout="")):
                    with self.assertRaisesRegex(qualify_native.LaneError, "target refused"):
                        subject.phase("preflight", subject.preflight)
                self.assertEqual(subject.state["phases"]["preflight"]["status"], "failed")
                subject.config.incumbent_state_root = "C:/not-the-recorded-root"
                subject.cleanup()
                self.assertEqual(subject.state["restoration"]["status"], "passed")
                self.assertEqual(len(scripts), 3)

    def test_preflight_receipt_hides_operator_paths_and_host_aliases(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            subject = self.orchestrator(Path(temporary))
            found = {"incumbent_state_root": "C:/incumbent", "incumbent_was_running": False,
                     "omp_models": ["ninfer-client-acceptance/q38-ninfer"]}
            status = {
                "lineage_exists": False, "active_release": "", "previous_release": "",
                "incumbent_state_root": "C:/incumbent", "incumbent_was_running": False,
                "power_limit_w": 370, "gpu": "NVIDIA GeForce RTX 3090, 24576, 621.01",
                "model_sha256": "",
            }
            with patch.object(qualify_native, "run", return_value=SimpleNamespace(stdout="")), \
                    patch.object(subject, "remote_json", side_effect=[found, status]):
                receipt = subject.phase("preflight", subject.preflight)
            text = json.dumps(receipt)
            for private in ("fake-builder", "fake-target", "C:/incumbent"):
                self.assertNotIn(private, text)
            for field in ("builder_identity_sha256", "target_identity_sha256", "incumbent_root_sha256"):
                self.assertRegex(receipt[field], "^[0-9a-f]{64}$")
            self.assertEqual(subject.state["found_incumbent"]["incumbent_state_root"], "C:/incumbent")

    def test_failed_found_state_write_never_reaches_stop(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            subject = self.orchestrator(Path(temporary))
            with patch.object(qualify_native, "run", return_value=SimpleNamespace(stdout="")), \
                    patch.object(subject, "remote_json", return_value={
                        "incumbent_state_root": "C:/incumbent", "incumbent_was_running": True,
                        "omp_models": ["ninfer-client-acceptance/q38-ninfer"],
                    }) as remote, patch.object(subject, "_save", side_effect=OSError("cannot persist")):
                with self.assertRaisesRegex(OSError, "cannot persist"):
                    subject.preflight()
                self.assertEqual(remote.call_count, 1)
                self.assertNotIn("-Action Stop", remote.call_args.args[1])

    def test_omp_fixture_without_the_lane_model_refuses_before_anything_stops(self) -> None:
        # Another lane's model in the shared client fixture is not this lane's: the omp phase would fail late.
        with tempfile.TemporaryDirectory() as temporary:
            subject = self.orchestrator(Path(temporary))
            subject.lane.model_id = "qwen3.8-27b"
            with patch.object(qualify_native, "run", return_value=SimpleNamespace(stdout="")), \
                    patch.object(subject, "remote_json", return_value={
                        "incumbent_state_root": "C:/incumbent", "incumbent_was_running": True,
                        "omp_models": ["ninfer-client-acceptance/q38-ninfer"],
                    }) as remote, patch.object(subject, "_save") as save:
                with self.assertRaisesRegex(qualify_native.LaneError,
                                            "does not declare ninfer-client-acceptance/qwen3.8-27b"):
                    subject.preflight()
            self.assertEqual(remote.call_count, 1)
            self.assertNotIn("-Action Stop", remote.call_args.args[1])
            save.assert_not_called()
            self.assertNotIn("found_incumbent", subject.state)

    def test_resume_always_reruns_preflight(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            subject = self.orchestrator(Path(temporary))
            subject.config.resume = True
            subject.state["phases"]["preflight"] = {"status": "passed", "receipt": {"old": True}}
            self.assertFalse(subject.verify_checkpoint("preflight", {"old": True}))
            with patch.object(subject, "preflight", return_value={"fresh": True}) as action:
                self.assertEqual(subject.phase("preflight", action), {"fresh": True})
                action.assert_called_once_with()

    def ready_for_receipt(self, directory: Path) -> qualify_native.Orchestrator:
        subject = self.orchestrator(directory)
        subject.lane.spec["deployment_profile"] = "candidate-profile"
        subject.lane.spec["gpu"]["cuda_architecture"] = "sm_86"
        subject.state["phases"] = {
            name: {"status": "passed", "receipt": {}, "receipt_sha256": "fixture"}
            for name in qualify_native.PHASES if name not in {"receipt", "restore"}
        }
        receipts = {
            "context_128k": {"prompt_tokens": 130048, "exact_output": "answer", "elapsed_seconds": 1},
            "restart": {
                "cached_input_tokens": 100, "old_pid": 1, "new_pid": 2,
                "managed_stop": {"mode": "stop-event", "outcome": "graceful", "forced": False},
                "managed_stop_flush": {"exact_output": qualify_native.FLUSH_MARKER, "cached_input_tokens": 100},
            },
            "benchmark_c1": {
                "decode_tokens_per_second": 1, "prefill_tokens_per_second": 1, "max_power_w": 300,
                "max_memory_used_mib": 1, "mtp_acceptance_percent": 1,
            },
            "package": {
                "package": {}, "build_profile": "fixture", "upstream_base_sha": "fixture",
                "lineage_base_sha": "fixture", "model_sha256": "fixture",
                "binaries": {"server_sha256": "fixture"}, "config_sha256": "fixture", "support_assets": {},
            },
            "protocol": {"checks": 1}, "rollback": {"status": "passed"},
            "security": {"low_privilege_read_denials": 1}, "omp": {"exact_final_answer": True},
        }
        for name, receipt in receipts.items():
            subject.state["phases"][name]["receipt"] = receipt
        return subject

    def test_summary_passes_only_after_restore_and_receipt_hashes_follow_it(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            subject = self.ready_for_receipt(Path(temporary))
            pending = subject.phase("receipt", subject.receipt)
            summary_path = Path(temporary) / "qualification-summary.json"
            self.assertEqual(pending["status"], "restore-pending")
            self.assertEqual(json.loads(summary_path.read_text())["status"], "restore-pending")
            self.assertIsNone(pending["qualified_utc"])

            def restored(host: str, script: str, **kwargs: object) -> dict:
                self.assertEqual(json.loads(summary_path.read_text())["status"], "restore-pending")
                return {"status": "passed", "process_count": 0}

            subject.remote_json = restored
            subject.phase("restore", subject.restore)
            final = json.loads(summary_path.read_text())
            self.assertEqual(final["status"], "passed")
            self.assertEqual(final["restoration"]["status"], "passed")
            self.assertIsNotNone(final["qualified_utc"])
            receipt_phase = subject.state["phases"]["receipt"]
            self.assertEqual(receipt_phase["receipt"]["receipt_sha256"], qualify_native.sha256_file(summary_path))
            self.assertEqual(receipt_phase["receipt_sha256"], qualify_native.sha256_file(subject.receipt_dir / "receipt.json"))

    def test_failed_restore_or_cleanup_never_leaves_a_passed_summary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            subject = self.ready_for_receipt(Path(temporary))
            subject.phase("receipt", subject.receipt)
            summary_path = Path(temporary) / "qualification-summary.json"
            with patch.object(subject, "remote_json", return_value={"status": "passed", "process_count": 0}):
                subject.phase("restore", subject.restore)
            self.assertEqual(json.loads(summary_path.read_text())["status"], "passed")

            def failed(host: str, script: str, **kwargs: object) -> dict:
                self.assertEqual(json.loads(summary_path.read_text())["status"], "restore-pending")
                raise qualify_native.LaneError("power was not restored")

            subject.remote_json = failed
            with self.assertRaisesRegex(qualify_native.LaneError, "power was not restored"):
                subject.phase("restore", subject.restore)
            self.assertEqual(json.loads(summary_path.read_text())["status"], "restore-failed")
            subject.cleanup()
            final = json.loads(summary_path.read_text())
            self.assertEqual(final["status"], "restore-failed")
            self.assertIsNone(final["qualified_utc"])
            self.assertEqual(subject.state["restoration"]["status"], "failed")
            self.assertNotIn("error", final["restoration"])
            self.assertEqual(final["restoration"]["reason"], "operator_failure")
            self.assertRegex(final["restoration"]["error_sha256"], "^[0-9a-f]{64}$")

    def test_successful_cleanup_cannot_mask_failed_preflight_in_an_old_summary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            subject = self.ready_for_receipt(Path(temporary))
            subject.phase("receipt", subject.receipt)
            subject.state["phases"]["preflight"] = {"status": "failed"}
            with patch.object(subject, "remote_json", return_value={"status": "passed", "process_count": 0}):
                subject.cleanup()
            summary = json.loads((Path(temporary) / "qualification-summary.json").read_text())
            self.assertEqual(summary["status"], "failed")
            self.assertIsNone(summary["qualified_utc"])


class SharedLifecycleTreeTests(unittest.TestCase):
    def test_controller_server_options_match_runtime(self) -> None:
        controller = (WINDOWS / "Control-Release.ps1").read_text(encoding="utf-8")
        start = controller.index("$serverArguments =")
        end = controller.index("$argumentLine =", start)
        context = ps_function(controller, "Add-ContextArguments")
        current_context = context[:context.index("elseif ([int]$Config.schema_version -eq 3)")]
        controller_flags = set(re.findall(r"'(--[a-z0-9-]+)'", controller[start:end] + current_context))
        parser = (ROOT / "src/serve/serve_options.cpp").read_text(encoding="utf-8")
        runtime_flags = set(re.findall(r'arg == "(--[a-z0-9-]+)"', parser))
        self.assertEqual(controller_flags - runtime_flags, set())
        self.assertTrue({
            "--api-key-file", "--binary-sha256", "--artifact-sha256", "--config-sha256",
            "--deployment-profile", "--device-state-slots", "--host-context-mib",
            "--session-checkpoint-dir",
            "--session-checkpoint-quota-mib", "--session-checkpoint-staging-mib",
        }.issubset(controller_flags))
        # Lane-branch options mainline never had.
        self.assertTrue({"--reasoning-effort", "--disk-cache", "--prompt-cache", "--no-ui",
                         "--wddm-evictable-budget", "--host-state-slots", "--host-kv-mib",
                         "--max-private-continuations"}.isdisjoint(controller_flags))

    def test_flags_newer_than_the_shipped_lineage_are_capability_gated(self) -> None:
        """The shared controller launches every installed release, including the shipped v0.6.0
        one after a rollback, and an older ninfer-serve.exe refuses an argument it never learned
        (`unknown argument`) and does not start. So a flag the runtime gained after that release
        may reach a server only inside the branch that checks the release's declared capability:
        the managed-stop plan for --stop-event and --shutdown-report (measured twice on the RTX
        4090 host), and a positive engine.gpu_keep_warm_ms in the release's own packaged config
        for --gpu-keep-warm-ms - a config packaged before the field existed reads as off."""
        shipped_parser = subprocess.run(
            ["git", "show", "075d442e:src/serve/serve_options.cpp"],
            capture_output=True, text=True, check=True, cwd=ROOT,
        ).stdout
        shipped_flags = set(re.findall(r'arg == "(--[a-z0-9-]+)"', shipped_parser))
        parser = (ROOT / "src/serve/serve_options.cpp").read_text(encoding="utf-8")
        runtime_flags = set(re.findall(r'arg == "(--[a-z0-9-]+)"', parser))
        gates = {
            "--stop-event": "if ([string]$stopPlan.mode -ceq 'stop-event') {",
            "--shutdown-report": "if ([string]$stopPlan.mode -ceq 'stop-event') {",
            "--gpu-keep-warm-ms": "if ($gpuKeepWarmMs -gt 0) {",
        }
        controller = (WINDOWS / "Control-Release.ps1").read_text(encoding="utf-8")
        start = controller.index("$serverArguments =")
        end = controller.index("$argumentLine =", start)
        launch = controller[start:end]
        newer_flags = set(re.findall(r"'(--[a-z0-9-]+)'", launch)) - shipped_flags
        self.assertEqual(newer_flags, set(gates))
        self.assertTrue(set(gates).issubset(runtime_flags))
        for flag, opener in gates.items():
            with self.subTest(flag=flag):
                gated = launch[launch.index(opener):]
                gated = gated[:gated.index("\n        }\n")]
                self.assertEqual(launch.count(f"'{flag}'"), 1)
                self.assertIn(f"'{flag}'", gated)
        self.assertEqual(launch.count("$gpuKeepWarmMs = Get-GpuKeepWarmMs $config"), 1)
        reader = controller[controller.index("function Get-GpuKeepWarmMs"):]
        reader = reader[:reader.index("\n}\n")]
        self.assertIn("if ($null -eq $property) { return 0 }", reader)

    def test_config_fields_newer_than_the_shipped_lineage_are_read_as_optional(self) -> None:
        """The controller runs under strict mode, where reading a field a JSON object lacks
        throws, and a rollback hands the launch an older release's own packaged config. The RTX
        3090's installed v0.6.0 config predates context_cache.host_kv_mib: a direct read killed
        the managed wrapper before it launched v0.6.0, and the 2026-10-01 RTX 3090 qualification
        failed its rollback. Every field the launch reads directly must exist in each shipped
        lane config; a newer field goes through a reader that treats absence as undeclared."""
        controller = (WINDOWS / "Control-Release.ps1").read_text(encoding="utf-8")
        start = controller.index("$config = Read-JsonFile")
        end = controller.index("$argumentLine =", start)
        direct = set(re.findall(r"\$config((?:\.[a-z_]+)+)", controller[start:end]))
        self.assertNotIn(".context_cache.host_state_slots", direct)
        self.assertNotIn(".context_cache.host_kv_mib", direct)
        for commit in ("075d442e", "03f56e58"):
            for lane in ("rtx3090", "rtx4090"):
                shipped = json.loads(subprocess.run(
                    ["git", "show", f"{commit}:packaging/windows/lanes/{lane}/server-config.json"],
                    capture_output=True, text=True, check=True, cwd=ROOT,
                ).stdout)
                for path in sorted(direct):
                    with self.subTest(commit=commit, lane=lane, field=path):
                        node = shipped
                        for key in path[1:].split("."):
                            self.assertIn(key, node)
                            node = node[key]
        body = ps_function(controller, "Get-GpuKeepWarmMs")
        self.assertIn("PSObject.Properties['gpu_keep_warm_ms']", body)
        self.assertIn("if ($null -eq $property) { return", body)
        self.assertEqual(controller[start:end].count("Get-GpuKeepWarmMs $config"), 1)
        context = ps_function(controller, "Add-ContextArguments")
        current, legacy = context.split("elseif ([int]$Config.schema_version -eq 3)")
        self.assertIn("if ([int]$Config.schema_version -eq 4)", current)
        self.assertIn("'--host-context-mib'", current)
        for flag in ("--host-state-slots", "--host-kv-mib", "--max-private-continuations"):
            self.assertNotIn(f"'{flag}'", current)
            self.assertIn(f"'{flag}'", legacy)
        self.assertIn("PSObject.Properties['host_kv_mib']", legacy)
        self.assertIn("if ($null -ne $hostKv)", legacy)
        self.assertEqual(controller[start:end].count("Add-ContextArguments $serverArguments $config"), 1)

    def test_scratch_task_suffix_cannot_select_the_shipped_task_or_product_root(self) -> None:
        installer = (WINDOWS / "Install-Release.ps1").read_text()
        resolve = ps_function(installer, "Resolve-LifecycleTaskName")
        for guard in ("IsNullOrWhiteSpace($Suffix)", "return $name", "'-Scratch-' + $Suffix",
                      "^[A-Za-z0-9][A-Za-z0-9-]{0,31}$", "[IO.Path]::GetFullPath($Root)",
                      "[IO.Path]::GetFullPath($productRoot)", "[StringComparison]::OrdinalIgnoreCase",
                      "scratch task isolation requires a non-product state root"):
            self.assertIn(guard, resolve)
        install = installer[installer.index("$taskName = Resolve-LifecycleTaskName"):]
        self.assertLess(install.index("$ScratchTaskSuffix"), install.index("Get-ScheduledTask"))
        self.assertIn("if ($null -eq $oldState -and $null -ne $oldTask)", install)

    def test_new_checkpoint_roots_never_open_predecessor_generations(self) -> None:
        controller = (WINDOWS / "Control-Release.ps1").read_text()
        run = ps_function(controller, "Invoke-Run")
        self.assertIn("if ([int]$config.schema_version -eq 4) { 'session-checkpoints-v3' }", run)
        self.assertIn("else { 'session-checkpoints' }", run)
        self.assertIn("$checkpointRoot = Join-Path $cache $checkpointDirectory", run)
        installer = (WINDOWS / "Install-Release.ps1").read_text()
        self.assertIn("$cacheRoot = Join-Path (Join-Path $StateRoot 'cache') $instanceId", installer)

    def test_managed_stop_is_a_signal_before_a_termination(self) -> None:
        """A managed stop must reach the server's graceful path (which saves live sessions), and
        only terminate a release that cannot receive it."""
        controller = (WINDOWS / "Control-Release.ps1").read_text(encoding="utf-8")
        stop = controller[controller.index("function Stop-ManagedProcess"):]
        stop = stop[:stop.index("\nfunction ")] if "\nfunction " in stop else stop
        self.assertLess(stop.index("Request-ManagedStop"), stop.index("Stop-LaunchNow"),
                        "the stop terminates the launch before it signals the server")
        # Termination lives in one best-effort helper; the stop path must not reach the
        # scheduler or the process any other way.
        self.assertNotIn("Stop-ScheduledTask", stop)
        self.assertNotIn("Stop-Process", stop)
        # The name is attributed to this launch and this release, never taken on trust.
        attribution = controller[controller.index("function Get-RuntimeStopEvent"):]
        for guard in ("ninfer_windows_runtime_state", "schema_version", "release_id",
                      r"^(Global|Local)\\NInfer-Serve-Stop-[0-9a-f]{32}$"):
            self.assertIn(guard, attribution[:attribution.index("\nfunction ")])
        # A release installed before the channel existed has no managed_stop field at all.
        plan = controller[controller.index("function Get-ManagedStopPlan"):]
        self.assertIn("'terminate'", plan[:plan.index("\nfunction ")])
        installer = (WINDOWS / "Install-Release.ps1").read_text(encoding="utf-8")
        self.assertIn("managed_stop = [string]$spec.lifecycle.managed_stop", installer)

    def test_controller_never_touches_state_a_wrapper_still_owns(self) -> None:
        """A terminated launch released nothing, so the controller could always follow straight
        on. A graceful one unwinds: it restores the GPU owner and drops the run lock on its own
        thread of control, so every controller action that touches either must first wait for it
        (alphastorm/ninfer#39)."""
        controller = (WINDOWS / "Control-Release.ps1").read_text(encoding="utf-8")

        def body(name: str) -> str:
            start = controller.index(f"function {name}")
            rest = controller[start + 1:]
            end = rest.index("\nfunction ") if "\nfunction " in rest else len(rest)
            return rest[:end]

        for name in ("Stop-ManagedProcess", "Start-ManagedRelease"):
            with self.subTest(function=name):
                self.assertIn("Wait-ManagedWrapperExit", body(name))
        wait = body("Wait-ManagedWrapperExit")
        self.assertIn("run.lock", wait)
        self.assertIn("Wait-TaskIdle", wait)
        # Both callers converge on the same end state, so the loser must not fail for finding the
        # lease already gone.
        restore = body("Restore-GpuOwnerLease")
        self.assertRegex(restore, r"gpu-owner-lease\.json'\)[^\n]*(\n\s*)?-ErrorAction SilentlyContinue")
        # A process the controller did not start does not always expose an exit code; a receipt
        # that classified on the unobservable value called a clean graceful stop a nonzero exit
        # (alphastorm/ninfer#40). Only an observed code may decide an outcome.
        stop = body("Stop-ManagedProcess")
        self.assertNotRegex(stop, r"\$owned\.ExitCode -ne 0")
        self.assertIn("try { $exitCode = $owned.ExitCode } catch", stop)
        self.assertRegex(stop, r"if \(\$null -ne \$exitCode\)")

    def test_a_stop_is_fail_closed_and_reports_an_unclean_shutdown(self) -> None:
        """A stop must end with no server running whatever fails on the way, must decide from
        state read now rather than the snapshot it opened with, and must not call a shutdown
        that lost live state graceful (alphastorm/ninfer#41)."""
        controller = (WINDOWS / "Control-Release.ps1").read_text(encoding="utf-8")

        def body(name: str) -> str:
            start = controller.index(f"function {name}")
            rest = controller[start + 1:]
            end = rest.index("\nfunction ") if "\nfunction " in rest else len(rest)
            return rest[:end]

        # Errors are terminating in this script, so each termination step needs its own catch or
        # the step after it never runs.
        terminate = body("Stop-LaunchNow")
        self.assertRegex(terminate, r"try \{ Stop-ScheduledTask[^}]*\}\s*\n\s*catch")
        self.assertRegex(terminate, r"try \{\s*\n\s*Stop-Process -Id \$live\.Id -Force")
        stop = body("Stop-ManagedProcess")
        # Fresh reads, not the opening snapshot, decide what still needs stopping.
        self.assertEqual(stop.count("Get-OwnedProcess (Get-RuntimeState) $release"), 2)
        self.assertEqual(terminate.count("Get-OwnedProcess (Get-RuntimeState) $Release"), 2)
        self.assertIn("Stop-LaunchNow $release $taskName $receipt", stop)
        # The receipt records every outcome, including the ones that throw.
        self.assertRegex(stop, r"finally \{\s*\n(\s*.*\n)*?\s*Write-ManagedStopReceipt \$receipt")
        self.assertIn("graceful_incomplete_shutdown", stop)
        # Concurrent mutating actions cannot interleave over the lease.
        for action in ("Start-ManagedRelease", "Stop-ManagedRelease"):
            self.assertIn(f"Invoke-WithActionLock {{ {action}", controller)
        self.assertIn("action.lock", body("Invoke-WithActionLock"))
        # The lock file lives inside the state root, so the uninstall - which deletes that root -
        # must scope it, not hold it to the end (alphastorm/ninfer#41); and stop plus task
        # removal are one step under it, or a Start between them launches a server that
        # outlives its release identity.
        self.assertNotIn("Invoke-WithActionLock { Uninstall-ManagedRelease", controller)
        uninstall = body("Uninstall-ManagedRelease")
        scoped = uninstall[uninstall.index("Invoke-WithActionLock {"):]
        scoped = scoped[:scoped.index("\n        }\n")]
        self.assertIn("Stop-ManagedRelease", scoped)
        self.assertIn("Unregister-ScheduledTask", scoped)
        self.assertLess(uninstall.index("Invoke-WithActionLock {"),
                        uninstall.index("Remove-Item -LiteralPath $fullStateRoot -Recurse -Force"))
        # Delivering the request cannot throw past the fail-closed scope: every failure mode of
        # opening or signalling the event is a return value the stop acts on.
        request = body("Request-ManagedStop")
        self.assertNotIn("throw", request)
        self.assertEqual(request.count("catch"), 3)
        self.assertLess(stop.index("try {"), stop.index("Request-ManagedStop $eventName"))
        # A shutdown report is evidence only when it is this launch's.
        self.assertIn("[string]$report.launch_id -cne $expectedLaunch", stop)
        run = body("Invoke-Run")
        self.assertIn("launch_id = $launchId", run)
        # A launch that ended by request must not be retried by the scheduler.
        self.assertIn("$stoppedByRequest", run)
        self.assertRegex(run, r"if \(-not \$stoppedByRequest\) \{\s*\n\s*throw")

    def test_installer_reconstruction_keeps_the_managed_stop_capability(self) -> None:
        """The installer rewrites every existing release record when a new release is installed.
        A copy that dropped managed_stop would turn the incumbent's next stop - and every later
        rollback to it - into a termination, silently reopening the loss the channel closes."""
        installer = (WINDOWS / "Install-Release.ps1").read_text(encoding="utf-8")
        start = installer.index("function Copy-InstalledRelease")
        copy = installer[start:installer.index("\nfunction ", start + 1)]
        self.assertIn("$copy['managed_stop'] = [string]$declared.Value", copy)
        self.assertIn("$copy['graceful_stop_timeout_seconds'] = [int]$bound.Value", copy)
        # A record that never had the field is a release that predates the channel and stays so.
        self.assertNotIn("$copy['managed_stop'] = 'terminate'", copy)

    def test_the_gpu_owner_lease_has_one_restorer_per_stop(self) -> None:
        """The wrapper and the controller both end a launch. Exactly one of them may restore the
        GPU owner, or the loser fails on work the winner already did - and while a controller
        action holds the action lock, the decision is that action's (alphastorm/ninfer#41)."""
        controller = (WINDOWS / "Control-Release.ps1").read_text(encoding="utf-8")
        run = controller[controller.index("function Invoke-Run"):]
        run = run[:run.index("\nfunction ")]
        self.assertIn("if ($ownerLeaseHeld -and -not (Test-ManagedActionInProgress)) "
                      "{ Restore-GpuOwnerLease }", run)
        probe = controller[controller.index("function Test-ManagedActionInProgress"):]
        self.assertIn("action.lock", probe[:probe.index("\nfunction ")])
        owner = (WINDOWS / "Control-GpuOwner.ps1").read_text(encoding="utf-8")
        self.assertRegex(owner, r"Remove-Item -LiteralPath \$statePath -Force -ErrorAction SilentlyContinue")

    def test_no_lifecycle_decision_reads_an_unreadable_exit_code(self) -> None:
        """Windows hands a parent that redirected a child's streams a handle whose ExitCode
        reads as absent, clean exit or not, and `$null -ne 0` is true - so any comparison
        against it decides on nothing. Measured on the RTX 4090 host; three receipts and one
        wrapper failure came from this (alphastorm/ninfer#40, #41)."""
        for name in SHARED_SCRIPTS:
            text = (WINDOWS / name).read_text(encoding="utf-8")
            with self.subTest(script=name):
                self.assertNotRegex(text, r"\$\w+\.ExitCode -(?:ne|eq|gt|lt) ",
                                    f"{name} decides on an exit code it may not be able to read")
        controller = (WINDOWS / "Control-Release.ps1").read_text(encoding="utf-8")
        # Both readers guard the value and treat its absence as no information.
        self.assertEqual(controller.count("catch { $serverExit = $null }"), 1)
        self.assertEqual(controller.count("catch { $exitCode = $null }"), 1)
        # The outcome comes from the server's own report instead.
        self.assertIn("ninfer_serve_shutdown_report", controller)
        self.assertIn("--shutdown-report", controller)
        self.assertIn("graceful_unreported", controller)
        # A stale report from an earlier launch must never be read as this launch's outcome.
        run = controller[controller.index("function Invoke-Run"):]
        run = run[:run.index("\nfunction ")]
        self.assertLess(run.index("Remove-Item -LiteralPath $shutdownReport"),
                        run.index("[Diagnostics.Process]::Start($startInfo)"))
        # One prior launch's logs survive, because they are what explains a stop.
        self.assertIn('Move-Item -LiteralPath $log -Destination "$log.previous"', run)

    def test_the_server_reports_a_shutdown_that_lost_state(self) -> None:
        """The manager can only record what the server tells it: a flush that could not save a
        live session must fail the exit, not log and return zero."""
        controller = (WINDOWS / "Control-Release.ps1").read_text()
        stop = ps_function(controller, "Stop-ManagedProcess")
        self.assertIn("[int]$report.schema_version -ne 1", stop)
        self.assertIn("$report.complete -isnot [bool]", stop)
        self.assertIn("[int]$report.refused -ne 0", stop)
        self.assertIn("sessions_skipped = $null", stop)
        self.assertIn("$receipt.sessions_skipped = [int]$report.skipped", stop)
        self.assertIn("graceful_incomplete_shutdown", stop)

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
                self.assertEqual(spec["schema_version"], 4)
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
                self.assertEqual(spec["lifecycle"]["managed_stop"], "stop-event")
                wait = spec["lifecycle"]["graceful_stop_timeout_seconds"]
                self.assertTrue(1 <= wait <= 3600)
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

                self.assertEqual(config["schema_version"], 4)
                self.assertEqual(config["release_id"], spec["release_id"])
                self.assertEqual(config["deployment_profile"], spec["deployment_profile"])
                self.assertEqual(config["listen"]["host"], "127.0.0.1")
                self.assertEqual(config["listen"]["port"], spec["lifecycle"]["listen_port"])
                self.assertEqual(config["authentication"]["mode"], "required-api-key-file")
                self.assertEqual(config["engine"]["max_concurrency"], 1)
                self.assertEqual(config["engine"]["kv_dtype"], "int8")
                keep_warm = config["engine"]["gpu_keep_warm_ms"]
                self.assertIs(type(keep_warm), int)
                self.assertTrue(0 <= keep_warm <= 2**31 - 1)
                self.assertEqual(config["speculative"]["backend"], "mtp")
                self.assertTrue(config["session_checkpoint"]["enabled"])
                self.assertGreaterEqual(config["context_cache"]["device_state_slots"], 1)
                translation = spec["host_context_translation"]
                full_bytes = ((translation["predecessor_host_kv_mib"] << 20) +
                              24 * translation["native_state_image_bytes"])
                self.assertEqual(translation["predecessor_host_state_slots"], 24)
                self.assertEqual(translation["host_context_bytes"], full_bytes)
                self.assertEqual(Decimal(config["context_cache"]["host_context_mib"]) * (1 << 20), full_bytes)
                self.assertEqual(qualify_native.host_context_mib(spec), config["context_cache"]["host_context_mib"])
                self.assertLess(Decimal(qualify_native.host_context_mib(spec, state_images=8)),
                                Decimal(config["context_cache"]["host_context_mib"]))
                self.assertTrue({"host_state_slots", "host_kv_mib", "max_private_continuations"}.isdisjoint(config["context_cache"]))
                self.assertEqual(spec["model"]["format_version"], 3)
                self.assertEqual(spec["model"]["sha256"], "5476dfd6b243332f1754a494469ba4a1c8206edba6518b61dba1e0f4338664e7")
                self.assertNotIn("persistent_cache", config)
                self.assertNotIn("effort", config["reasoning"])
        self.assertEqual(len(models), 1, "every lane pins the same model artifact")


class PackagedStatusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        spec = importlib.util.spec_from_file_location("packaged_agent_protocol", WINDOWS / "agent_protocol.py")
        assert spec is not None and spec.loader is not None
        cls.protocol = importlib.util.module_from_spec(spec)
        with patch.object(sys, "path", [str(ROOT / "tools/smoke"), *sys.path]):
            spec.loader.exec_module(cls.protocol)

    def fixture(self) -> tuple[dict, SimpleNamespace]:
        identity = {
            "binary_sha256": "a" * 64, "model_artifact_sha256": "b" * 64,
            "config_sha256": "c" * 64, "patch_stack_sha": "d" * 40,
            "upstream_base_sha": "e" * 40, "deployment_profile": "fixture",
            "model_id": "model", "build_profile": "native-fixture", "source_dirty": False,
        }
        status = {
            "artifact_type": "ninfer_server_status", "schema_version": 2, "status": "ok",
            "identity": identity, "runtime": {"public_model_id": "model"},
            "scheduler": dict.fromkeys((
                "max_concurrency", "running", "prefilling", "decode_ready", "waiting",
                "materializing", "capture_pending", "committed_decode_tokens", "computed_prefill_tokens",
            ), 0),
            "cache": {
                "reused_prompt_tokens": 0, "last_selection": {"path": "root"},
                "host_state": {"capacity": None}, "host_kv": {"capacity_bytes": None},
                "private_catalog": {"capacity": None}, "shared_catalog": {"capacity": None},
            },
            "mtp": {},
        }
        args = SimpleNamespace(model="model", **{
            f"expect_{field}": identity[field] for field in (
                "binary_sha256", "model_artifact_sha256", "config_sha256", "deployment_profile",
            )
        })
        return status, args

    def test_schema2_accepts_real_counters_with_unavailable_capacity_fields(self) -> None:
        status, args = self.fixture()
        self.assertEqual(self.protocol.validate_status(status, args)["deployment_profile"], "fixture")

    def test_schema1_or_nulled_mandatory_fields_never_pass_as_schema2(self) -> None:
        for path in (
            ("schema_version",), ("identity", "binary_sha256"), ("identity", "source_dirty"),
            ("runtime", "public_model_id"), ("scheduler", "committed_decode_tokens"),
            ("scheduler", "computed_prefill_tokens"), ("cache", "reused_prompt_tokens"),
            ("cache", "last_selection", "path"),
        ):
            status, args = self.fixture()
            node = status
            for key in path[:-1]:
                node = node[key]
            node[path[-1]] = 1 if path == ("schema_version",) else None
            with self.subTest(path=path), self.assertRaises(self.protocol.ContractError):
                self.protocol.validate_status(status, args)

    def test_layout_translation_refuses_a_mismatched_budget(self) -> None:
        spec = json.loads((WINDOWS / "lanes/rtx4090/release-spec.json").read_text())
        broken = copy.deepcopy(spec)
        broken["host_context_translation"]["host_context_bytes"] += 1
        with self.assertRaisesRegex(qualify_native.LaneError, "inconsistent"):
            qualify_native.host_context_mib(broken)


POWERSHELL = shutil.which("pwsh") or shutil.which("powershell")


@unittest.skipUnless(POWERSHELL, "PowerShell runtime is required for executable script contracts")
class PowerShellContractTests(unittest.TestCase):
    def invoke(self, script: str) -> object:
        result = subprocess.run(
            [POWERSHELL, "-NoProfile", "-NonInteractive", "-Command",
             "$ErrorActionPreference='Stop';\n" + script],
            check=True, text=True, capture_output=True,
        )
        return json.loads(result.stdout)

    def test_all_release_scripts_parse(self) -> None:
        paths = [WINDOWS / name for name in SHARED_SCRIPTS] + [ROOT / "tests/test_release_security.ps1"]
        literal = ",".join("'" + str(path).replace("'", "''") + "'" for path in paths)
        parsed = self.invoke(f"""
$failures=@()
foreach($path in @({literal})){{
  $tokens=$null;$errors=$null
  [void][Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
  foreach($entry in $errors){{$failures+=$entry.Message}}
}}
ConvertTo-Json -InputObject @($failures) -Compress
""")
        self.assertEqual(parsed, [])

    def test_schema4_and_rollback_arguments_execute_without_aliases(self) -> None:
        controller = (WINDOWS / "Control-Release.ps1").read_text()
        function = ps_function(controller, "Add-ContextArguments")
        result = self.invoke(function + """
$new=[Collections.Generic.List[string]]::new()
$old=[Collections.Generic.List[string]]::new()
$current='{"schema_version":4,"context_cache":{"host_context_mib":"14787.734375"}}'|ConvertFrom-Json
$legacy='{"schema_version":3,"context_cache":{"host_state_slots":24,"host_kv_mib":11264,"max_private_continuations":8}}'|ConvertFrom-Json
Add-ContextArguments $new $current
Add-ContextArguments $old $legacy
$current.context_cache|Add-Member -NotePropertyName host_kv_mib -NotePropertyValue 1
$refused=$false
try{Add-ContextArguments ([Collections.Generic.List[string]]::new()) $current}catch{$refused=$true}
@{current=@($new);legacy=@($old);mixed_schema_refused=$refused}|ConvertTo-Json -Compress
""")
        self.assertEqual(result["current"], ["--host-context-mib", "14787.734375"])
        self.assertEqual(result["legacy"], [
            "--host-state-slots", "24", "--max-private-continuations", "8", "--host-kv-mib", "11264",
        ])
        self.assertTrue(result["mixed_schema_refused"])

    def test_scratch_task_suffix_executes_without_product_task_mutation(self) -> None:
        installer = (WINDOWS / "Install-Release.ps1").read_text()
        function = ps_function(installer, "Resolve-LifecycleTaskName")
        with tempfile.TemporaryDirectory() as directory:
            program_data = directory.replace("'", "''")
            result = self.invoke(function + f"""
$env:ProgramData='{program_data}'
$spec='{{"lifecycle":{{"task_name":"NInfer-Qwen38-0000-Native","state_root_name":"lane-fixture"}}}}'|ConvertFrom-Json
$canonical=Join-Path (Join-Path $env:ProgramData 'NInfer') 'lane-fixture'
$scratch=Join-Path $env:ProgramData 'isolated-fixture'
$normal=Resolve-LifecycleTaskName $spec $canonical ''
$isolated=Resolve-LifecycleTaskName $spec $scratch 'C3'
$refused=0
foreach($suffix in @('..','bad/suffix','bad suffix')){{
  try{{Resolve-LifecycleTaskName $spec $scratch $suffix|Out-Null}}catch{{$refused++}}
}}
try{{Resolve-LifecycleTaskName $spec $canonical 'C3'|Out-Null}}catch{{$refused++}}
@{{normal=$normal;isolated=$isolated;refusals=$refused}}|ConvertTo-Json -Compress
""")
        self.assertEqual(result["normal"], "NInfer-Qwen38-0000-Native")
        self.assertEqual(result["isolated"], "NInfer-Qwen38-0000-Native-Scratch-C3")
        self.assertEqual(result["refusals"], 4)

    def test_startup_attestation_binds_schema_uuid_ordinal_name_and_process(self) -> None:
        controller = (WINDOWS / "Control-Release.ps1").read_text()
        function = ps_function(controller, "Test-ServerStartGpu")
        with tempfile.TemporaryDirectory() as directory:
            log_path = str(Path(directory) / "requests.jsonl").replace("'", "''")
            result = self.invoke(function + f"""
$path='{log_path}'
$uuid='GPU-00000000-0000-0000-0000-000000000001'
$encoding=[Text.UTF8Encoding]::new($false)
$record=[ordered]@{{artifact_type='ninfer_serve_request_log';schema_version=30;event='server_start';server_instance_id='serve-4242-1';environment=@{{gpu_uuid=$uuid;device=0;gpu_name='Synthetic adapter'}}}}
$historical=($record|ConvertTo-Json -Compress)+"`n"
[IO.File]::WriteAllText($path,$historical,$encoding)
$offset=$encoding.GetByteCount($historical)
$creation=(Get-Item -LiteralPath $path).CreationTimeUtc.Ticks
$checks=[ordered]@{{}}
$checks.historical=(-not(Test-ServerStartGpu $path $uuid $offset $creation 4242 0 'Synthetic adapter' 30))
$record.server_instance_id='serve-4242-2'
[IO.File]::AppendAllText($path,($record|ConvertTo-Json -Compress),$encoding)
$checks.partial=(-not(Test-ServerStartGpu $path $uuid $offset $creation 4242 0 'Synthetic adapter' 30))
[IO.File]::AppendAllText($path,"`n",$encoding)
$checks.fresh=Test-ServerStartGpu $path $uuid $offset $creation 4242 0 'Synthetic adapter' 30
foreach($field in @('uuid','schema','device','name','pid')){{
  $record.schema_version=30;$record.environment.device=0;$record.environment.gpu_name='Synthetic adapter'
  $record.environment.gpu_uuid=$uuid;$record.server_instance_id='serve-4242-2'
  switch($field){{
    'uuid'{{$record.environment.gpu_uuid='GPU-00000000-0000-0000-0000-000000000002'}}
    'schema'{{$record.schema_version=29}}
    'device'{{$record.environment.device=1}}
    'name'{{$record.environment.gpu_name='Another adapter'}}
    'pid'{{$record.server_instance_id='serve-4243-2'}}
  }}
  [IO.File]::WriteAllText($path,$historical+($record|ConvertTo-Json -Compress)+"`n",$encoding)
  $refused=$false
  try{{Test-ServerStartGpu $path $uuid $offset $creation 4242 0 'Synthetic adapter' 30|Out-Null}}catch{{$refused=$true}}
  $checks[$field]=$refused
}}
[IO.File]::WriteAllText($path,'',$encoding)
$checks.truncation=$false;$checks.replacement=$false
try{{Test-ServerStartGpu $path $uuid $offset $creation 4242 0 'Synthetic adapter' 30|Out-Null}}catch{{$checks.truncation=$true}}
try{{Test-ServerStartGpu $path $uuid 0 ($creation+1) 4242 0 'Synthetic adapter' 30|Out-Null}}catch{{$checks.replacement=$true}}
$checks|ConvertTo-Json -Compress
""")
        self.assertEqual(len(result), 10)
        self.assertTrue(all(value is True for value in result.values()), result)


if __name__ == "__main__":
    unittest.main()
