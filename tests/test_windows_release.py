"""Contracts of the shared native Windows lifecycle tree and its lane specifications."""

from __future__ import annotations

import copy
import base64
from decimal import Decimal
import importlib.util
import io
import inspect
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
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


def product_protocol_fixture() -> dict:
    """Canonical metadata for adapter units, not an observed model-run receipt."""
    return {
        "artifact_type": "ninfer_agent_protocol_smoke", "schema_version": 2,
        "contract": "product", "status_identity_verified": True,
        "authenticated_session_identity": True, "same_session_reuse_path": "private_input_replay",
        "isolated_session_reuse_path": "root",
    }


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
                         "'--spec','mtp','--draft-tokens',[string]$c.speculative.draft_tokens,'--lm-head-draft'",
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
            remote_json=lambda host, script, **kwargs: captured.append(script) or {
                "status": "passed", "protocol": product_protocol_fixture()},
            config=SimpleNamespace(target="not-contacted", pressure_port=18201),
        )
        with patch.object(qualify_native, "remote_ps", return_value="") as cleanup:
            qualify_native.Orchestrator.pressure_protocol(subject)
        cleanup.assert_called_once()
        self.assertEqual(cleanup.call_args.args[0], subject.config.target)
        self.assertIn("Remove-Item -LiteralPath 'C:/qualification/pressure' -Recurse -Force",
                      cleanup.call_args.args[1])
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

    def test_gpu_metrics_preserves_fractional_temperature(self) -> None:
        uuid = "GPU-00000000-0000-0000-0000-000000000001"
        with patch.object(qualify_native, "run", return_value=SimpleNamespace(stdout="100.5, 48.94, 99, 12000\n")) as run:
            self.assertEqual(qualify_native.gpu_metrics(uuid), (100.5, 48.94, 99, 12000))
        run.assert_called_once_with([
            "nvidia-smi", "-i", uuid,
            "--query-gpu=power.draw,temperature.gpu,utilization.gpu,memory.used",
            "--format=csv,noheader,nounits",
        ])

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

    def test_gpu_workload_gate_uses_selected_resident_memory(self) -> None:
        owner = (WINDOWS / "Control-GpuOwner.ps1").read_text()
        memory = ps_function(owner, "Get-GpuMemoryUsedBytes")
        self.assertIn("'-i', $GpuUuid", memory)
        self.assertIn("'--query-gpu=memory.used'", memory)
        gate = ps_function(owner, "Get-InteractiveGpuWorkloadCount")
        self.assertIn("Get-GpuMemoryUsedBytes", gate)
        self.assertIn("$interactiveGpuMemoryThresholdBytes", gate)
        self.assertNotIn("Get-CimInstance", gate)
        self.assertNotIn("Get-Wddm", gate)
        self.assertIn("$interactiveGpuMemoryThresholdBytes = 1GB", owner)
        status = ps_function(owner, "Write-Status")
        self.assertIn("Get-WddmProcessMemoryDiagnostics", status)
        self.assertIn("'nvidia-smi.memory.used'", status)

    def test_owner_refuses_ambiguity_and_scopes_diagnostic_counter_luids(self) -> None:
        owner = (WINDOWS / "Control-GpuOwner.ps1").read_text()
        self.assertIn("$rows.Count -ne 1", ps_function(owner, "Resolve-OwnerGpu"))
        count = ps_function(owner, "Get-WddmProcessMemoryDiagnostics")
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
            scratch_state_root="C:/candidate", scratch_task_suffix="I6",
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

    def test_transfer_install_bootstrap_protects_each_root_and_uses_unique_helpers(self) -> None:
        class BootstrapCaptured(Exception):
            pass

        with tempfile.TemporaryDirectory() as temporary:
            subject = self.orchestrator(Path(temporary))
            subject.head8 = "f" * 8
            subject.state["phases"]["package"] = {
                "receipt": {"scratch_package": {"package": {"sha256": "e" * 64}}}
            }
            helpers = []
            scripts = []
            for lane in ("rtx4090", "rtx3090"):
                subject.lane.name = lane
                subject.target_root = f"C:/qualification-{lane}"
                for _ in range(2):
                    with patch.object(qualify_native, "run", return_value=SimpleNamespace(stdout="# helper")), \
                            patch.object(qualify_native, "scp") as copied, \
                            patch.object(qualify_native, "remote_ps", side_effect=BootstrapCaptured) as remote:
                        with self.assertRaises(BootstrapCaptured):
                            subject.transfer_install()
                        helpers.append(copied.call_args.args[1])
                        scripts.append(remote.call_args.args[1])
                with self.subTest(lane=lane):
                    self.assertTrue(all(lane in helper for helper in helpers[-2:]))
            with self.subTest(invariant="unique helper for every invocation"):
                self.assertEqual(len(helpers), len(set(helpers)))
            for script in scripts:
                with self.subTest(invariant="protected staging root"):
                    self.assertIn("Initialize-NInferProtectedStateRoot $p|Out-Null", script)
                    self.assertNotIn("New-Item -ItemType Directory -Path $p,", script)

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
                    return {"status": "passed", "process_count": 0}

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

    def test_native_save_receipt_matches_frozen_k7_fixture(self) -> None:
        receipt = json.loads((ROOT / "tests/fixtures/durable/ack-windows.json").read_text())
        qualify_native.require_native_save_receipt(receipt)
        for field, value in (
            ("schema", None), ("committed", 1), ("origin", "unauthenticated"),
            ("dir_sync", "fsync"), ("state", "available"), ("payload_bytes", None),
            ("restored_tokens", receipt["frontier_tokens"] + 1), ("points", False),
        ):
            broken = {**receipt, field: value}
            with self.subTest(field=field), self.assertRaises(qualify_native.LaneError):
                qualify_native.require_native_save_receipt(broken)
        legacy = {key: value for key, value in receipt.items() if key != "schema"}
        legacy["artifact_type"] = "ninfer-durable-operation/1"
        with self.assertRaises(qualify_native.LaneError):
            qualify_native.require_native_save_receipt(legacy)

    def test_checkpoint_status_never_accepts_a_predecessor_schema(self) -> None:
        status = {"artifact_type": "ninfer_session_checkpoint_status", "schema_version": 3,
                  "state": "available"}
        with patch.object(qualify_native, "request_json", return_value=status):
            self.assertEqual(qualify_native.checkpoint_state("unused", "fixture", "session"), "available")
        for version in (1, 2, None):
            with self.subTest(version=version), patch.object(
                qualify_native, "request_json", return_value={**status, "schema_version": version}
            ), self.assertRaises(qualify_native.LaneError):
                qualify_native.checkpoint_state("unused", "fixture", "session")

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
            "protocol": {"protocol": product_protocol_fixture()}, "rollback": {"status": "passed"},
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


