from __future__ import annotations

import argparse
import contextlib
import io
import json
from pathlib import Path
import tempfile
import subprocess
import unittest
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


def write_config(path: Path, *, restart_policy: str | None = None) -> Path:
    config = {
        "image": "ninfer:test",
        "container": "ninfer-test",
        "model_path": "/srv/ninfer/model.ninfer",
        "model_id": "test-model",
        "deployment_profile": "test-profile",
        "port": 18088,
        "request_log_dir": "/srv/ninfer/logs",
        "api_key_file": "/run/secrets/ninfer-test-api-key",
    }
    if restart_policy is not None:
        config["restart_policy"] = restart_policy
    path.write_text(json.dumps(config), encoding="utf-8")
    return path


class LifecycleConfigTests(unittest.TestCase):
    def test_start_uses_the_resolved_image_id(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config_path = write_config(root / "runtime.json")
            config = json.loads(config_path.read_text(encoding="utf-8"))
            config["model_path"] = str(root / "model.ninfer")
            config["request_log_dir"] = str(root / "logs")
            config["api_key_file"] = str(root / "api-key")
            config_path.write_text(json.dumps(config), encoding="utf-8")
            image_id = "sha256:" + "1" * 64
            identity = RuntimeIdentity(
                image_id=image_id,
                binary_sha256="2" * 64,
                model_artifact_sha256="3" * 64,
                config_sha256="4" * 64,
            )
            args = argparse.Namespace(
                config=config_path,
                timeout=1.0,
                expect_image_id=None,
                expect_binary_sha256=None,
                expect_model_artifact_sha256=None,
                expect_config_sha256=None,
            )
            docker_result = subprocess.CompletedProcess(
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
                    "tools.lifecycle.ninfer_container.docker", return_value=docker_result
                ) as docker_mock,
                mock.patch(
                    "tools.lifecycle.ninfer_container.inspect_container",
                    return_value={"Image": image_id},
                ),
                mock.patch(
                    "tools.lifecycle.ninfer_container.wait_for_health"
                ),
                mock.patch(
                    "tools.lifecycle.ninfer_container.fetch_status", return_value={}
                ),
                mock.patch(
                    "tools.lifecycle.ninfer_container.verify_status"
                ),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                command_start(args)

            run_command = docker_mock.call_args.args[0]
            self.assertIn(image_id, run_command)
            self.assertNotIn("ninfer:test", run_command)

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

            (source / "tracked.txt").write_text("dirty bytes\n", encoding="utf-8")
            with self.assertRaisesRegex(LifecycleError, "source tree must be clean"):
                verify_clean_source(source, head, head)

if __name__ == "__main__":
    unittest.main()
