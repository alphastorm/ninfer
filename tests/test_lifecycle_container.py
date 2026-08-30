from __future__ import annotations

import hashlib
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import subprocess
import unittest
from types import SimpleNamespace
from unittest import mock

from tools.lifecycle.ninfer_container import (
    LifecycleError,
    RuntimeIdentity,
    canonical_config_sha256,
    checkpoint_seccomp_profile,
    command_start,
    create_parser,
    image_binary_sha256,
    inspect_container,
    load_config,
    materialize_source_archive,
    prepare_private_bind_directory,
    verify_clean_source,
    verify_container_configuration,
    wait_for_health,
)


def write_config(
    path: Path,
    *,
    restart_policy: str | None = None,
    checkpoint_dir: str | None = None,
    request_log_dir: str = "/srv/ninfer/logs",
    model_path: str = "/srv/ninfer/model.ninfer",
    api_key_file: str = "/run/secrets/ninfer-test-api-key",
    args: list[str] | None = None,
    bind_host: str | None = None,
) -> Path:
    config = {
        "image": "ninfer:test",
        "container": "ninfer-test",
        "model_path": model_path,
        "model_id": "test-model",
        "deployment_profile": "test-profile",
        "port": 18088,
        "request_log_dir": request_log_dir,
        "api_key_file": api_key_file,
    }
    if restart_policy is not None:
        config["restart_policy"] = restart_policy
    if checkpoint_dir is not None:
        config["checkpoint_dir"] = checkpoint_dir
    if args is not None:
        config["args"] = args
    if bind_host is not None:
        config["bind_host"] = bind_host
    path.write_text(json.dumps(config), encoding="utf-8")
    return path


