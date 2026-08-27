from __future__ import annotations

import json
import os
import shlex
import signal
import socket
import subprocess
import tempfile
import time
import unittest
from collections.abc import Callable
from pathlib import Path

from tools.bench.summarize_profile_ab import SummaryError, summarize


REPO_ROOT = Path(__file__).resolve().parents[1]
LEASE_SCRIPT = REPO_ROOT / "tools" / "bench" / "run_gpu_profile_lease.sh"
PAIR_RUNS = (
    ("baseline", "baseline-a"),
    ("candidate", "candidate-a"),
    ("candidate", "candidate-b"),
    ("baseline", "baseline-b"),
    ("baseline", "baseline-c"),
    ("candidate", "candidate-c"),
)


def write_executable(path: Path, content: str) -> None:
    path.write_text(content, encoding="utf-8")
    path.chmod(0o755)


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_until(predicate: Callable[[], bool], timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("condition was not reached before timeout")


def lease_fixture(
    tmp_path: Path, *, controller: bool, stop_timeout_seconds: int = 1
) -> tuple[Path, dict[str, str], Path, Path]:
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    state = tmp_path / "production-running"
    state.write_text("true\n", encoding="utf-8")
    docker_log = tmp_path / "docker.log"
    controller_log = tmp_path / "controller.log"
    candidate_state = tmp_path / "candidate-running"

    write_executable(
        bin_dir / "docker",
        """#!/usr/bin/env python3
import os
import sys
from pathlib import Path

state = Path(os.environ["FAKE_PRODUCTION_STATE"])
log = Path(os.environ["FAKE_DOCKER_LOG"])
candidate_state = Path(os.environ["FAKE_CANDIDATE_STATE"])
args = sys.argv[1:]
if args[:2] != ["--context", "default"]:
    raise SystemExit(f"unexpected fake docker context: {args!r}")
args = args[2:]


def record(value: str) -> None:
    with log.open("a", encoding="utf-8") as stream:
        stream.write(value + "\\n")


if args[:2] == ["context", "inspect"]:
    print("unix:///var/run/docker.sock")
    raise SystemExit(0)
if args[0] == "info":
    if "{{json .Runtimes}}" in args:
        print('{}' if os.environ.get("FAKE_NO_NVIDIA_RUNTIME") == "1" else '{"nvidia": {}}')
    else:
        print("fake-daemon-id")
    raise SystemExit(0)
if args[0] == "inspect":
    fmt, container = args[2], args[3]
    if container == "production":
        values = {
            "{{.Id}}": "production-id",
            "{{.Config.Image}}": "production:image",
            "{{.HostConfig.RestartPolicy.Name}}": "unless-stopped",
            "{{.State.Running}}": state.read_text(encoding="utf-8").strip(),
        }
    elif container == "rollback":
        values = {
            "{{.Id}}": "rollback-id",
            "{{.Image}}": "sha256:rollback-image",
            "{{.HostConfig.RestartPolicy.Name}}": "no",
            "{{.State.Running}}": "false",
        }
    else:
        raise SystemExit(1)
    print(values.get(fmt, "snapshot"))
    raise SystemExit(0)
if args[0] == "ps":
    if candidate_state.exists():
        print("candidate-id")
    raise SystemExit(0)
if args[0] == "stop":
    record("stop")
    state.write_text("false\\n", encoding="utf-8")
    raise SystemExit(0)
if args[0] == "rm":
    record("remove")
    if os.environ.get("FAKE_REMOVE_FAIL") == "1":
        raise SystemExit(1)
    candidate_state.unlink(missing_ok=True)
    raise SystemExit(0)
raise SystemExit(f"unsupported fake docker command: {args!r}")
""",
    )
    write_executable(bin_dir / "curl", "#!/usr/bin/env bash\nexit 0\n")
    write_executable(
        bin_dir / "nvidia-smi",
        "#!/usr/bin/env bash\n[[ ${FAKE_NVIDIA_SMI_FAIL:-0} != 1 ]] || exit 88\necho 'fake gpu'\n",
    )
    write_executable(
        bin_dir / "setsid",
        """#!/usr/bin/env python3
import os
import sys
if sys.argv[1] == "--":
    del sys.argv[1]
os.setsid()
os.execvp(sys.argv[1], sys.argv[1:])
""",
    )

    controller_path = tmp_path / "production-controller"
    if controller:
        write_executable(
            controller_path,
            """#!/usr/bin/env python3
import os
import sys
from pathlib import Path
state = Path(os.environ["FAKE_PRODUCTION_STATE"])
log = Path(os.environ["FAKE_CONTROLLER_LOG"])
with log.open("a", encoding="utf-8") as stream:
    stream.write(sys.argv[1] + "\\n")
if sys.argv[1] == "restart":
    state.write_text("true\\n", encoding="utf-8")
elif sys.argv[1] == "status":
    print("healthy")
else:
    raise SystemExit(2)
""",
        )

    result_dir = tmp_path / "result"
    docker_config = tmp_path / "docker-config"
    docker_config.mkdir()
    docker_config.joinpath("config.json").write_text("{}\n", encoding="utf-8")
    config_values = {
        "PROFILE_RESULT_DIR": result_dir,
        "PROFILE_DOCKER_CLI": bin_dir / "docker",
        "PROFILE_DOCKER_CONTEXT": "default",
        "PROFILE_DOCKER_ENDPOINT": "unix:///var/run/docker.sock",
        "PROFILE_DOCKER_DAEMON_ID": "fake-daemon-id",
        "PROFILE_DOCKER_CONFIG": docker_config,
        "CANDIDATE_PREFIX": "profile-test-",
        "CANDIDATE_PORT": free_port(),
        "PRODUCTION_CONTAINER": "production",
        "PRODUCTION_ID": "production-id",
        "PRODUCTION_IMAGE": "production:image",
        "PRODUCTION_RESTART_POLICY": "unless-stopped",
        "PRODUCTION_PORT": free_port(),
        "PRODUCTION_CONTROLLER": controller_path,
        "ROLLBACK_CONTAINER": "rollback",
        "ROLLBACK_ID": "rollback-id",
        "ROLLBACK_IMAGE_ID": "sha256:rollback-image",
        "ROLLBACK_RUNNING": "false",
        "ROLLBACK_RESTART_POLICY": "no",
        "PROFILE_PAYLOAD_STOP_TIMEOUT_SECONDS": stop_timeout_seconds,
    }
    config = tmp_path / "profile.conf"
    config.write_text(
        "\n".join(f"{key}={shlex.quote(str(value))}" for key, value in config_values.items()) + "\n",
        encoding="utf-8",
    )
    env = {
        **os.environ,
        "PATH": f"{bin_dir}:{os.environ['PATH']}",
        "FAKE_PRODUCTION_STATE": str(state),
        "FAKE_DOCKER_LOG": str(docker_log),
        "FAKE_CANDIDATE_STATE": str(candidate_state),
        "FAKE_CONTROLLER_LOG": str(controller_log),
    }
    return config, env, state, docker_log


def write_campaign(
    result_dir: Path,
    *,
    request_drift_label: str | None = None,
    binary_drift_label: str | None = None,
    output_drift_label: str | None = None,
) -> None:
    e2e = result_dir / "e2e"
    e2e.mkdir(parents=True)
    e2e.joinpath("order.tsv").write_text(
        "".join(f"{variant}\t{label}\n" for variant, label in PAIR_RUNS), encoding="utf-8"
    )
    for index, (variant, label) in enumerate(PAIR_RUNS):
        run_dir = result_dir / "runs" / f"{label}-{variant}"
        run_dir.mkdir(parents=True)
        role = "baseline" if label.startswith("baseline-") else "candidate"
        binary_sha = ("a" if role == "baseline" else "b") * 64
        if label == binary_drift_label:
            binary_sha = "c" * 64
        server_start = {
            "engine": {"max_context": 131072, "prefill_chunk": 1024, "draft_tokens": 3},
            "identity": {
                "binary_sha256": binary_sha,
                "model_artifact_sha256": "d" * 64,
                "deployment_profile": f"profile-{label}",
                "target": "qwen3.8-27b",
                "model_id": "q38-ninfer",
                "weights_id": "groupwise-int",
            },
            "memory": {"device_bytes": 1},
        }
        run_dir.joinpath("server-start.json").write_text(
            json.dumps(server_start), encoding="utf-8"
        )
        for workload, filename in (("long", "long-prefill.json"), ("mtp", "mtp3-decode.json")):
            request = {
                "model": "q38-ninfer",
                "messages": [{"role": "user", "content": f"fixed-{workload}"}],
                "max_completion_tokens": 128 if workload == "long" else 65536,
                "stream": False,
                "enable_thinking": workload == "mtp",
                "seed": 7632647173703959000,
            }
            if label == request_drift_label and workload == "mtp":
                request["seed"] = 1
            baseline_rate = 100.0 + index
            rate = baseline_rate + (20.0 if role == "candidate" else 0.0)
            message_sha = "e" * 64
            if label == output_drift_label and workload == "mtp":
                message_sha = "f" * 64
            summary = {
                "exact_match": True,
                "prompt_tokens": 131072 if workload == "long" else 1024,
                "completion_tokens": 128 if workload == "long" else 4096,
                "wall_seconds": 2.0,
                "prefill_tokens_per_second": rate,
                "decode_tokens_per_second": rate,
                "message_sha256": message_sha,
            }
            record = {
                "summary": summary,
                "request": request,
                "response": {},
                "server_event": {"event": "request_done"},
            }
            run_dir.joinpath(filename).write_text(json.dumps(record), encoding="utf-8")


class ProfileHarnessTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.tmp_path = Path(self.temporary_directory.name)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def test_lease_rejects_missing_controller_before_stopping_production(self) -> None:
        config, env, state, docker_log = lease_fixture(self.tmp_path, controller=False)

        completed = subprocess.run(
            [LEASE_SCRIPT, "run", config, "/usr/bin/true"],
            cwd=REPO_ROOT,
            env=env,
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("not an executable file", completed.stderr)
        self.assertEqual(state.read_text(encoding="utf-8").strip(), "true")
        self.assertFalse(docker_log.exists())

    def test_lease_rejects_daemon_identity_drift_before_stopping_production(self) -> None:
        config, env, state, docker_log = lease_fixture(self.tmp_path, controller=True)
        config.write_text(
            config.read_text(encoding="utf-8").replace(
                "PROFILE_DOCKER_DAEMON_ID=fake-daemon-id",
                "PROFILE_DOCKER_DAEMON_ID=wrong-daemon-id",
            ),
            encoding="utf-8",
        )

        completed = subprocess.run(
            [LEASE_SCRIPT, "run", config, "/usr/bin/true"],
            cwd=REPO_ROOT,
            env=env,
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("differs from PROFILE_DOCKER_DAEMON_ID", completed.stderr)
        self.assertEqual(state.read_text(encoding="utf-8").strip(), "true")
        self.assertFalse(docker_log.exists())
        self.assertFalse((self.tmp_path / "result").exists())

    def test_lease_check_validates_restore_route_without_mutation(self) -> None:
        config, env, state, docker_log = lease_fixture(self.tmp_path, controller=True)

        completed = subprocess.run(
            [LEASE_SCRIPT, "check", config],
            cwd=REPO_ROOT,
            env=env,
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )

        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertIn("profile lease check passed", completed.stdout)
        self.assertEqual(state.read_text(encoding="utf-8").strip(), "true")
        self.assertFalse(docker_log.exists())
        self.assertFalse((self.tmp_path / "result").exists())

    def test_lease_rejects_missing_nvidia_smi_before_stopping_production(self) -> None:
        config, env, state, docker_log = lease_fixture(self.tmp_path, controller=True)
        Path(env["PATH"].split(":", 1)[0], "nvidia-smi").unlink()

        completed = subprocess.run(
            [LEASE_SCRIPT, "run", config, "/usr/bin/true"],
            cwd=REPO_ROOT,
            env=env,
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("nvidia-smi is required", completed.stderr)
        self.assertEqual(state.read_text(encoding="utf-8").strip(), "true")
        self.assertFalse(docker_log.exists())
        self.assertFalse((self.tmp_path / "result").exists())

    def test_lease_rejects_missing_nvidia_runtime_before_stopping_production(self) -> None:
        config, env, state, _docker_log = lease_fixture(self.tmp_path, controller=True)
        env["FAKE_NO_NVIDIA_RUNTIME"] = "1"

        completed = subprocess.run(
            [LEASE_SCRIPT, "check", config],
            cwd=REPO_ROOT,
            env=env,
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )

        self.assertEqual(completed.returncode, 1)
        self.assertIn("has no nvidia runtime", completed.stderr)
        self.assertEqual(state.read_text(encoding="utf-8").strip(), "true")
        self.assertFalse((self.tmp_path / "result").exists())

    def test_lease_rejects_failed_nvidia_query_before_stopping_production(self) -> None:
        config, env, state, docker_log = lease_fixture(self.tmp_path, controller=True)
        env["FAKE_NVIDIA_SMI_FAIL"] = "1"

        completed = subprocess.run(
            [LEASE_SCRIPT, "run", config, "/usr/bin/true"],
            cwd=REPO_ROOT,
            env=env,
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("host nvidia-smi query failed before the profile outage", completed.stderr)
        self.assertEqual(state.read_text(encoding="utf-8").strip(), "true")
        self.assertFalse(docker_log.exists())
        self.assertFalse((self.tmp_path / "result").exists())

    def test_lease_marks_the_fixed_payload_boundary_and_restores_production(self) -> None:
        config, env, state, _docker_log = lease_fixture(self.tmp_path, controller=True)
        observed_marker = self.tmp_path / "observed-marker"
        env["FAKE_OBSERVED_MARKER"] = str(observed_marker)
        payload = self.tmp_path / "fixed-payload"
        write_executable(
            payload,
            """#!/usr/bin/env python3
import os
from pathlib import Path

Path(os.environ["FAKE_OBSERVED_MARKER"]).write_text(
    os.environ["NINFER_PROFILE_LEASE_ACTIVE"], encoding="utf-8"
)
""",
        )

        completed = subprocess.run(
            [LEASE_SCRIPT, "run", config, payload],
            cwd=REPO_ROOT,
            env=env,
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )

        result_dir = self.tmp_path / "result"
        self.assertEqual(completed.returncode, 0, completed.stderr + completed.stdout)
        self.assertEqual(
            observed_marker.read_text(encoding="utf-8"),
            str(result_dir / "lease-active"),
        )
        self.assertTrue((result_dir / "production-stopped-at.txt").is_file())
        self.assertFalse((result_dir / "lease-active").exists())
        self.assertTrue((result_dir / "production-restored-at.txt").is_file())
        self.assertEqual(state.read_text(encoding="utf-8").strip(), "true")

    def test_lease_blocks_restart_when_candidate_removal_fails(self) -> None:
        config, env, state, _docker_log = lease_fixture(self.tmp_path, controller=True)
        env["FAKE_REMOVE_FAIL"] = "1"
        payload = self.tmp_path / "leave-candidate"
        write_executable(
            payload,
            """#!/usr/bin/env python3
import os
from pathlib import Path

Path(os.environ["FAKE_CANDIDATE_STATE"]).write_text("running\\n", encoding="utf-8")
""",
        )

        completed = subprocess.run(
            [LEASE_SCRIPT, "run", config, payload],
            cwd=REPO_ROOT,
            env=env,
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )

        result_dir = self.tmp_path / "result"
        self.assertEqual(completed.returncode, 90, completed.stderr + completed.stdout)
        self.assertEqual(state.read_text(encoding="utf-8").strip(), "false")
        self.assertTrue((result_dir / "candidate-removal-failed.txt").is_file())
        self.assertTrue((result_dir / "production-restore-failed-at.txt").is_file())
        self.assertFalse((result_dir / "production-restored-at.txt").exists())
        controller_calls = Path(env["FAKE_CONTROLLER_LOG"]).read_text(encoding="utf-8")
        self.assertNotIn("restart", controller_calls.splitlines())

    def test_lease_signal_terminates_payload_and_restores_production(self) -> None:
        cases = (
            (signal.SIGHUP, 129, "HUP"),
            (signal.SIGINT, 130, "INT"),
            (signal.SIGTERM, 143, "TERM"),
        )
        for index, (sent_signal, expected_status, signal_name) in enumerate(cases):
            with self.subTest(signal=signal_name):
                case_path = self.tmp_path / f"signal-{index}"
                case_path.mkdir()
                config, env, state, docker_log = lease_fixture(
                    case_path, controller=True, stop_timeout_seconds=4
                )
                payload_pid = case_path / "payload.pid"
                payload_signal = case_path / "payload.signal"
                env["FAKE_PAYLOAD_PID"] = str(payload_pid)
                env["FAKE_PAYLOAD_SIGNAL"] = str(payload_signal)
                payload = case_path / "payload"
                write_executable(
                    payload,
                    """#!/usr/bin/env python3
import os
import signal
from pathlib import Path


def stop(signum: int, _frame: object) -> None:
    Path(os.environ["FAKE_PAYLOAD_SIGNAL"]).write_text(signal.Signals(signum).name.removeprefix("SIG"))
    raise SystemExit(128 + signum)


Path(os.environ["FAKE_PAYLOAD_PID"]).write_text(str(os.getpid()))
for handled in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
    signal.signal(handled, stop)
while True:
    signal.pause()
""",
                )

                process = subprocess.Popen(
                    [LEASE_SCRIPT, "run", config, payload],
                    cwd=REPO_ROOT,
                    env=env,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
                try:
                    wait_until(
                        lambda: payload_pid.exists()
                        and docker_log.exists()
                        and "stop" in docker_log.read_text(encoding="utf-8")
                    )
                    signal_started = time.monotonic()
                    process.send_signal(sent_signal)
                    try:
                        returncode = process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        if payload_pid.exists():
                            try:
                                os.killpg(int(payload_pid.read_text(encoding="utf-8")), signal.SIGKILL)
                            except ProcessLookupError:
                                pass
                        process.wait(timeout=2)
                        self.fail("lease did not preempt the running payload")
                    signal_elapsed = time.monotonic() - signal_started
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=2)

                result_dir = case_path / "result"
                self.assertEqual(returncode, expected_status)
                self.assertLess(signal_elapsed, 2.0)
                self.assertEqual(payload_signal.read_text(encoding="utf-8"), signal_name)
                self.assertEqual(state.read_text(encoding="utf-8").strip(), "true")
                self.assertEqual(
                    (result_dir / "exit-status.txt").read_text(encoding="utf-8").strip(),
                    str(expected_status),
                )

    def test_lease_signal_kills_payload_descendant_group(self) -> None:
        config, env, state, _docker_log = lease_fixture(self.tmp_path, controller=True)
        payload_pid = self.tmp_path / "payload.pid"
        child_pid = self.tmp_path / "child.pid"
        payload_signal = self.tmp_path / "payload.signal"
        env.update(
            FAKE_PAYLOAD_PID=str(payload_pid),
            FAKE_CHILD_PID=str(child_pid),
            FAKE_PAYLOAD_SIGNAL=str(payload_signal),
        )
        payload = self.tmp_path / "payload-with-child"
        write_executable(
            payload,
            """#!/usr/bin/env python3
import os
import signal
import time
from pathlib import Path

child = os.fork()
if child == 0:
    Path(os.environ["FAKE_CHILD_PID"]).write_text(str(os.getpid()))
    for ignored in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(ignored, signal.SIG_IGN)
    while True:
        time.sleep(1)

Path(os.environ["FAKE_PAYLOAD_PID"]).write_text(str(os.getpid()))

def stop(signum: int, _frame: object) -> None:
    Path(os.environ["FAKE_PAYLOAD_SIGNAL"]).write_text(signal.Signals(signum).name.removeprefix("SIG"))
    raise SystemExit(128 + signum)

for handled in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
    signal.signal(handled, stop)
signal.pause()
""",
        )
        process = subprocess.Popen(
            [LEASE_SCRIPT, "run", config, payload],
            cwd=REPO_ROOT,
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            wait_until(lambda: payload_pid.exists() and child_pid.exists())
            process.send_signal(signal.SIGTERM)
            self.assertEqual(process.wait(timeout=5), 143)
            child = int(child_pid.read_text(encoding="utf-8"))
            wait_until(lambda: self._process_gone(child), timeout=5)
            self.assertEqual(state.read_text(encoding="utf-8").strip(), "true")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=2)

    def test_lease_normal_exit_kills_payload_descendant_group(self) -> None:
        config, env, state, _docker_log = lease_fixture(self.tmp_path, controller=True)
        child_pid = self.tmp_path / "normal-child.pid"
        env["FAKE_CHILD_PID"] = str(child_pid)
        payload = self.tmp_path / "normal-payload-with-child"
        write_executable(
            payload,
            """#!/usr/bin/env python3
import os
import signal
import time
from pathlib import Path

child = os.fork()
if child == 0:
    Path(os.environ["FAKE_CHILD_PID"]).write_text(str(os.getpid()))
    for ignored in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(ignored, signal.SIG_IGN)
    while True:
        time.sleep(1)
raise SystemExit(0)
""",
        )
        process = subprocess.Popen(
            [LEASE_SCRIPT, "run", config, payload],
            cwd=REPO_ROOT,
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            self.assertEqual(process.wait(timeout=5), 0)
            child = int(child_pid.read_text(encoding="utf-8"))
            wait_until(lambda: self._process_gone(child), timeout=5)
            self.assertEqual(state.read_text(encoding="utf-8").strip(), "true")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=2)

    def test_lease_signal_force_kills_unresponsive_payload(self) -> None:
        config, env, state, _docker_log = lease_fixture(self.tmp_path, controller=True)
        payload_pid = self.tmp_path / "payload.pid"
        env["FAKE_PAYLOAD_PID"] = str(payload_pid)
        payload = self.tmp_path / "unresponsive-payload"
        write_executable(
            payload,
            """#!/usr/bin/env python3
import os
import signal
import time
from pathlib import Path

Path(os.environ["FAKE_PAYLOAD_PID"]).write_text(str(os.getpid()))
for ignored in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
    signal.signal(ignored, signal.SIG_IGN)
while True:
    time.sleep(1)
""",
        )
        process = subprocess.Popen(
            [LEASE_SCRIPT, "run", config, payload],
            cwd=REPO_ROOT,
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            wait_until(lambda: payload_pid.exists())
            started = time.monotonic()
            process.send_signal(signal.SIGTERM)
            self.assertEqual(process.wait(timeout=5), 143)
            self.assertLess(time.monotonic() - started, 4.0)
            self.assertEqual(state.read_text(encoding="utf-8").strip(), "true")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=2)

    def test_summary_accepts_one_state_faithful_campaign(self) -> None:
        write_campaign(self.tmp_path)

        summary = summarize(self.tmp_path)

        self.assertIs(summary["long_prefill_tokens_per_second"]["credible_positive"], True)
        self.assertIs(summary["mtp3_decode_tokens_per_second"]["credible_positive"], True)

    def test_summary_rejects_cross_trial_identity_drift(self) -> None:
        cases = (
            ({"request_drift_label": "candidate-c"}, "request payloads differ"),
            ({"binary_drift_label": "candidate-c"}, "candidate binary identity differs"),
            ({"output_drift_label": "candidate-c"}, "MTP3 output hashes differ"),
        )
        for index, (drift, message) in enumerate(cases):
            with self.subTest(drift=drift):
                result_dir = self.tmp_path / f"campaign-{index}"
                write_campaign(result_dir, **drift)
                with self.assertRaisesRegex(SummaryError, message):
                    summarize(result_dir)

    def test_summary_rejects_non_interleaved_order(self) -> None:
        write_campaign(self.tmp_path)
        order = self.tmp_path / "e2e" / "order.tsv"
        rows = order.read_text(encoding="utf-8").splitlines()
        order.write_text("".join(f"{row}\n" for row in reversed(rows)), encoding="utf-8")

        with self.assertRaisesRegex(SummaryError, "fixed interleaved campaign"):
            summarize(self.tmp_path)

    @staticmethod
    def _process_gone(pid: int) -> bool:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        except PermissionError:
            return False
        return False


if __name__ == "__main__":
    unittest.main()
