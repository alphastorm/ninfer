from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
CONTROLLER_WORKFLOW = REPOSITORY / "tools" / "bench" / "run_remote_sm120_mtp3_workflow.sh"


def write_executable(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(content).lstrip(), encoding="utf-8")
    path.chmod(0o755)


class Sm120RemoteWorkflowTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "source"
        tools = self.source / "tools" / "bench"
        tools.mkdir(parents=True)
        write_executable(
            tools / "run_sm120_mtp3_workflow.sh",
            r"""
            #!/usr/bin/env bash
            set -Eeuo pipefail
            mode=$1
            config=$2
            # shellcheck source=/dev/null
            source "$config"
            case "$mode" in
              check)
                printf 'mock target preflight passed\n'
                ;;
              run)
                mkdir -p "$PROFILE_RESULT_DIR/sm120-mtp3/ncu"
                : >"$PROFILE_RESULT_DIR/production-stop-requested-at.txt"
                : >"$PROFILE_RESULT_DIR/production-restored-at.txt"
                : >"$PROFILE_RESULT_DIR/production-final-status.txt"
                : >"$PROFILE_RESULT_DIR/production-final-inspect.txt"
                : >"$PROFILE_RESULT_DIR/rollback-final-inspect.txt"
                printf '%s\n' "${MOCK_TARGET_STATUS:-0}" >"$PROFILE_RESULT_DIR/exit-status.txt"
                if [[ ${MOCK_TARGET_STATUS:-0} == 0 ]]; then
                  : >"$PROFILE_RESULT_DIR/sm120-mtp3/ncu/counter-access-verified.txt"
                  : >"$PROFILE_RESULT_DIR/sm120-mtp3/packet-complete.txt"
                fi
                exit "${MOCK_TARGET_STATUS:-0}"
                ;;
              *) exit 2 ;;
            esac
            """,
        )
        write_executable(
            tools / "ensure_sm120_profile_toolchain.sh",
            "#!/usr/bin/env bash\nprintf 'sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\\n'\n",
        )
        subprocess.run(["git", "init", "-q", self.source], check=True)
        subprocess.run(["git", "-C", self.source, "config", "user.name", "Test"], check=True)
        subprocess.run(["git", "-C", self.source, "config", "core.fsmonitor", "false"], check=True)
        subprocess.run(
            ["git", "-C", self.source, "config", "user.email", "test@example.invalid"],
            check=True,
        )
        subprocess.run(["git", "-C", self.source, "add", "."], check=True)
        subprocess.run(["git", "-C", self.source, "commit", "-qm", "base"], check=True)
        self.base_sha = subprocess.check_output(
            ["git", "-C", self.source, "rev-parse", "HEAD"], text=True
        ).strip()
        (self.source / "source-marker").write_text("head\n", encoding="utf-8")
        subprocess.run(["git", "-C", self.source, "add", "."], check=True)
        subprocess.run(["git", "-C", self.source, "commit", "-qm", "head"], check=True)
        self.source_sha = subprocess.check_output(
            ["git", "-C", self.source, "rev-parse", "HEAD"], text=True
        ).strip()

        self.fake_ssh = self.root / "fake-ssh"
        self.ssh_log = self.root / "ssh.log"
        write_executable(
            self.fake_ssh,
            r"""
            #!/usr/bin/env bash
            set -Eeuo pipefail
            printf '%q ' "$@" >>"$MOCK_SSH_LOG"
            printf '\n' >>"$MOCK_SSH_LOG"
            while (($#)); do
              case "$1" in
                -o|-J) shift 2 ;;
                *) shift; break ;;
              esac
            done
            [[ $1 == wsl.exe && $2 == -d && $4 == -- ]] || exit 91
            shift 4
            if [[ $1 == /usr/bin/sha256sum ]]; then
              shift
              exec /usr/bin/shasum -a 256 "$@"
            fi
            if [[ $1 == /usr/bin/test ]]; then
              shift
              exec /bin/test "$@"
            fi
            case "$1" in
              /usr/bin/mkdir) shift; exec /bin/mkdir "$@" ;;
              /usr/bin/rm) shift; exec /bin/rm "$@" ;;
              /usr/bin/cat) shift; exec /bin/cat "$@" ;;
            esac
            exec "$@"
            """,
        )
        self.operation_parent = self.root / "target-operations"
        self.operation_parent.mkdir()
        self.receipt_parent = self.root / "receipts"
        self.model = self.root / "model.ninfer"
        self.model.write_bytes(b"model")
        self.controller = self.root / "production-controller"
        write_executable(self.controller, "#!/usr/bin/env bash\nexit 0\n")

    def write_config(self, operation_name: str, status: int = 0) -> Path:
        values = {
            "CONTROLLER_TARGET_HOST": "mock-target",
            "CONTROLLER_WSL_DISTRIBUTION": "Ubuntu-24.04",
            "CONTROLLER_OPERATION_PARENT": self.operation_parent,
            "CONTROLLER_RECEIPT_ROOT": self.receipt_parent,
            "CONTROLLER_SOURCE_ROOT": self.source,
            "CONTROLLER_OPERATION_NAME": operation_name,
            "CONTROLLER_SSH_BIN": self.fake_ssh,
            "CONTROLLER_GIT_BIN": shutil.which("git"),
            "CONTROLLER_SHA256_BIN": "/usr/bin/shasum",
            "PROFILE_UPSTREAM_BASE_SHA": self.base_sha,
            "PROFILE_DOCKER_CLI": "/usr/bin/docker",
            "PROFILE_DOCKER_CONTEXT": "default",
            "PROFILE_DOCKER_ENDPOINT": "unix:///var/run/docker.sock",
            "PROFILE_DOCKER_DAEMON_ID": "mock-daemon",
            "PROFILE_TOOLCHAIN_BASE_IMAGE": "profile-base:fixed",
            "PROFILE_TOOLCHAIN_BASE_IMAGE_ID": "sha256:" + "9" * 64,
            "PROFILE_TOOLCHAIN_IMAGE": "profile:fixed",
            "PROFILE_CONTAINER_PYTHON": "python3.11",
            "PROFILE_NCU_TIMEOUT_SECONDS": "180",
            "PROFILE_NSYS_TIMEOUT_SECONDS": "300",
            "TOOLCHAIN_IMAGE": "runtime:fixed",
            "CANDIDATE_PORT": "19317",
            "MODEL_PATH": self.model,
            "MODEL_SHA256": "b" * 64,
            "MODEL_ID": "q38-ninfer",
            "PROFILE_CONTROLLER_ID": "mock-controller",
            "PROFILE_TARGET_ID": "mock-target",
            "PRODUCTION_CONTAINER": "production",
            "PRODUCTION_ID": "c" * 64,
            "PRODUCTION_IMAGE": "runtime:fixed",
            "PRODUCTION_RESTART_POLICY": "unless-stopped",
            "PRODUCTION_PORT": "18088",
            "PRODUCTION_CONTROLLER": self.controller,
            "ROLLBACK_CONTAINER": "rollback",
            "ROLLBACK_ID": "d" * 64,
            "ROLLBACK_IMAGE_ID": "sha256:" + "e" * 64,
            "ROLLBACK_RUNNING": "false",
            "ROLLBACK_RESTART_POLICY": "no",
            "PROFILE_MAX_CONTEXT": "131072",
            "PROFILE_PREFILL_CHUNK": "1024",
            "PROFILE_KV_DTYPE": "bf16",
            "PROFILE_MAX_CONCURRENCY": "1",
            "PROFILE_DRAFT_TOKENS": "3",
            "PROFILE_SEED": "7632647173703959000",
            "PROFILE_STARTUP_TIMEOUT_SECONDS": "600",
            "PROFILE_PAYLOAD_STOP_TIMEOUT_SECONDS": "30",
            "PROFILE_WARMUP_MESSAGES": "warmup.json",
            "PROFILE_LONG_MESSAGES": "long.json",
            "PROFILE_DECODE_MESSAGES": "decode.json",
            "PROFILE_WARMUP_EXPECTED": "42",
            "PROFILE_LONG_EXPECTED": "ORCHID=493817; COLOR=COBALT",
            "MOCK_TARGET_STATUS": str(status),
        }
        config = self.root / f"{operation_name}.conf"
        config.write_text(
            "\n".join(f"{key}={shlex.quote(str(value))}" for key, value in values.items()) + "\n",
            encoding="utf-8",
        )
        return config

    def run_workflow(self, config: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [CONTROLLER_WORKFLOW, "run", config],
            check=False,
            capture_output=True,
            text=True,
            env={**os.environ, "MOCK_SSH_LOG": str(self.ssh_log)},
        )

    def test_stages_exact_source_runs_target_and_retrieves_receipts(self) -> None:
        operation = "remote-workflow-success"
        completed = self.run_workflow(self.write_config(operation))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        target = self.operation_parent / operation
        receipt = self.receipt_parent / operation
        self.assertEqual(
            subprocess.check_output(["git", "-C", target / "source", "rev-parse", "HEAD"], text=True).strip(),
            self.source_sha,
        )
        self.assertFalse((target / "source.bundle").exists())
        self.assertIn(
            "PROFILE_TOOLCHAIN_IMAGE_ID=sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
            (target / "profile.conf").read_text(encoding="utf-8"),
        )
        self.assertTrue((receipt / "results" / "production-restored-at.txt").is_file())
        self.assertTrue((receipt / "results" / "sm120-mtp3" / "packet-complete.txt").is_file())
        self.assertIn("remote profile workflow complete", completed.stdout)
        log = self.ssh_log.read_text(encoding="utf-8")
        self.assertIn("run_sm120_mtp3_workflow.sh check", log)
        self.assertIn("run_sm120_mtp3_workflow.sh run", log)

    def test_failed_target_run_retrieves_restoration_receipt(self) -> None:
        operation = "remote-workflow-failure"
        completed = self.run_workflow(self.write_config(operation, status=7))
        self.assertEqual(completed.returncode, 7)
        receipt = self.receipt_parent / operation / "results"
        self.assertTrue((receipt / "production-stop-requested-at.txt").is_file())
        self.assertTrue((receipt / "production-restored-at.txt").is_file())
        self.assertIn("verified restoration", completed.stderr)


if __name__ == "__main__":
    unittest.main()