class LifecycleConfigTests(unittest.TestCase):
    def test_authorizing_commands_require_all_identity_pins(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                create_parser().parse_args(["preflight", "--config", "candidate.json"])

    def test_container_release_cannot_self_declare_clean_without_git(self) -> None:
        root = Path(__file__).resolve().parents[1]
        self.assertNotIn(
            "NINFER_SOURCE_CLEAN_VERIFIED",
            (root / "CMakeLists.txt").read_text(encoding="utf-8"),
        )
        ignored = (root / ".dockerignore").read_text(encoding="utf-8").splitlines()
        self.assertNotIn(".git", ignored)
        dockerfile = (root / "Dockerfile").read_text(encoding="utf-8")
        self.assertNotIn("NINFER_SOURCE_CLEAN_VERIFIED", dockerfile)

    def test_container_lookup_ignores_same_named_image(self) -> None:
        image_only = subprocess.CompletedProcess(
            args=["docker", "container", "inspect", "ninfer:test"],
            returncode=1,
            stdout="",
            stderr="no such container",
        )
        with mock.patch(
            "tools.lifecycle.ninfer_container.run", return_value=image_only
        ) as run_mock:
            self.assertIsNone(inspect_container("ninfer:test"))

        run_mock.assert_called_once_with(
            ["docker", "container", "inspect", "ninfer:test"], check=False
        )

    def test_bind_host_is_loopback_only(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            accepted = load_config(
                write_config(root / "loopback.json", bind_host="127.0.0.1"))
            self.assertEqual(accepted.bind_host, "127.0.0.1")
            with self.assertRaises(LifecycleError):
                load_config(write_config(root / "external.json", bind_host="0.0.0.0"))
            with self.assertRaises(LifecycleError):
                load_config(write_config(root / "other.json", bind_host="192.168.1.10"))

    def test_restart_policy_contract(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            default = load_config(write_config(root / "default.json"))
            persistent = load_config(
                write_config(root / "persistent.json", restart_policy="unless-stopped")
            )

            self.assertEqual(default.restart_policy, "no")
            self.assertEqual(persistent.restart_policy, "unless-stopped")
            self.assertNotEqual(
                canonical_config_sha256(default), canonical_config_sha256(persistent)
            )

            with self.assertRaisesRegex(LifecycleError, "restart_policy"):
                load_config(
                    write_config(root / "invalid.json", restart_policy="always")
                )

    def test_checkpoint_directory_is_mounted_by_lifecycle_only(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            ordinary = load_config(write_config(root / "ordinary.json"))
            durable = load_config(
                write_config(
                    root / "durable.json", checkpoint_dir="/srv/ninfer/checkpoints"
                )
            )

            self.assertIsNone(ordinary.checkpoint_dir)
            self.assertEqual(durable.checkpoint_dir, Path("/srv/ninfer/checkpoints"))
            self.assertNotEqual(
                canonical_config_sha256(ordinary), canonical_config_sha256(durable)
            )
            profile_path = checkpoint_seccomp_profile(durable)
            self.assertIsNotNone(profile_path)
            profile = json.loads(profile_path.read_text(encoding="utf-8"))
            self.assertEqual(profile["defaultAction"], "SCMP_ACT_ERRNO")
            additions = [
                block
                for block in profile["syscalls"]
                if set(block.get("names", []))
                & {"io_uring_setup", "io_uring_enter", "io_uring_register"}
            ]
            self.assertEqual(
                additions,
                [
                    {
                        "names": [
                            "io_uring_setup",
                            "io_uring_enter",
                            "io_uring_register",
                        ],
                        "action": "SCMP_ACT_ALLOW",
                        "includes": {"arches": ["amd64"]},
                    }
                ],
            )
            with self.assertRaisesRegex(LifecycleError, "lifecycle-owned option"):
                load_config(
                    write_config(
                        root / "override.json",
                        args=["--session-checkpoint-dir", "/unmanaged"],
                    )
                )

    def test_start_mounts_checkpoint_directory_at_owned_server_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            model = root / "model.ninfer"
            secret = root / "api-key"
            model.write_bytes(b"model")
            secret.write_text("mounted-secret\n", encoding="utf-8")
            secret.chmod(0o600)
            config_path = write_config(
                root / "durable.json",
                checkpoint_dir=str(root / "checkpoints"),
                request_log_dir=str(root / "logs"),
                model_path=str(model),
                api_key_file=str(secret),
                args=["--max-context", "1024"],
            )
            identity = RuntimeIdentity(
                image_id="sha256:" + "1" * 64,
                binary_sha256="2" * 64,
                model_artifact_sha256="3" * 64,
                config_sha256="4" * 64,
            )
            args = SimpleNamespace(
                config=config_path,
                timeout=1.0,
                expect_image_id=identity.image_id,
                expect_binary_sha256=identity.binary_sha256,
                expect_model_artifact_sha256=identity.model_artifact_sha256,
                expect_config_sha256=identity.config_sha256,
            )
            started = subprocess.CompletedProcess(
                args=["docker", "run"], returncode=0, stdout="container-id\n", stderr=""
            )
            with (
                mock.patch("tools.lifecycle.ninfer_container.require_gpu_idle"),
                mock.patch(
                    "tools.lifecycle.ninfer_container.preflight", return_value=identity
                ),
                mock.patch(
                    "tools.lifecycle.ninfer_container.docker", return_value=started
                ) as docker_mock,
                mock.patch(
                    "tools.lifecycle.ninfer_container.require_managed_container",
                    return_value={},
                ),
                mock.patch(
                    "tools.lifecycle.ninfer_container.verify_container_configuration"
                ),
                mock.patch(
                    "tools.lifecycle.ninfer_container.identity_from_labels",
                    return_value=identity,
                ),
                mock.patch("tools.lifecycle.ninfer_container.wait_for_health"),
                mock.patch(
                    "tools.lifecycle.ninfer_container.fetch_status", return_value={}
                ),
                mock.patch("tools.lifecycle.ninfer_container.verify_status"),
                mock.patch("builtins.print"),
            ):
                command_start(args)

            command = docker_mock.call_args.args[0]
            self.assertIn(f"{root / 'checkpoints'}:/checkpoints", command)
            self.assertIn("no-new-privileges=true", command)
            self.assertEqual(command[command.index("--cap-drop") + 1], "ALL")
            self.assertEqual(
                command[command.index("--user") + 1],
                f"{os.geteuid()}:{os.getegid()}",
            )
            self.assertNotIn("/bin/sh", command)
            self.assertNotIn("mounted-secret", command)
            api_key_option = command.index("--api-key-file")
            self.assertEqual(command[api_key_option + 1], "/run/secrets/ninfer_api_key")
            self.assertLess(api_key_option, command.index("--max-context"))
            self.assertIn(
                f"seccomp={checkpoint_seccomp_profile(load_config(config_path))}",
                command,
            )
            option = command.index("--session-checkpoint-dir")
            self.assertEqual(command[option + 1], "/checkpoints")
            self.assertTrue((root / "checkpoints").is_dir())
            self.assertEqual((root / "checkpoints").stat().st_mode & 0o777, 0o700)

    def test_health_wait_reports_the_first_container_exit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            secret = root / "api-key"
            secret.write_text("secret-value\n", encoding="utf-8")
            secret.chmod(0o600)
            logs_dir = root / "logs"
            logs_dir.mkdir(mode=0o700)
            config = load_config(
                write_config(
                    root / "candidate.json",
                    request_log_dir=str(logs_dir),
                    model_path=str(secret),
                    api_key_file=str(secret),
                )
            )
            logs = subprocess.CompletedProcess(
                args=["docker", "logs"],
                returncode=0,
                stdout="",
                stderr="[error] io_uring_setup: Operation not permitted\n",
            )
            with (
                mock.patch(
                    "tools.lifecycle.ninfer_container.inspect_container",
                    return_value={"State": {"Running": False}},
                ),
                mock.patch("tools.lifecycle.ninfer_container.run", return_value=logs),
            ):
                with self.assertRaisesRegex(
                    LifecycleError, "exited or restarted before becoming healthy"
                ):
                    wait_for_health(config, 10)
            diagnostics = list(logs_dir.glob("startup-failure-*.log"))
            self.assertEqual(len(diagnostics), 1)
            diagnostic = diagnostics[0]
            self.assertEqual(
                diagnostic.read_text(encoding="utf-8"),
                "[error] io_uring_setup: Operation not permitted\n",
            )
            self.assertEqual(diagnostic.stat().st_mode & 0o777, 0o600)

    def test_binary_measurement_uses_host_hashing(self) -> None:
        container_id = "a" * 64
        created = subprocess.CompletedProcess(
            args=["docker", "create"],
            returncode=0,
            stdout=container_id + "\n",
            stderr="",
        )

        def host_command(
            arguments: list[str], **_: object
        ) -> subprocess.CompletedProcess[str]:
            if arguments[:2] == ["docker", "cp"]:
                Path(arguments[-1]).write_bytes(b"trusted-binary-bytes")
            return subprocess.CompletedProcess(arguments, 0, "", "")

        with (
            mock.patch(
                "tools.lifecycle.ninfer_container.docker", return_value=created
            ) as docker_mock,
            mock.patch(
                "tools.lifecycle.ninfer_container.run", side_effect=host_command
            ) as run_mock,
        ):
            measured = image_binary_sha256("ninfer:test")

        self.assertEqual(measured, hashlib.sha256(b"trusted-binary-bytes").hexdigest())
        self.assertEqual(docker_mock.call_args.args[0][0], "create")
        self.assertTrue(
            any(
                call.args[0][:2] == ["docker", "cp"] for call in run_mock.call_args_list
            )
        )
        self.assertFalse(
            any(
                call.args[0][:2] == ["docker", "run"]
                for call in run_mock.call_args_list
            )
        )

    def test_private_bind_directory_rejects_symlink_and_writable_ancestor(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            target = root / "target"
            target.mkdir()
            original_mode = target.stat().st_mode & 0o777
            link = root / "link"
            link.symlink_to(target, target_is_directory=True)
            with self.assertRaisesRegex(LifecycleError, "symlink"):
                prepare_private_bind_directory(link, "checkpoint_dir")
            self.assertEqual(target.stat().st_mode & 0o777, original_mode)

            writable = root / "writable"
            writable.mkdir(mode=0o777)
            writable.chmod(0o777)
            with self.assertRaisesRegex(LifecycleError, "externally writable"):
                prepare_private_bind_directory(writable / "child", "request_log_dir")

    def test_binary_measurement_rejects_image_symlink(self) -> None:
        container_id = "b" * 64
        created = subprocess.CompletedProcess(
            args=["docker", "create"],
            returncode=0,
            stdout=container_id + "\n",
            stderr="",
        )

        def host_command(
            arguments: list[str], **_: object
        ) -> subprocess.CompletedProcess[str]:
            if arguments[:2] == ["docker", "cp"]:
                Path(arguments[-1]).symlink_to("/etc/hosts")
            return subprocess.CompletedProcess(arguments, 0, "", "")

        with (
            mock.patch("tools.lifecycle.ninfer_container.docker", return_value=created),
            mock.patch(
                "tools.lifecycle.ninfer_container.run", side_effect=host_command
            ),
        ):
            with self.assertRaisesRegex(LifecycleError, "regular file"):
                image_binary_sha256("ninfer:symlink")

    def test_start_cleans_container_created_before_run_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            model = root / "model.ninfer"
            secret = root / "api-key"
            model.write_bytes(b"model")
            secret.write_text("secret\n", encoding="utf-8")
            secret.chmod(0o600)
            config_path = write_config(
                root / "candidate.json",
                checkpoint_dir=str(root / "checkpoints"),
                request_log_dir=str(root / "logs"),
                model_path=str(model),
                api_key_file=str(secret),
            )
            identity = RuntimeIdentity(
                image_id="sha256:" + "1" * 64,
                binary_sha256="2" * 64,
                model_artifact_sha256="3" * 64,
                config_sha256="4" * 64,
            )
            args = SimpleNamespace(
                config=config_path,
                timeout=1.0,
                expect_image_id=identity.image_id,
                expect_binary_sha256=identity.binary_sha256,
                expect_model_artifact_sha256=identity.model_artifact_sha256,
                expect_config_sha256=identity.config_sha256,
            )
            owned = {
                "Config": {
                    "Labels": {
                        "org.ninfer.lifecycle": "tools.lifecycle.ninfer_container"
                    }
                }
            }
            removed = subprocess.CompletedProcess(
                args=["docker", "rm"], returncode=0, stdout="", stderr=""
            )
            with (
                mock.patch("tools.lifecycle.ninfer_container.require_gpu_idle"),
                mock.patch(
                    "tools.lifecycle.ninfer_container.preflight", return_value=identity
                ),
                mock.patch(
                    "tools.lifecycle.ninfer_container.docker",
                    side_effect=LifecycleError("docker run failed"),
                ),
                mock.patch(
                    "tools.lifecycle.ninfer_container.inspect_container",
                    return_value=owned,
                ),
                mock.patch(
                    "tools.lifecycle.ninfer_container.run", return_value=removed
                ) as run_mock,
            ):
                with self.assertRaisesRegex(LifecycleError, "docker run failed"):
                    command_start(args)

            run_mock.assert_called_with(
                ["docker", "rm", "--force", "ninfer-test"], check=False
            )

    def test_container_configuration_binds_mounts_and_security(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            model = root / "model.ninfer"
            secret = root / "api-key"
            logs = root / "logs"
            checkpoints = root / "checkpoints"
            model.write_bytes(b"model")
            secret.write_text("secret\n", encoding="utf-8")
            secret.chmod(0o600)
            logs.mkdir()
            checkpoints.mkdir()
            config = load_config(
                write_config(
                    root / "candidate.json",
                    checkpoint_dir=str(checkpoints),
                    request_log_dir=str(logs),
                    model_path=str(model),
                    api_key_file=str(secret),
                )
            )
            seccomp = checkpoint_seccomp_profile(config)
            seccomp_json = json.dumps(
                json.loads(seccomp.read_text(encoding="utf-8")),
                separators=(",", ":"),
            )
            inspected = {
                "HostConfig": {
                    "RestartPolicy": {"Name": "no"},
                    "Privileged": False,
                    "CapAdd": None,
                    "CapDrop": ["ALL"],
                    "IpcMode": "host",
                    "PidMode": "",
                    "SecurityOpt": [
                        "no-new-privileges=true",
                        f"seccomp={seccomp_json}",
                    ],
                },
                "Config": {
                    "User": f"{os.geteuid()}:{os.getegid()}",
                    "Cmd": ["/usr/local/bin/ninfer-serve", "/models/model.ninfer"],
                    "Labels": {
                        "org.ninfer.checkpoint-seccomp-sha256": (
                            "3b7bf1e9fa71bfd8ed536c8aaaa2d40e4f133a0c853d226efd9b9e4966dd1506"
                        )
                    },
                },
                "Mounts": [
                    {
                        "Destination": "/models/model.ninfer",
                        "Source": str(model),
                        "RW": False,
                    },
                    {"Destination": "/logs", "Source": str(logs), "RW": True},
                    {
                        "Destination": "/checkpoints",
                        "Source": str(checkpoints),
                        "RW": True,
                    },
                    {
                        "Destination": "/run/secrets/ninfer_api_key",
                        "Source": str(secret),
                        "RW": False,
                    },
                ],
            }
            verify_container_configuration(config, inspected)
            inspected["HostConfig"]["SecurityOpt"] = ["seccomp=unconfined"]
            with self.assertRaisesRegex(LifecycleError, "no-new-privileges"):
                verify_container_configuration(config, inspected)

    def test_clean_source_attestation_and_archive(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            source = root / "source"
            source.mkdir()

            def git(*arguments: str) -> str:
                result = subprocess.run(
                    ["git", "-C", str(source), *arguments],
                    check=True,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                return result.stdout.strip()

            git("init", "--quiet")
            git("config", "user.name", "NInfer Test")
            git("config", "user.email", "ninfer-test@example.invalid")
            (source / "Dockerfile").write_text("FROM scratch\n", encoding="utf-8")
            (source / "tracked.txt").write_text("release bytes\n", encoding="utf-8")
            git("add", "Dockerfile", "tracked.txt")
            git("commit", "--quiet", "-m", "test fixture")
            head = git("rev-parse", "HEAD")

            self.assertEqual(verify_clean_source(source, head, head), head)
            first_context = root / "context-one"
            second_context = root / "context-two"
            first_context.mkdir()
            second_context.mkdir()
            first_hash = materialize_source_archive(source, head, first_context)
            second_hash = materialize_source_archive(source, head, second_context)
            self.assertEqual(first_hash, second_hash)
            self.assertEqual(
                (first_context / "tracked.txt").read_text(encoding="utf-8"),
                "release bytes\n",
            )
            self.assertTrue((first_context / ".git").is_dir())
            self.assertEqual(
                subprocess.run(
                    ["git", "-C", str(first_context), "rev-parse", "HEAD"],
                    check=True,
                    text=True,
                    stdout=subprocess.PIPE,
                ).stdout.strip(),
                head,
            )
            self.assertEqual(
                subprocess.run(
                    ["git", "-C", str(first_context), "status", "--porcelain"],
                    check=True,
                    text=True,
                    stdout=subprocess.PIPE,
                ).stdout,
                "",
            )

            (source / "tracked.txt").write_text("dirty bytes\n", encoding="utf-8")
            with self.assertRaisesRegex(LifecycleError, "source tree must be clean"):
                verify_clean_source(source, head, head)


if __name__ == "__main__":
    unittest.main()