class RestartFixtureTests(unittest.TestCase):
    @staticmethod
    def status(prefill: int = 53, decode: int = 3, reused: int = 0) -> dict:
        return {
            "schema_version": 2,
            "scheduler": {"computed_prefill_tokens": prefill, "committed_decode_tokens": decode},
            "cache": {"reused_prompt_tokens": reused},
        }

    def restart_fixture(
        self, *, baseline: dict | None = None, restarted: dict | None = None,
        pids: tuple[int, int] = (101, 202), control_answer: str | None = None,
        flush_answer: str | None = None, control_cached: int = 1600, flush_cached: int = 1600,
        entry_stopped: bool = False,
    ) -> tuple[dict, list, list]:
        baseline = copy.deepcopy(self.status() if baseline is None else baseline)
        restarted = copy.deepcopy(baseline if restarted is None else restarted)
        events, seeds = [], []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / "cache"
            generation = cache / "session-checkpoints-v3" / "sessions" / "unit-session"
            generation.mkdir(parents=True)
            (generation / "unit-payload").write_bytes(b"unit-only checkpoint fixture")
            (root / "state.json").write_text(json.dumps({
                "active_release": "unit-release",
                "releases": {"unit-release": {
                    "api_key_file": str(root / "unit.key"), "host": "127.0.0.1",
                    "port": 18211, "cache_root": str(cache),
                }},
            }))
            args = SimpleNamespace(evidence_root=str(root / "evidence"),
                                   state_root=str(root), model_id="unit-model")
            counters, listeners = iter((baseline, restarted)), iter(pids)
            save = json.loads((ROOT / "tests/fixtures/durable/ack-windows.json").read_text())
            running = not entry_stopped

            def controller(_controller, _root, action):
                nonlocal running
                events.append(("controller", action))
                stop = {"mode": "stop-event" if running else "none",
                        "outcome": "graceful" if running else "already_stopped", "forced": False}
                running = True
                return {"last_stop": stop}

            def run(command, **_kwargs):
                nonlocal running
                if any("Get-NetTCPConnection" in str(part) for part in command):
                    return SimpleNamespace(stdout=str(next(listeners)))
                if "-Action" in command and command[command.index("-Action") + 1] == "Start":
                    events.append(("controller", "Start"))
                    running = True
                return SimpleNamespace(stdout="")

            def request(_base, _key, method, route, body=None):
                events.append(("request", method, route))
                if method == "GET" and route == "/v1/ninfer/status":
                    return next(counters)
                if method == "POST" and route == "/v1/ninfer/checkpoints":
                    return save
                if method == "DELETE":
                    return {"deleted": True}
                if method == "POST" and route == "/v1/responses":
                    if "previous_response_id" not in body:
                        seeds.append(body)
                        return {"id": f"unit-response-{len(seeds)}"}
                    control = body["previous_response_id"] == "unit-response-1"
                    answer = ((qualify_native.CHECKPOINT_MARKER if control_answer is None else control_answer)
                              if control else (qualify_native.FLUSH_MARKER if flush_answer is None else flush_answer))
                    return {
                        "output": [{"type": "message", "content": [{"type": "output_text", "text": answer}]}],
                        "usage": {"input_tokens_details": {
                            "cached_tokens": control_cached if control else flush_cached,
                        }},
                    }
                self.fail(f"unexpected unit request: {method} {route}")

            with patch.object(qualify_native, "run", side_effect=run), \
                    patch.object(qualify_native, "read_key", return_value="unit-only credential"), \
                    patch.object(qualify_native, "controller_status", side_effect=controller), \
                    patch.object(qualify_native, "request_json", side_effect=request), \
                    patch.object(qualify_native, "checkpoint_state",
                                 side_effect=("available", "missing", "available")):
                receipt = qualify_native.internal_restart(args)
            return receipt, events, seeds

    def test_restart_resets_to_measured_nonzero_warmup_baseline(self) -> None:
        for prefill, decode in ((53, 3), (137, 11)):
            with self.subTest(prefill=prefill, decode=decode):
                receipt, events, _ = self.restart_fixture(baseline=self.status(prefill, decode))
                expected = {"computed_prefill_tokens": prefill, "committed_decode_tokens": decode,
                            "reused_prompt_tokens": 0}
                self.assertEqual(receipt["counter_baseline"], expected)
                self.assertEqual(receipt["counters_after_restart"], expected)
                self.assertTrue(receipt["restart_counters_reset"])
                self.assertNotEqual(receipt["old_pid"], receipt["new_pid"])
                self.assertEqual(events[0], ("controller", "Start"))
                self.assertEqual(events[1], ("controller", "Restart"))
                self.assertEqual(events[2], ("request", "GET", "/v1/ninfer/status"))
                self.assertEqual([event for event in events if event[0] == "controller"],
                                 [("controller", "Start"), ("controller", "Restart"),
                                  ("controller", "Restart")])

    def test_restart_preserves_idempotent_stopped_entry_before_cold_setup(self) -> None:
        receipt, events, _ = self.restart_fixture(entry_stopped=True)
        self.assertTrue(receipt["restart_counters_reset"])
        self.assertEqual(events[:3], [("controller", "Start"), ("controller", "Restart"),
                                     ("request", "GET", "/v1/ninfer/status")])

    def test_restart_refuses_drift_from_each_measured_counter(self) -> None:
        for section, field in (("scheduler", "computed_prefill_tokens"),
                               ("scheduler", "committed_decode_tokens"),
                               ("cache", "reused_prompt_tokens")):
            changed = self.status()
            changed[section][field] += 1
            with self.subTest(field=field), self.assertRaises(qualify_native.LaneError):
                self.restart_fixture(restarted=changed)

    def test_restart_requires_schema_two_and_real_integer_counters(self) -> None:
        for value in (None, True, -1, "53"):
            for when in ("baseline", "restarted"):
                damaged = self.status()
                damaged["scheduler"]["computed_prefill_tokens"] = value
                with self.subTest(value=value, when=when), self.assertRaises(qualify_native.LaneError):
                    self.restart_fixture(**{when: damaged})
        with self.assertRaises(qualify_native.LaneError):
            self.restart_fixture(baseline={**self.status(), "schema_version": 1})
        with self.assertRaises(qualify_native.LaneError):
            self.restart_fixture(baseline=self.status(reused=1))

    def test_restart_requires_actual_process_replacement(self) -> None:
        with self.assertRaisesRegex(qualify_native.LaneError, "replace"):
            self.restart_fixture(pids=(101, 101))

    def test_restart_keeps_both_exact_marker_and_positive_cache_gates(self) -> None:
        for damage in ({"control_answer": "wrong"}, {"flush_answer": "wrong"},
                       {"control_cached": 0}, {"flush_cached": 0}):
            with self.subTest(damage=damage), self.assertRaises(qualify_native.LaneError):
                self.restart_fixture(**damage)

    def test_restart_seeds_use_canonical_private_user_context(self) -> None:
        _, _, seeds = self.restart_fixture()
        self.assertEqual(len(seeds), 2)
        context = "Private user context:" + " durable" * 1600
        for seed, marker in zip(seeds, (qualify_native.CHECKPOINT_MARKER, qualify_native.FLUSH_MARKER)):
            self.assertTrue(seed["input"].startswith(context + "\n\n"))
            self.assertIn(marker, seed["input"])
            self.assertNotIn("instructions", seed)


