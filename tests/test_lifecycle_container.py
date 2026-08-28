from __future__ import annotations

import json
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest import mock

from tools.lifecycle.ninfer_container import (
    LifecycleError,
    canonical_config_sha256,
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
