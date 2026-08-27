from __future__ import annotations

import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
WORKFLOW = REPOSITORY / "tools" / "bench" / "run_sm120_mtp3_workflow.sh"
SOURCE_SHA = "0123456789abcdef0123456789abcdef01234567"
UPSTREAM_SHA = "89abcdef0123456789abcdef0123456789abcdef"
IMAGE_ID = "sha256:" + "b" * 64
RUNTIME_IMAGE_ID = "sha256:" + "d" * 64
DAEMON_ID = "mock-wsl-daemon"
PRODUCTION_ID = "c" * 64


def write_executable(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(content).lstrip(), encoding="utf-8")
    path.chmod(0o755)


class Sm120ProfileWorkflowTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = (self.root / "source").resolve()
        tools = self.source / "tools" / "bench"
        tools.mkdir(parents=True)
        shutil.copy2(WORKFLOW, tools / WORKFLOW.name)
        (tools / WORKFLOW.name).chmod(0o755)
        self.workflow = tools / WORKFLOW.name
        write_executable(
            tools / "run_sm120_mtp3_profile.sh",
            "#!/usr/bin/env bash\necho 'fake Docker must intercept the profile runner' >&2\nexit 97\n",
        )
        self.lease_log = self.root / "lease.log"
        write_executable(
            tools / "run_gpu_profile_lease.sh",
            r"""
            #!/usr/bin/env bash
            set -Eeuo pipefail
            mode=$1
            config=$2
            shift 2
            # shellcheck source=/dev/null
            source "$config"
            if [[ $mode == check ]]; then
              [[ $MOCK_LEASE_CHECK_FAIL != 1 ]] || exit 93
              [[ -x $PRODUCTION_CONTROLLER ]] || exit 93
              exit 0
            fi
            [[ $mode == run ]] || exit 92
            [[ -f $PROFILE_PREPARE_DIR/prepare-complete.txt ]] || exit 94
            printf '%s\n' "$@" >"$MOCK_LEASE_LOG"
            mkdir -p "$PROFILE_RESULT_DIR"
            : >"$PROFILE_RESULT_DIR/production-stopped-at.txt"
            marker="$PROFILE_RESULT_DIR/lease-active"
            printf '%s\n' "$$" >"$marker"
            printf '%s\n' false >"$MOCK_PRODUCTION_STATE"
            export NINFER_PROFILE_LEASE_ACTIVE=$marker
            "$@"
            status=$?
            rm -f -- "$marker"
            printf '%s\n' true >"$MOCK_PRODUCTION_STATE"
            printf '%s\n' "$status" >"$PROFILE_RESULT_DIR/exit-status.txt"
            : >"$PROFILE_RESULT_DIR/production-restored-at.txt"
            exit "$status"
            """,
        )

        self.production_state = self.root / "production-state"
        self.production_state.write_text("true\n", encoding="utf-8")
        self.controller = self.root / "production-controller"
        write_executable(self.controller, "#!/usr/bin/env bash\nexit 0\n")
        self.fake_bin = self.root / "fake-bin"
        self.fake_bin.mkdir()
        write_executable(
            self.fake_bin / "git",
            f"""
            #!/usr/bin/env bash
            set -euo pipefail
            if [[ ${{1:-}} == -C ]]; then
              shift 2
            fi
            case ${{1:-}} in
              rev-parse) printf '{SOURCE_SHA}\n' ;;
              status) ;;
              cat-file) ;;
              merge-base) ;;
              *) exit 90 ;;
            esac
            """,
        )
        self.docker_log = self.root / "docker.jsonl"
        self.docker_cli = self.fake_bin / "docker"
        write_executable(
            self.docker_cli,
            f"""
            #!{sys.executable}
            import json
            import os
            import sys
            from pathlib import Path

            log = Path(os.environ["MOCK_DOCKER_LOG"])
            args = sys.argv[1:]
            with log.open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(args) + "\\n")
            if args[:2] != ["--context", "default"]:
                raise SystemExit(f"unexpected context: {{args!r}}")
            args = args[2:]
            if args[:2] == ["context", "inspect"]:
                print("unix:///var/run/docker.sock")
                raise SystemExit(0)
            if args[0] == "info":
                print("{DAEMON_ID}")
                raise SystemExit(0)
            if args[0] == "inspect":
                if any(".Id" in value for value in args):
                    print("{PRODUCTION_ID}")
                elif any(".State.Running" in value for value in args):
                    print(Path(os.environ["MOCK_PRODUCTION_STATE"]).read_text().strip())
                else:
                    raise SystemExit(f"unsupported inspect: {{args!r}}")
                raise SystemExit(0)
            if args[:2] == ["image", "inspect"]:
                if args[2] == "runtime-candidate:fixed":
                    print(os.environ["MOCK_RUNTIME_IMAGE_ID"])
                else:
                    print("{IMAGE_ID}")
                raise SystemExit(0)
            if args[0] == "ps":
                raise SystemExit(0)
            if args[0] != "run":
                raise SystemExit(f"unsupported fake Docker command: {{args!r}}")

            mounts = []
            for index, value in enumerate(args[:-1]):
                if value == "--mount":
                    fields = dict(
                        field.split("=", 1)
                        for field in args[index + 1].split(",")
                        if "=" in field
                    )
                    mounts.append(fields)
            mount_by_target = {{mount["target"]: Path(mount["source"]) for mount in mounts}}
            if "--gpus" in args:
                result = mount_by_target["/workspace/result"] / "sm120-mtp3"
                (result / "ncu").mkdir(parents=True)
                (result / "ncu" / "counter-access-verified.txt").write_text("mock\\n")
                (result / "packet-complete.txt").write_text("mock\\n")
            else:
                prepare = mount_by_target["/workspace/prepare"]
                prepare.mkdir(parents=True, exist_ok=True)
                (prepare / "prepare-complete.txt").write_text("mock\\n")
            """,
        )

        self.profile_root = self.root / "operation"
        self.build = self.profile_root / "build" / "run"
        self.prepare = self.profile_root / "prepared" / "run"
        self.result = self.profile_root / "results" / "run"
        self.docker_config = self.profile_root / "docker-config" / "run"
        self.model = self.root / "model.ninfer"
        self.model.write_bytes(b"mock model")
        values = {
            "PROFILE_ROOT": self.profile_root,
            "PROFILE_SOURCE_ROOT": self.source,
            "PROFILE_SOURCE_SHA": SOURCE_SHA,
            "PROFILE_UPSTREAM_BASE_SHA": UPSTREAM_SHA,
            "PROFILE_BUILD_DIR": self.build,
            "PROFILE_PREPARE_DIR": self.prepare,
            "PROFILE_RESULT_DIR": self.result,
            "PROFILE_DOCKER_CONFIG": self.docker_config,
            "PROFILE_DOCKER_CLI": self.docker_cli,
            "PROFILE_DOCKER_CONTEXT": "default",
            "PROFILE_DOCKER_ENDPOINT": "unix:///var/run/docker.sock",
            "PROFILE_DOCKER_DAEMON_ID": DAEMON_ID,
            "TOOLCHAIN_IMAGE": "runtime-candidate:fixed",
            "PROFILE_TOOLCHAIN_IMAGE": "profile-toolchain:fixed",
            "PROFILE_TOOLCHAIN_IMAGE_ID": IMAGE_ID,
            "PROFILE_CONTAINER_PYTHON": "python3.11",
            "PROFILE_NCU_TIMEOUT_SECONDS": "180",
            "PROFILE_NSYS_TIMEOUT_SECONDS": "300",
            "CANDIDATE_PREFIX": "workflow-test-",
            "CANDIDATE_PORT": "18081",
            "MODEL_PATH": self.model,
            "MODEL_SHA256": hashlib.sha256(b"mock model").hexdigest(),
            "PROFILE_CONTROLLER_ID": "stable-controller",
            "PROFILE_TARGET_ID": "mock-5090",
            "PRODUCTION_CONTAINER": "production",
            "PRODUCTION_ID": PRODUCTION_ID,
            "PRODUCTION_IMAGE": "runtime-candidate:fixed",
            "PRODUCTION_RESTART_POLICY": "unless-stopped",
            "PRODUCTION_PORT": "8000",
            "PRODUCTION_CONTROLLER": self.controller,
            "ROLLBACK_CONTAINER": "production-rollback",
            "ROLLBACK_ID": "f" * 64,
            "ROLLBACK_IMAGE_ID": "sha256:" + "e" * 64,
            "ROLLBACK_RUNNING": "false",
            "ROLLBACK_RESTART_POLICY": "no",
        }
        self.config = self.root / "profile.conf"
        self.config.write_text(
            "\n".join(f"{key}={shlex.quote(str(value))}" for key, value in values.items()) + "\n",
            encoding="utf-8",
        )
        self.environment = {
            **os.environ,
            "PATH": f"{self.fake_bin}:{os.environ['PATH']}",
            "PYTHON": sys.executable,
            "MOCK_DOCKER_LOG": str(self.docker_log),
            "MOCK_LEASE_LOG": str(self.lease_log),
            "MOCK_PRODUCTION_STATE": str(self.production_state),
            "MOCK_LEASE_CHECK_FAIL": "0",
            "MOCK_RUNTIME_IMAGE_ID": RUNTIME_IMAGE_ID,
        }

    def run_workflow(self, mode: str, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [self.workflow, mode, self.config, *extra],
            cwd=self.source,
            env=self.environment,
            text=True,
            capture_output=True,
            check=False,
            timeout=10,
        )

    def docker_calls(self) -> list[list[str]]:
        if not self.docker_log.exists():
            return []
        return [
            json.loads(line)
            for line in self.docker_log.read_text(encoding="utf-8").splitlines()
        ]

    def test_check_is_read_only_and_pins_existing_wsl_daemon(self) -> None:
        completed = self.run_workflow("check")

        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertIn("profile workflow check passed", completed.stdout)
        for path in (self.build, self.prepare, self.result, self.docker_config):
            self.assertFalse(path.exists(), path)
        commands = [call[2] for call in self.docker_calls()]
        self.assertEqual(commands, ["context", "info", "image", "image", "ps"])
        self.assertNotIn("run", commands)

    def test_check_rejects_unusable_restoration_controller_before_docker(self) -> None:
        self.controller.chmod(0o644)

        completed = self.run_workflow("check")

        self.assertEqual(completed.returncode, 2)
        self.assertIn("PRODUCTION_CONTROLLER is not executable", completed.stderr)
        self.assertFalse(self.docker_log.exists())
        for path in (self.build, self.prepare, self.result, self.docker_config):
            self.assertFalse(path.exists(), path)

    def test_check_rejects_shared_runtime_and_profile_image_before_docker(self) -> None:
        with self.config.open("a", encoding="utf-8") as stream:
            stream.write("PROFILE_TOOLCHAIN_IMAGE=runtime-candidate:fixed\n")

        completed = self.run_workflow("check")

        self.assertEqual(completed.returncode, 2)
        self.assertIn("profiling and runtime candidate images must be distinct", completed.stderr)
        self.assertFalse(self.docker_log.exists())
        for path in (self.build, self.prepare, self.result, self.docker_config):
            self.assertFalse(path.exists(), path)

    def test_check_rejects_tags_resolving_to_same_image_id(self) -> None:
        self.environment["MOCK_RUNTIME_IMAGE_ID"] = IMAGE_ID

        completed = self.run_workflow("check")

        self.assertEqual(completed.returncode, 2)
        self.assertIn("resolve to the same image ID", completed.stderr)
        commands = [call[2] for call in self.docker_calls()]
        self.assertEqual(commands, ["context", "info", "image", "image"])
        for path in (self.build, self.prepare, self.result, self.docker_config):
            self.assertFalse(path.exists(), path)

    def test_run_stops_before_prepare_when_lease_preflight_fails(self) -> None:
        self.environment["MOCK_LEASE_CHECK_FAIL"] = "1"

        completed = self.run_workflow("run")

        self.assertEqual(completed.returncode, 93, completed.stderr + completed.stdout)
        self.assertFalse(self.build.exists())
        self.assertFalse(self.prepare.exists())
        self.assertFalse(self.result.exists())
        self.assertFalse(self.docker_config.exists())
        commands = [call[2] for call in self.docker_calls()]
        self.assertEqual(commands, ["context", "info", "image", "image"])
        self.assertNotIn("run", commands)

    def test_run_uses_fixed_off_gpu_prepare_then_leased_gpu_payload(self) -> None:
        completed = self.run_workflow("run")

        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertEqual(
            self.lease_log.read_text(encoding="utf-8").splitlines(),
            [str(self.workflow), "payload", str(self.config.resolve())],
        )
        run_calls = [call[2:] for call in self.docker_calls() if call[2] == "run"]
        self.assertEqual(len(run_calls), 2)
        self.assertTrue(
            any(
                call[2:5] == ["image", "inspect", "profile-toolchain:fixed"]
                for call in self.docker_calls()
            )
        )
        self.assertTrue(
            any(
                call[2:5] == ["image", "inspect", "runtime-candidate:fixed"]
                for call in self.docker_calls()
            )
        )
        self.assertFalse(
            any(
                call[2] == "run" and "runtime-candidate:fixed" in call
                for call in self.docker_calls()
            )
        )
        prepare_call, payload_call = run_calls
        self.assertNotIn("--gpus", prepare_call)
        self.assertNotIn("--device", prepare_call)
        self.assertIn("--network", prepare_call)
        self.assertIn("--gpus", payload_call)
        self.assertIn("all", payload_call)
        self.assertIn("--cap-add", payload_call)
        self.assertIn("SYS_ADMIN", payload_call)
        self.assertEqual(
            json.loads((self.docker_config / "config.json").read_text(encoding="utf-8")),
            {},
        )
        self.assertTrue((self.prepare / "workflow-preflight.tsv").is_file())
        self.assertTrue((self.result / "exit-status.txt").is_file())
        self.assertTrue((self.result / "production-restored-at.txt").is_file())
        self.assertFalse((self.result / "lease-active").exists())
        self.assertTrue((self.result / "sm120-mtp3" / "packet-complete.txt").is_file())

    def test_payload_rejects_live_but_unrelated_stale_marker(self) -> None:
        initial = self.run_workflow("run")
        self.assertEqual(initial.returncode, 0, initial.stderr + initial.stdout)
        sleeper = subprocess.Popen(["/bin/sleep", "30"])
        self.addCleanup(sleeper.wait)
        self.addCleanup(lambda: sleeper.poll() is not None or sleeper.kill())
        marker = self.result / "lease-active"
        marker.write_text(f"{sleeper.pid}\n", encoding="utf-8")
        environment = {**self.environment, "NINFER_PROFILE_LEASE_ACTIVE": str(marker)}
        gpu_runs_before = sum("--gpus" in call for call in self.docker_calls())

        completed = subprocess.run(
            [self.workflow, "payload", self.config],
            cwd=self.source,
            env=environment,
            text=True,
            capture_output=True,
            check=False,
            timeout=10,
        )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("not owned by its active profile lease", completed.stderr)
        self.assertEqual(
            sum("--gpus" in call for call in self.docker_calls()),
            gpu_runs_before,
        )

    def test_payload_rejects_forged_active_marker_while_production_runs(self) -> None:
        initial = self.run_workflow("run")
        self.assertEqual(initial.returncode, 0, initial.stderr + initial.stdout)
        gpu_runs_before = sum("--gpus" in call for call in self.docker_calls())
        marker = self.result / "lease-active"
        marker.write_text(f"{os.getpid()}\n", encoding="utf-8")
        environment = {**self.environment, "NINFER_PROFILE_LEASE_ACTIVE": str(marker)}

        completed = subprocess.run(
            [self.workflow, "payload", self.config],
            cwd=self.source,
            env=environment,
            text=True,
            capture_output=True,
            check=False,
            timeout=10,
        )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("production must be stopped by the active lease", completed.stderr)
        self.assertEqual(
            sum("--gpus" in call for call in self.docker_calls()),
            gpu_runs_before,
        )

    def test_rejects_an_injected_payload_without_touching_docker(self) -> None:
        completed = self.run_workflow("run", "/tmp/injected-payload")

        self.assertEqual(completed.returncode, 2)
        self.assertIn("usage:", completed.stderr)
        self.assertFalse(self.docker_log.exists())
        for path in (self.build, self.prepare, self.result, self.docker_config):
            self.assertFalse(path.exists(), path)


if __name__ == "__main__":
    unittest.main()
