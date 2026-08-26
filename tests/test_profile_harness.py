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


def lease_fixture(tmp_path: Path, *, controller: bool) -> tuple[Path, dict[str, str], Path, Path]:
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    state = tmp_path / "production-running"
    state.write_text("true\n", encoding="utf-8")
    docker_log = tmp_path / "docker.log"
    controller_log = tmp_path / "controller.log"

    write_executable(
        bin_dir / "docker",
        """#!/usr/bin/env python3
import os
import sys
from pathlib import Path

state = Path(os.environ["FAKE_PRODUCTION_STATE"])
log = Path(os.environ["FAKE_DOCKER_LOG"])
args = sys.argv[1:]


def record(value: str) -> None:
    with log.open("a", encoding="utf-8") as stream:
        stream.write(value + "\\n")


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
    raise SystemExit(0)
if args[0] == "stop":
    record("stop")
    state.write_text("false\\n", encoding="utf-8")
    raise SystemExit(0)
if args[0] == "rm":
    record("remove")
    raise SystemExit(0)
raise SystemExit(f"unsupported fake docker command: {args!r}")
""",
    )
    write_executable(bin_dir / "curl", "#!/usr/bin/env bash\nexit 0\n")
    write_executable(bin_dir / "nvidia-smi", "#!/usr/bin/env bash\necho 'fake gpu'\n")
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
    config_values = {
        "PROFILE_RESULT_DIR": result_dir,
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
        "PROFILE_PAYLOAD_STOP_TIMEOUT_SECONDS": 1,
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
            [LEASE_SCRIPT, config, "/usr/bin/true"],
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
                config, env, state, docker_log = lease_fixture(case_path, controller=True)
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
                    [LEASE_SCRIPT, config, payload],
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
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=2)

                result_dir = case_path / "result"
                self.assertEqual(returncode, expected_status)
                self.assertEqual(payload_signal.read_text(encoding="utf-8"), signal_name)
                self.assertEqual(state.read_text(encoding="utf-8").strip(), "true")
                self.assertEqual(
                    (result_dir / "exit-status.txt").read_text(encoding="utf-8").strip(),
                    str(expected_status),
                )

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


if __name__ == "__main__":
    unittest.main()
