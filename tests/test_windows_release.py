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

    def test_flags_newer_than_the_shipped_lineage_are_capability_gated(self) -> None:
        """The shared controller launches every installed release, including the shipped v0.6.0
        one after a rollback, and an older ninfer-serve.exe refuses an argument it never learned
        (`unknown argument`) and does not start. So a flag the runtime gained after that release
        may reach a server only inside the branch that checks the release's declared capability.
        Measured twice on the RTX 4090 host - once each for --stop-event and --shutdown-report."""
        shipped_parser = subprocess.run(
            ["git", "show", "075d442e:src/serve/serve_options.cpp"],
            capture_output=True, text=True, check=True, cwd=ROOT,
        ).stdout
        shipped_flags = set(re.findall(r'arg == "(--[a-z0-9-]+)"', shipped_parser))
        parser = (ROOT / "src/serve/serve_options.cpp").read_text(encoding="utf-8")
        newer_flags = set(re.findall(r'arg == "(--[a-z0-9-]+)"', parser)) - shipped_flags
        self.assertEqual(newer_flags, {"--stop-event", "--shutdown-report"})
        controller = (WINDOWS / "Control-Release.ps1").read_text(encoding="utf-8")
        start = controller.index("$serverArguments =")
        end = controller.index("$argumentLine =", start)
        launch = controller[start:end]
        gate = launch.index("if ([string]$stopPlan.mode -ceq 'stop-event') {")
        gated = launch[gate:]
        gated = gated[:gated.index("\n        }\n")]
        for flag in newer_flags:
            with self.subTest(flag=flag):
                self.assertEqual(launch.count(f"'{flag}'"), 1)
                self.assertIn(f"'{flag}'", gated)

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
                        run.index("Start-Process -FilePath"))
        # One prior launch's logs survive, because they are what explains a stop.
        self.assertIn('Move-Item -LiteralPath $log -Destination "$log.previous"', run)

    def test_the_server_reports_a_shutdown_that_lost_state(self) -> None:
        """The manager can only record what the server tells it: a flush that could not save a
        live session must fail the exit, not log and return zero."""
        flush = (ROOT / "src/serve/http_server.cpp").read_text(encoding="utf-8")
        start = flush.index("ShutdownCheckpointSummary HttpServer::save_all_checkpoints")
        flush = flush[start:flush.index("\nbool HttpServer::bind", start)]
        self.assertIn("++summary.refused", flush)
        self.assertIn("++summary.skipped", flush)
        main = (ROOT / "apps/serve/main.cpp").read_text(encoding="utf-8")
        self.assertRegex(main, r"if \(!flushed\.complete\(\)\) \{(.|\n)*?return 1;")

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
