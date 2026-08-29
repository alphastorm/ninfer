from __future__ import annotations

import json
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
    command_start,
    inspect_container,
    load_config,
    materialize_source_archive,
    verify_clean_source,
)


def write_config(
    path: Path,
    *,
    restart_policy: str | None = None,
    checkpoint_dir: str | None = None,
    request_log_dir: str = "/srv/ninfer/logs",
    args: list[str] | None = None,
) -> Path:
    config = {
        "image": "ninfer:test",
        "container": "ninfer-test",
        "model_path": "/srv/ninfer/model.ninfer",
        "model_id": "test-model",
        "deployment_profile": "test-profile",
        "port": 18088,
        "request_log_dir": request_log_dir,
        "api_key_file": "/run/secrets/ninfer-test-api-key",
    }
    if restart_policy is not None:
        config["restart_policy"] = restart_policy
    if checkpoint_dir is not None:
        config["checkpoint_dir"] = checkpoint_dir
    if args is not None:
        config["args"] = args
    path.write_text(json.dumps(config), encoding="utf-8")
    return path


class LifecycleConfigTests(unittest.TestCase):
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

    def test_restart_policy_contract(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            default = load_config(write_config(root / "default.json"))
            persistent = load_config(
                write_config(
                    root / "persistent.json", restart_policy="unless-stopped"
                )
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
            root = Path(directory)
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
            with self.assertRaisesRegex(LifecycleError, "lifecycle-owned option"):
                load_config(
                    write_config(
                        root / "override.json",
                        args=["--session-checkpoint-dir", "/unmanaged"],
                    )
                )

    def test_start_mounts_checkpoint_directory_at_owned_server_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config_path = write_config(
                root / "durable.json",
                checkpoint_dir=str(root / "checkpoints"),
                request_log_dir=str(root / "logs"),
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
                expect_image_id=None,
                expect_binary_sha256=None,
                expect_model_artifact_sha256=None,
                expect_config_sha256=None,
            )
            started = subprocess.CompletedProcess(
                args=["docker", "run"], returncode=0, stdout="container-id\n", stderr=""
            )
            with (
                mock.patch(
                    "tools.lifecycle.ninfer_container.require_gpu_idle"
                ),
                mock.patch(
                    "tools.lifecycle.ninfer_container.preflight", return_value=identity
                ),
                mock.patch(
                    "tools.lifecycle.ninfer_container.docker", return_value=started
                ) as docker_mock,
                mock.patch(
                    "tools.lifecycle.ninfer_container.wait_for_health"
                ),
                mock.patch(
                    "tools.lifecycle.ninfer_container.fetch_status", return_value={}
                ),
                mock.patch(
                    "tools.lifecycle.ninfer_container.verify_status"
                ),
                mock.patch("builtins.print"),
            ):
                command_start(args)

            command = docker_mock.call_args.args[0]
            self.assertIn(f"{root / 'checkpoints'}:/checkpoints", command)
            option = command.index("--session-checkpoint-dir")
            self.assertEqual(command[option + 1], "/checkpoints")
            self.assertTrue((root / "checkpoints").is_dir())
            self.assertEqual((root / "checkpoints").stat().st_mode & 0o777, 0o700)


    def test_clean_source_attestation_and_archive(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
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