class PrivateMarkerTests(unittest.TestCase):
    def test_forward_separator_profile_fixture_is_rejected(self) -> None:
        separator = bytes((92,))
        profile = separator.join((b"C:", b"Users", b"operator"))
        fixture = profile.replace(separator, b"/") + b"/vcpkg/tools/pkg-config"
        self.assertGreater(qualify_native.marker_findings(fixture, profile), 0)

    def test_binary_home_prefixes_are_rejected_without_an_owner_component(self) -> None:
        backslash = bytes((92,))
        prefixes = (
            backslash.join((b"C:", b"Users", b"")),
            b"/".join((b"C:", b"Users", b"")),
            b"/".join((b"", b"Users", b"")),
            b"/".join((b"", b"home", b"")),
        )
        for prefix in prefixes:
            with self.subTest(prefix=prefix):
                self.assertGreater(qualify_native.marker_findings(prefix + b"\0"), 0)

    def test_binary_domain_rejects_all_home_spellings_and_known_hosts(self) -> None:
        backslash = bytes((92,))
        windows = backslash.join((b"C:", b"Users", b"private-owner", b"build"))
        host = b"-".join((b"private", b"builder"))
        for fixture in (
            windows, windows.replace(backslash, b"/"), windows.replace(backslash, backslash * 2),
            b"/".join((b"", b"Users", b"private-owner", b"build")),
            b"/".join((b"", b"home", b"private-owner", b"build")),
            host.upper(),
        ):
            with self.subTest(fixture=fixture):
                self.assertGreater(qualify_native.marker_findings(fixture, private_markers=(host,)), 0)
        self.assertEqual(qualify_native.marker_findings(b"C:/b/native/vcpkg", private_markers=(host,)), 0)

    def test_source_domain_reports_public_references_without_waiving_actual_homes(self) -> None:
        slash = b"/"
        references = (
            slash.join((b"", b"home", b"neroued", b"models")),
            slash.join((b"", b"home", b"operator", b"build")),
            slash.join((b"C:", b"Users", b"REPLACE_ME", b"models")),
            slash.join((b"", b"users", b":id", b"subscriptions")),
        )
        profile = slash.join((b"C:", b"Users", b"private-owner"))
        host = b"-".join((b"private", b"builder"))
        payload = b"\n".join(references)
        markers = (profile, host)
        self.assertEqual(qualify_native.marker_findings(payload, private_markers=markers, source=True), 0)
        reported = qualify_native.source_references(payload, private_markers=markers)
        self.assertEqual(len(reported), 4)
        self.assertEqual({row["kind"] for row in reported}, {"posix-home", "windows-user", "macos-user"})
        encoded = json.dumps(reported).encode()
        self.assertEqual(qualify_native.marker_findings(encoded), 0)
        for fixture in (profile + b"/build", profile.replace(slash, bytes((92,))) + bytes((92,)) + b"build",
                        slash.join((b"", b"home", b"private-owner", b"build")), host.upper()):
            with self.subTest(fixture=fixture):
                self.assertGreater(qualify_native.marker_findings(fixture, private_markers=markers, source=True), 0)
        credential = b"AKIA" + b"A" * 16
        self.assertGreater(qualify_native.marker_findings(credential, source=True), 0)

    def test_real_directory_and_tar_scans_preserve_the_two_domains(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payload = b"/".join((b"", b"home", b"neroued", b"models"))
            (root / "fixture.dll").write_bytes(payload)
            self.assertGreater(qualify_native.scan_directory(root)["findings"], 0)
            archive = root / "fixture.tar.gz"
            with tarfile.open(archive, "w:gz") as stream:
                member = tarfile.TarInfo("source/public-example.txt")
                member.size = len(payload)
                stream.addfile(member, io.BytesIO(payload))
            self.assertGreater(qualify_native.scan_archive(archive)["findings"], 0)
            source = qualify_native.scan_archive(archive, source_archive=True)
            self.assertEqual(source["findings"], 0)
            self.assertEqual(source["domain"], "source")
            self.assertEqual(source["public_generic_references"][0]["components"], ["neroued", "models"])
            private = b"/".join((b"", b"home", b"private-owner"))
            with tarfile.open(archive, "w:gz") as stream:
                member = tarfile.TarInfo("source/link")
                member.type = tarfile.SYMTYPE
                member.linkname = (private + b"/models").decode()
                stream.addfile(member)
            self.assertGreater(qualify_native.scan_archive(
                archive, source_archive=True, private_markers=(private,),
            )["findings"], 0)

    def test_internal_scope_round_trip_and_scanner_definitions_do_not_self_match(self) -> None:
        home = "/".join(("", "home", "private-owner"))
        host = "-".join(("private", "builder"))
        scope = base64.b64encode(json.dumps({"homes": [home], "hosts": [host]}).encode()).decode()
        self.assertEqual(set(qualify_native.decode_privacy_scope(scope)), {home.encode(), host.encode()})
        for name in ("tools/qualification/qualify_native.py", "tests/test_windows_release.py",
                     "tools/bench/run_exp097.py"):
            with self.subTest(name=name):
                self.assertEqual(qualify_native.marker_findings((ROOT / name).read_bytes()), 0)


class ScratchQualificationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.spec = json.loads((WINDOWS / "lanes/rtx4090/release-spec.json").read_text())
        self.config = json.loads((WINDOWS / "lanes/rtx4090/server-config.json").read_text())
        self.lane = qualify_native.Lane("rtx4090", self.spec, self.config)

    def test_scratch_binding_refuses_product_roots_tasks_and_ports(self) -> None:
        config = SimpleNamespace(
            scratch_state_root="C:/ProgramData/NInferQualification/C3-I6/qwen38-4090-native",
            scratch_task_suffix="I6", scratch_port=18210, pressure_port=18200,
            api_key_file="C:/protected/operator.key", qualified_runtime_bin=None, qualified_build_receipt=None,
        )
        qualify_native.validate_scratch_configuration(config, self.spec)
        for field, value in (
            ("scratch_state_root", "C:/ProgramData/NInfer/qwen38-4090-native"),
            ("scratch_state_root", "c:/programdata/ninfer/qwen38-4090-native"),
            ("scratch_state_root", "C:/scratch/wrong-leaf"),
            ("scratch_state_root", "C:/scratch/../qwen38-4090-native"),
            ("scratch_task_suffix", "../unsafe"), ("scratch_task_suffix", "x" * 27),
            ("scratch_port", 18082), ("pressure_port", 18210),
            ("api_key_file", ""), ("qualified_runtime_bin", "D:/qualified/bin"),
        ):
            changed = copy.copy(config)
            setattr(changed, field, value)
            with self.subTest(field=field, value=value), self.assertRaises(qualify_native.LaneError):
                qualify_native.validate_scratch_configuration(changed, self.spec)
        subject = qualify_native.Orchestrator.__new__(qualify_native.Orchestrator)
        subject.config, subject.lane = config, self.lane
        self.assertEqual(subject.task_name, self.spec["lifecycle"]["task_name"] + "-Scratch-I6")
        self.assertEqual(subject.state_root, config.scratch_state_root)
        source = (ROOT / "tools/qualification/qualify_native.py").read_text()
        self.assertNotIn("Unregister-ScheduledTask", source)
        self.assertNotIn("Export-ScheduledTask", source)
        self.assertIn("ScratchTaskSuffix=", source)

    def test_scratch_equivalence_allows_only_the_explicit_port_change(self) -> None:
        scratch = copy.deepcopy(self.config)
        scratch["listen"]["port"] = 18210
        original = copy.deepcopy(scratch)
        qualify_native.require_port_only_equivalence(self.config, scratch, 18210)
        self.assertEqual(scratch, original)
        for group, key, value in (
            ("engine", "admission_headroom_mib", 256), ("engine", "device", 1),
            ("session_checkpoint", "require_origin_auth", False), ("listen", "port", 18082),
        ):
            changed = copy.deepcopy(scratch)
            changed[group][key] = value
            with self.subTest(group=group, key=key), self.assertRaises(qualify_native.LaneError):
                qualify_native.require_port_only_equivalence(self.config, changed, 18210)

    def test_failed_inner_receipt_cannot_be_recorded_as_a_passing_phase(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            subject = QualificationRestorationTests().orchestrator(Path(temporary))
            with self.assertRaisesRegex(qualify_native.LaneError, "non-passing receipt"):
                subject.phase("protocol", lambda: {"status": "failed"})
            self.assertEqual(subject.state["phases"]["protocol"]["status"], "failed")

    def pressure_subject(self, response):
        scripts = []

        def remote_json(_host, script, **_kwargs):
            scripts.append(script)
            if isinstance(response, Exception):
                raise response
            return response

        subject = SimpleNamespace(
            state_root="C:/unit-state", target_root="C:/unit-stage", gpu_uuid="GPU-unit-fixture",
            lane=SimpleNamespace(spec=self.spec, model_id="unit-model"),
            config=SimpleNamespace(target="unit-target", pressure_port=18200),
            remote_json=remote_json,
        )
        return subject, scripts

    def test_pressure_transport_failure_preserves_store_and_drained_logs(self) -> None:
        subject, scripts = self.pressure_subject(qualify_native.LaneError("unit transport failure"))
        with patch.object(qualify_native, "remote_ps") as cleanup:
            with self.assertRaisesRegex(qualify_native.LaneError, "unit transport failure"):
                qualify_native.Orchestrator.pressure_protocol(subject)
        cleanup.assert_not_called()
        self.assertEqual(len(scripts), 1)
        self.assertIn("pressure scratch already exists; preserve it until explicit operator cleanup", scripts[0])
        self.assertNotIn("Remove-Item $scratch", scripts[0])
        self.assertIn("$stdoutCopy.GetAwaiter().GetResult()", scripts[0])
        self.assertIn("$stderrCopy.GetAwaiter().GetResult()", scripts[0])
        self.assertIn("artifacts retained at", scripts[0])

    def test_pressure_nonpassing_receipt_never_cleans_the_store(self) -> None:
        subject, _ = self.pressure_subject({"status": "failed", "protocol": product_protocol_fixture()})
        with patch.object(qualify_native, "remote_ps") as cleanup:
            with self.assertRaisesRegex(qualify_native.LaneError, "logs and store are retained"):
                qualify_native.Orchestrator.pressure_protocol(subject)
        cleanup.assert_not_called()

    def test_pressure_bad_canonical_payload_never_cleans_the_store(self) -> None:
        protocol = product_protocol_fixture()
        protocol["schema_version"] = 1
        subject, _ = self.pressure_subject({"status": "passed", "protocol": protocol})
        with patch.object(qualify_native, "remote_ps") as cleanup:
            with self.assertRaises(qualify_native.LaneError):
                qualify_native.Orchestrator.pressure_protocol(subject)
        cleanup.assert_not_called()

    def test_pressure_cleans_only_its_validated_successful_scratch(self) -> None:
        receipt = {"status": "passed", "protocol": product_protocol_fixture()}
        subject, scripts = self.pressure_subject(receipt)
        with patch.object(qualify_native, "remote_ps", return_value="") as cleanup:
            self.assertIs(qualify_native.Orchestrator.pressure_protocol(subject), receipt)
        cleanup.assert_called_once()
        self.assertEqual(cleanup.call_args.args[0], "unit-target")
        self.assertIn("Remove-Item -LiteralPath 'C:/unit-stage/pressure'", cleanup.call_args.args[1])
        self.assertNotIn("Remove-Item $scratch", scripts[0])

    def stage_fixture(self) -> tuple[dict, dict]:
        # Synthetic unit evidence exercises the adapter, not a native/GPU qualification.
        package = {
            "package": {"filename": "unit.tar.gz", "sha256": "a" * 64, "bytes": 1},
            "config_sha256": "c" * 64, "binaries": {"server_sha256": "b" * 64},
            "support_assets": {"installer_sha256": "e" * 64},
            "upstream_base_sha": self.spec["source"]["upstream_base_sha"],
            "lineage_base_sha": self.spec["source"]["lineage_base_sha"],
            "build_profile": self.spec["build_profile"], "model_sha256": self.spec["model"]["sha256"],
            "sbom": {"sha256": "f" * 64}, "dependency_sbom": {"sha256": "f" * 64},
            "outer_sha256": "f" * 64,
        }
        scratch = copy.deepcopy(package)
        scratch["package"]["sha256"] = "d" * 64
        scratch["config_sha256"] = "e" * 64
        package["scratch_package"] = scratch
        package["scratch_equivalence"] = {
            "status": "passed", "public_package_executed": False,
            "configuration_delta": {"path": "/listen/port", "public": 18082, "scratch": 18210},
            "public_package_sha256": package["package"]["sha256"],
            "scratch_package_sha256": scratch["package"]["sha256"],
            "public_configuration_sha256": package["config_sha256"],
            "scratch_configuration_sha256": scratch["config_sha256"],
        }
        protocol = product_protocol_fixture()
        identity = {
            "patch_stack_sha": "a" * 40, "source_dirty": False,
            "upstream_base_sha": package["upstream_base_sha"], "build_profile": package["build_profile"],
            "deployment_profile": self.spec["deployment_profile"], "model_id": self.lane.model_id,
            "binary_sha256": "b" * 64, "model_artifact_sha256": package["model_sha256"],
            "config_sha256": scratch["config_sha256"],
        }
        receipts = {
            "package": package, "transfer_install": {"package_sha256": scratch["package"]["sha256"]},
            "protocol": {
                "status": "passed", "protocol": protocol, "identity": identity,
                "startup_attestation": {"request_log_schema": 30, "gpu_uuid_verified": True,
                                        "gpu_index": 0, "gpu_name": self.spec["gpu"]["name"]},
            },
            "pressure_protocol": {"status": "passed", "protocol": copy.deepcopy(protocol), "host_context_mib": "1",
                                  "device_state_slots": 2},
            "context_128k": {"prompt_tokens": 130048, "exact_output": qualify_native.EXPECTED_LONG},
            "restart": {
                "old_pid": 1, "new_pid": 2, "cached_input_tokens": 100,
                "managed_stop": {"mode": "stop-event", "outcome": "graceful", "forced": False},
                "managed_stop_flush": {"exact_output": qualify_native.FLUSH_MARKER},
            },
            "security": {"security": {"status": "passed", "root_dacl_protected": True,
                                      "low_privilege_effective_read_denials": 2}},
            "omp": {"status": "passed", "exact_final_answer": True, "typed_tool_name": "read",
                    "tool_results": 1, "events": 1, "client_version": "unit-fixture"},
            "benchmark_c1": {"completion_tokens": 1024, "decode_tokens_per_second": 100,
                             "gpu_uuid": "GPU-private-unit-value"},
            "rollback": {"status": "not_applicable", "directions": 0},
        }
        state = {
            "status": "passed", "source_commit": "a" * 40,
            "phases": {name: {"status": "passed", "receipt_sha256": "f" * 64,
                               "receipt": receipts.get(name, {"status": "passed"})}
                       for name in qualify_native.PHASES},
        }
        summary = {"status": "passed", "qualified_utc": "2026-01-01T00:00:00Z",
                   "restoration": {"status": "passed"}}
        return state, summary

    def test_stage_adapter_binds_public_package_and_real_scratch_identity_separately(self) -> None:
        state, summary = self.stage_fixture()
        staged = qualify_native.stage_lane_qualification(state, summary, self.lane)
        self.assertEqual(staged["artifact_type"], "ninfer_windows_release_qualification")
        self.assertEqual(staged["identity"]["config_sha256"], "c" * 64)
        self.assertEqual(staged["identity"]["qualified_configuration_sha256"], "e" * 64)
        self.assertEqual(staged["scratch_equivalence"]["observed_identity"]["config_sha256"], "e" * 64)
        self.assertFalse(staged["scratch_equivalence"]["public_package_executed"])
        self.assertEqual(staged["deterministic_gates"]["rollback"], "not_applicable")
        self.assertNotIn("GPU-private", json.dumps(staged))

    def test_stage_adapter_refuses_missing_failed_or_misbound_evidence(self) -> None:
        for damage in (
            lambda state, summary: state["phases"]["restart"].update(status="failed"),
            lambda state, summary: summary["restoration"].update(status="failed"),
            lambda state, summary: state["phases"]["protocol"]["receipt"]["identity"].update(config_sha256="c" * 64),
            lambda state, summary: state["phases"]["protocol"]["receipt"]["protocol"].update(authenticated_session_identity=False),
            lambda state, summary: state["phases"]["package"]["receipt"]["scratch_equivalence"].update(public_package_executed=True),
            lambda state, summary: state["phases"]["transfer_install"]["receipt"].update(package_sha256="f" * 64),
            lambda state, summary: state["phases"]["restart"]["receipt"].update(new_pid=1),
            lambda state, summary: state["phases"]["omp"]["receipt"].update(exact_final_answer=False),
            lambda state, summary: state["phases"]["omp"]["receipt"].update(status="failed"),
            lambda state, summary: state["phases"]["benchmark_c1"]["receipt"].update(completion_tokens=1),
        ):
            state, summary = self.stage_fixture()
            damage(state, summary)
            with self.assertRaises(qualify_native.LaneError):
                qualify_native.stage_lane_qualification(state, summary, self.lane)

    def test_product_protocol_refuses_legacy_or_nonprivate_results(self) -> None:
        state, _ = self.stage_fixture()
        protocol = state["phases"]["protocol"]["receipt"]["protocol"]
        for damage in (
            {"schema_version": 1}, {"contract": "public"}, {"status_identity_verified": False},
            {"authenticated_session_identity": False}, {"same_session_reuse_path": "root"},
            {"isolated_session_reuse_path": "private_input_replay"},
        ):
            with self.subTest(damage=damage), self.assertRaises(qualify_native.LaneError):
                qualify_native.require_product_protocol({**protocol, **damage})


class SharedLifecycleTreeTests(unittest.TestCase):
    def test_native_protocol_uses_canonical_product_smoke(self) -> None:
        options = SimpleNamespace(
            lane_dir="packaging/windows/lanes/rtx3090", lifecycle_dir="packaging/windows",
            release_head_sha="f" * 40, release_notes=None, release_version="0.7.0-beta.2",
        )
        with patch.object(release_package, "read_committed_file", return_value=b"committed") as read:
            release_package.windows_support_files(options, ROOT, "payload")
        sources = {call.args[2] for call in read.call_args_list}
        with self.subTest(invariant="one canonical fixture"):
            self.assertIn("tools/smoke/agent_protocol.py", sources)
            self.assertNotIn("packaging/windows/agent_protocol.py", sources)
            self.assertFalse((WINDOWS / "agent_protocol.py").exists())
        self.assertIn("tools/smoke/serve_contract.py", sources)
        for method in (qualify_native.Orchestrator.protocol, qualify_native.Orchestrator.pressure_protocol):
            with self.subTest(method=method.__name__):
                self.assertIn("--contract product", inspect.getsource(method))

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
            "--spec", "--draft-tokens", "--lm-head-draft",
            "--admission-headroom-mib", "--session-checkpoint-require-origin-auth",
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
        forbidden = re.compile(r"3090|4090|5090|sm_8[69]|sm_120|\b(?:300|301|350|351|370|450)\s*W\b|omp-v0\.2|NInfer-Qwen38-[0-9]{4}")
        paths = [WINDOWS / name for name in SHARED_SCRIPTS] + [ROOT / "tools/smoke/agent_protocol.py"]
        for path in paths:
            text = path.read_text(encoding="utf-8")
            hits = {match.group(0) for match in forbidden.finditer(text)}
            self.assertEqual(hits, set(), f"{path.name} carries lane literals {sorted(hits)}")

    def test_packaged_lifecycle_inventory_is_one_set(self) -> None:
        packager_files = set(release_package._WINDOWS_LIFECYCLE_FILES)
        installer = (WINDOWS / "Install-Release.ps1").read_text(encoding="utf-8")
        start = installer.index("$lifecycleBin = ")
        installed = set(re.findall(r"'((?:Control|Install|Protect)-[A-Za-z]+\.ps1)'", installer[start:start + 600]))
        self.assertEqual(installed, packager_files)
        for name in packager_files | {"RELEASE_NOTES.md"}:
            self.assertTrue((WINDOWS / name).is_file(), name)
        for name in ("agent_protocol.py", "serve_contract.py"):
            self.assertTrue((ROOT / "tools/smoke" / name).is_file())


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
                architecture = gpu["compute_capability"].replace(".", "")
                self.assertEqual(gpu["cmake_cuda_architecture"], architecture + ("a" if lane == "rtx5090" else ""))
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
                blackwell = lane == "rtx5090"
                self.assertEqual(config["engine"]["max_concurrency"], 2 if blackwell else 1)
                self.assertEqual(config["engine"]["kv_dtype"], "bf16" if blackwell else "int8")
                keep_warm = config["engine"]["gpu_keep_warm_ms"]
                self.assertIs(type(keep_warm), int)
                self.assertTrue(0 <= keep_warm <= 2**31 - 1)
                self.assertEqual(config["speculative"]["backend"], "dflash2" if blackwell else "mtp")
                self.assertEqual(config["speculative"]["draft_tokens"], 7 if blackwell else 3)
                self.assertEqual(config["engine"]["admission_headroom_mib"], 768 if lane == "rtx3090" else 640)
                if blackwell:
                    self.assertEqual(spec["qualification"]["purpose"], "experimental-feasibility-only-not-a-release")
                    self.assertEqual(config["engine"]["max_context"], 131072)
                    self.assertEqual(config["engine"]["kv_capacity"], 146368)
                    self.assertEqual(config["engine"]["prefill_chunk"], 1024)
                    self.assertTrue(config["engine"]["vision"])
                self.assertTrue(config["session_checkpoint"]["enabled"])
                self.assertIs(config["session_checkpoint"]["require_origin_auth"], True)
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
                self.assertEqual(spec["model"]["sha256"], "81f924d440c27261d820c19a9f8d45794c5aee410f8a68bd358133fa8c0375da")
                self.assertEqual(spec["model"]["bytes"], 20437521664)
                self.assertEqual(spec["model"]["revision"], "1cbd84e7221e51186bd7f093a149912d2489625b")
                self.assertNotIn("upgraded_from_sha256", spec["model"])
                self.assertNotIn("persistent_cache", config)
                self.assertNotIn("effort", config["reasoning"])
        self.assertEqual(len(models), 1, "every lane pins the same model artifact")

    def test_rtx3090_lane_serves_at_the_approved_350_w_cap(self) -> None:
        # Approved 2026-10-26: the managed GPU-owner lease caps the RTX 3090 at 350 W (was 300 W);
        # the C1 qualification envelope keeps its 1 W sampling allowance above the cap, and the
        # owner's 370 W default is still what release restores.
        lane_dir = WINDOWS / "lanes/rtx3090"
        lane = qualify_native.Lane(
            "rtx3090", json.loads((lane_dir / "release-spec.json").read_text(encoding="utf-8")),
            json.loads((lane_dir / "server-config.json").read_text(encoding="utf-8")),
        )
        self.assertEqual(lane.qualified_power_limit_w, 350)
        self.assertEqual(lane.managed_power_envelope_w, 351.0)
        self.assertEqual(lane.owner_power_limit_w, 370)
        owner = lane.spec["lifecycle"]["gpu_owner_controller_protocol"]
        self.assertEqual(owner["prior_power_limit_range_w"], [100, 390])


class PackagedStatusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        spec = importlib.util.spec_from_file_location("packaged_agent_protocol", ROOT / "tools/smoke/agent_protocol.py")
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
        self.assertIsNone(self.protocol.validate_status_identity(status, args))

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
            with self.subTest(path=path), self.assertRaises(self.protocol.ProtocolError):
                self.protocol.validate_status_identity(status, args)

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

    def test_native_lane_speculation_guards_agree(self) -> None:
        installer = (WINDOWS / "Install-Release.ps1").read_text()
        packager = (WINDOWS / "New-Package.ps1").read_text()
        package_guard = packager[packager.index("$validSpeculation ="):
                                 packager.index("if ($config.artifact_type")]
        functions = "\n".join(ps_function(installer, name) for name in (
            "Assert-AllowedListenHost", "Assert-InstallerArchitectureContract"))
        for lane_dir in LANES:
            spec = qualify_native.ps_quote(str(lane_dir / "release-spec.json"))
            config = qualify_native.ps_quote(str(lane_dir / "server-config.json"))
            observed = self.invoke(f"""
{functions}
$Spec=Get-Content -Raw {spec}|ConvertFrom-Json
$Config=Get-Content -Raw {config}|ConvertFrom-Json
$Lane=[string]$Spec.lane
$results=@()
foreach($variant in @('valid','wrong-concurrency','wrong-backend','zero-drafts')){{
  $Config=Get-Content -Raw {config}|ConvertFrom-Json
  if($variant -ceq 'wrong-concurrency'){{$Config.engine.max_concurrency=3}}
  if($variant -ceq 'wrong-backend'){{$Config.speculative.backend='dflash'}}
  if($variant -ceq 'zero-drafts'){{$Config.speculative.draft_tokens=0}}
  {package_guard}
  $accepted=$true
  try{{Assert-InstallerArchitectureContract $Spec $Config}}catch{{$accepted=$false}}
  $results+=@{{package=[bool]$validSpeculation;installer=$accepted}}
}}
ConvertTo-Json -InputObject @($results) -Compress
""")
            self.assertEqual(observed, [
                {"package": valid, "installer": valid} for valid in (True, False, False, False)
            ], lane_dir.name)

    def test_dflash2_controller_arguments_and_bounds(self) -> None:
        controller = (WINDOWS / "Control-Release.ps1").read_text()
        start = controller.index("$speculativeBackend =")
        launch = controller[start:controller.index("# Both flags belong", start)]
        observed = self.invoke(f"""
$results=@()
foreach($draft in @(0,7,16)){{
  $config=@{{speculative=@{{backend='dflash2';draft_tokens=$draft}}}}
  $serverArguments=[Collections.Generic.List[string]]::new()
  $accepted=$true
  try{{{launch}}}catch{{$accepted=$false}}
  $results+=@{{accepted=$accepted;arguments=@($serverArguments.ToArray())}}
}}
ConvertTo-Json -InputObject @($results) -Depth 4 -Compress
""")
        self.assertEqual(observed, [
            {"accepted": False, "arguments": []},
            {"accepted": True, "arguments": [
                "--spec", "dflash2", "--draft-tokens", "7", "--lm-head-draft",
            ]},
            {"accepted": False, "arguments": []},
        ])

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

    def test_residency_ceiling_is_independent_of_wddm_commit(self) -> None:
        path = qualify_native.ps_quote(str(WINDOWS / "Control-GpuOwner.ps1"))
        observed = self.invoke(f"""
$ast=[Management.Automation.Language.Parser]::ParseFile({path},[ref]$null,[ref]$null)
foreach($name in @('Get-GpuMemoryUsedBytes','Get-InteractiveGpuWorkloadCount','Assert-InteractiveGpuWorkloadAbsent','Write-Status')){{
  $node=$ast.Find({{param($entry) $entry -is [Management.Automation.Language.FunctionDefinitionAst] -and $entry.Name -ceq $name}},$true)
  . ([scriptblock]::Create($node.Extent.Text))
}}
$GpuUuid='GPU-00000000-0000-4000-8000-000000000001'
$gpuPciBusId='00000000:01:00.0';$qualifiedPowerLimitW=0
$interactiveGpuMemoryThresholdBytes=1GB
function Invoke-NvidiaSmi([string[]]$Arguments){{$script:query=$Arguments;return $script:memoryMiB}}
function Get-WddmProcessMemoryDiagnostics{{throw 'diagnostic provider unavailable'}}
$script:memoryMiB='186';Assert-InteractiveGpuWorkloadAbsent
$idle=Write-Status $false 450|ConvertFrom-Json
$script:memoryMiB='1023';Assert-InteractiveGpuWorkloadAbsent
$script:memoryMiB='1024';$ceilingRejected=$false
try{{Assert-InteractiveGpuWorkloadAbsent}}catch{{$ceilingRejected=$_.Exception.Message -like '*active interactive workload*'}}
$invalidRejected=0
foreach($value in @('N/A','-1','1.5')){{
  $script:memoryMiB=$value
  try{{Get-GpuMemoryUsedBytes|Out-Null}}catch{{$invalidRejected++}}
}}
[ordered]@{{idle_bytes=$idle.interactive_gpu_memory_used_bytes;diagnostics=$idle.wddm_process_memory.status;ceiling_rejected=$ceilingRejected;invalid_rejected=$invalidRejected;query=$script:query}}|ConvertTo-Json -Compress
""")
        self.assertEqual(observed["idle_bytes"], 186 * 1024 * 1024)
        self.assertEqual(observed["diagnostics"], "unavailable")
        self.assertTrue(observed["ceiling_rejected"])
        self.assertEqual(observed["invalid_rejected"], 3)
        self.assertEqual(observed["query"][:2], ["-i", "GPU-00000000-0000-4000-8000-000000000001"])
        self.assertIn("--query-gpu=memory.used", observed["query"])

    def test_orchestrator_scratch_and_package_scripts_parse(self) -> None:
        class CapturedScript(Exception):
            pass

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subject = qualify_native.Orchestrator.__new__(qualify_native.Orchestrator)
            subject.config = SimpleNamespace(
                source_root=ROOT, state_dir=root, builder="not-contacted", target="not-contacted",
                dependency_sbom_tool="D:/P4/generate.py", vcpkg_resolved="D:/P4/vcpkg.json",
                cuda_inventory="D:/P4/cuda.json", third_party_inventory="D:/P4/third-party.json",
                runtime_inventory="D:/P4/runtime.json", scratch_port=18210, pressure_port=18200,
                scratch_state_root="C:/qualification/qwen38-4090-native", scratch_task_suffix="I6",
                api_key_file="C:/protected/key", model_path="C:/models/model.ninfer",
                qualified_runtime_bin="D:/qualified/bin", qualified_build_receipt="D:/qualified/build.json",
                gpu_uuid=None, long_fixture=root / "fixture.json", omp_root="C:/omp-fixture",
            )
            subject.lane = qualify_native.Lane(
                "rtx4090", json.loads((WINDOWS / "lanes/rtx4090/release-spec.json").read_text()),
                json.loads((WINDOWS / "lanes/rtx4090/server-config.json").read_text()),
            )
            subject.head = "a" * 40
            subject.tool_head = "f" * 40
            subject.head8 = "a" * 8
            subject.script = ROOT / "tools/qualification/qualify_native.py"
            subject.local_stage = root / "stage"
            subject.builder_root = "D:/overnight/C3/native-unit"
            subject.builder_source = subject.builder_root + "/source"
            subject.builder_bin = subject.builder_root + "/bin"
            subject.builder_script = subject.builder_root + "/qualify_native.py"
            subject.builder_out_a, subject.builder_out_b = "D:/C3/public-a", "D:/C3/public-b"
            subject.builder_scratch_a, subject.builder_scratch_b = "D:/C3/scratch-a", "D:/C3/scratch-b"
            subject.target_root = "C:/qualification"
            subject.state = {"gpu_uuid": "GPU-00000000-0000-4000-8000-000000004090",
                             "phases": {"package": {"receipt": {"scratch_package": {
                                 "package": {"sha256": "e" * 64}}}}}}
            scripts = []

            def capture(host: str, script: str, **kwargs: object) -> None:
                scripts.append(script)
                raise CapturedScript

            subject.remote_json = capture
            with patch.object(qualify_native, "run", return_value=SimpleNamespace(stdout="1700000000")), \
                    patch.object(qualify_native, "sha256_file", return_value="f" * 64), \
                    patch.object(qualify_native.subprocess, "check_output", return_value=b"unit-config"), \
                    patch.object(qualify_native, "scp"), patch.object(qualify_native, "remote_ps"):
                for method in ("consume_qualified_build", "package", "transfer_install", "protocol",
                               "pressure_protocol", "security", "omp", "restore"):
                    with self.subTest(method=method), self.assertRaises(CapturedScript):
                        getattr(subject, method)()
            self.assertIn(f"checkout --quiet --detach {subject.tool_head}", scripts[0])
            self.assertIn(f"--source-commit {subject.head}", scripts[0])
            self.assertIn(f"ReleaseHeadSha='{subject.tool_head}';RuntimeSourceSha='{subject.head}'", scripts[1])
            paths = []
            for index, script in enumerate(scripts):
                path = root / f"generated-{index}.ps1"
                path.write_text(script, encoding="utf-8")
                paths.append(path)
            literal = ",".join(qualify_native.ps_quote(str(path)) for path in paths)
            errors = self.invoke(f"""
$failures=@()
foreach($path in @({literal})){{
  $tokens=$null;$errors=$null
  [void][Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
  foreach($entry in $errors){{$failures+=([IO.Path]::GetFileName($path)+': '+$entry.Message)}}
}}
ConvertTo-Json -InputObject @($failures) -Compress
""")
            self.assertEqual(len(scripts), 8)
            self.assertEqual(errors, [])

    def test_schema4_and_rollback_arguments_execute_without_aliases(self) -> None:
        controller = (WINDOWS / "Control-Release.ps1").read_text()
        function = ps_function(controller, "Add-ContextArguments")
        result = self.invoke(function + """
$new=[Collections.Generic.List[string]]::new()
$old=[Collections.Generic.List[string]]::new()
$current='{"schema_version":4,"context_cache":{"host_context_mib":"14787.734375"},"engine":{"admission_headroom_mib":640},"session_checkpoint":{"require_origin_auth":true}}'|ConvertFrom-Json
$legacy='{"schema_version":3,"context_cache":{"host_state_slots":24,"host_kv_mib":11264,"max_private_continuations":8}}'|ConvertFrom-Json
Add-ContextArguments $new $current
Add-ContextArguments $old $legacy
$current.context_cache|Add-Member -NotePropertyName host_kv_mib -NotePropertyValue 1
$refused=$false
try{Add-ContextArguments ([Collections.Generic.List[string]]::new()) $current}catch{$refused=$true}
@{current=@($new);legacy=@($old);mixed_schema_refused=$refused}|ConvertTo-Json -Compress
""")
        self.assertEqual(result["current"], ["--host-context-mib", "14787.734375",
                                             "--admission-headroom-mib", "640",
                                             "--session-checkpoint-require-origin-auth"])
        self.assertEqual(result["legacy"], [
            "--host-state-slots", "24", "--max-private-continuations", "8", "--host-kv-mib", "11264",
        ])
        self.assertTrue(result["mixed_schema_refused"])

    def test_strict_origin_key_floor_counts_valid_utf8_bytes(self) -> None:
        function = ps_function((WINDOWS / "Install-Release.ps1").read_text(), "Assert-OneLineSecret")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures = (b"a" * 32, b"a" * 31, ("é" * 16).encode(), b"a" * 32 + b"\nb",
                        b"a" * 32 + b"\xff")
            for index, content in enumerate(fixtures):
                (root / str(index)).write_bytes(content)
            result = self.invoke(function + f"""
$accepted=@()
foreach($index in 0..4){{
  try{{Assert-OneLineSecret (Join-Path {qualify_native.ps_quote(str(root))} ([string]$index));$accepted+=$true}}
  catch{{$accepted+=$false}}
}}
ConvertTo-Json -InputObject @($accepted) -Compress
""")
        self.assertEqual(result, [True, False, True, False, False])

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

    def test_runtime_collector_copies_selected_cuda_crt_and_transitive_dlls(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            apps = build / "apps"
            vcpkg = build / "vcpkg_installed/x64-windows/bin"
            redist = root / "redist"
            crt = redist / "x64/Microsoft.VC143.CRT"
            cuda = root / "cuda/bin"
            bin_dir = root / "bin"
            rogue = root / "unselected-path"
            for path in (apps, vcpkg, crt, cuda / "x64", bin_dir, rogue):
                path.mkdir(parents=True)
            for name in ("ninfer.exe", "ninfer-serve.exe", "ninfer_bench.exe"):
                (bin_dir / name).write_bytes(b"synthetic-import-graph-not-a-native-binary")
            for name in qualify_native.DIRECTSTORAGE_DLLS:
                (apps / name).write_bytes(name.encode())
            (cuda / "x64/cudart64_13.dll").write_bytes(b"selected-cuda")
            (crt / "vcruntime140.dll").write_bytes(b"selected-crt")
            (crt / "msvcp140.dll").write_bytes(b"selected-cxx")
            (vcpkg / "vcruntime140.dll").write_bytes(b"wrong-toolchain-crt")
            (vcpkg / "libfixture.dll").write_bytes(b"vcpkg-dependency")
            (vcpkg / "libtransitive.dll").write_bytes(b"transitive-dependency")
            (rogue / "cudart64_13.dll").write_bytes(b"unselected-path-cuda")
            dumpbin = root / "synthetic-imports.ps1"
            dumpbin.write_text("""
$global:LASTEXITCODE=0
switch([IO.Path]::GetFileName([string]$args[-1])){
  'ninfer.exe' {'  cudart64_13.dll';'  vcruntime140.dll';'  libfixture.dll';'  kernel32.dll'}
  'dstoragecore.dll' {'  msvcp140.dll'}
  'libfixture.dll' {'  libtransitive.dll'}
}
""", encoding="utf-8")
            failed_dumpbin = root / "failed-import-inspection.ps1"
            failed_dumpbin.write_text("$global:LASTEXITCODE=9\n", encoding="utf-8")
            def quote(path: Path) -> str:
                return qualify_native.ps_quote(str(path))

            result = self.invoke(qualify_native.NATIVE_RUNTIME_DEPENDENCIES_PS + f"""
$build={quote(build)};$bin={quote(bin_dir)};$cuda={quote(cuda / 'nvcc.exe')};$redist={quote(redist)}
$names=@(Copy-NativeRuntimeDependencies $build $bin $cuda $redist {quote(dumpbin)} @('dstorage.dll','dstoragecore.dll'))
Remove-Item -LiteralPath {quote(cuda / 'x64/cudart64_13.dll')}
$env:PATH={quote(rogue)}+[IO.Path]::PathSeparator+$env:PATH
$missingRefused=$false;$inspectionRefused=$false
foreach($suffix in @('missing','inspection')){{
  $out=Join-Path {quote(root)} $suffix
  New-Item -ItemType Directory -Path $out|Out-Null
  foreach($exe in @('ninfer.exe','ninfer-serve.exe','ninfer_bench.exe')){{Copy-Item -LiteralPath (Join-Path $bin $exe) -Destination $out}}
  try{{
    $tool=if($suffix -eq 'missing'){{{quote(dumpbin)}}}else{{{quote(failed_dumpbin)}}}
    Copy-NativeRuntimeDependencies $build $out $cuda $redist $tool @('dstorage.dll','dstoragecore.dll')|Out-Null
  }}catch{{
    if($suffix -eq 'missing'){{$missingRefused=$_.Exception.Message -like '*app-local runtime DLL is unavailable: cudart64_13.dll*'}}
    else{{$inspectionRefused=$_.Exception.Message -like '*import inspection failed*'}}
  }}
}}
@{{names=$names;missing_selected_cuda_refused=$missingRefused;inspection_failure_refused=$inspectionRefused;system_dll_copied=(Test-Path (Join-Path $bin 'kernel32.dll'))}}|ConvertTo-Json -Compress
""")
            self.assertEqual(result["names"], sorted([
                "dstorage.dll", "dstoragecore.dll", "cudart64_13.dll", "vcruntime140.dll",
                "msvcp140.dll", "libfixture.dll", "libtransitive.dll",
            ]))
            self.assertTrue(result["missing_selected_cuda_refused"])
            self.assertTrue(result["inspection_failure_refused"])
            self.assertFalse(result["system_dll_copied"])
            self.assertEqual((bin_dir / "cudart64_13.dll").read_bytes(), b"selected-cuda")
            self.assertEqual((bin_dir / "vcruntime140.dll").read_bytes(), b"selected-crt")
            self.assertEqual((bin_dir / "libtransitive.dll").read_bytes(), b"transitive-dependency")


if __name__ == "__main__":
    unittest.main()
