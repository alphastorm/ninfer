from __future__ import annotations

import json
from pathlib import Path
import tempfile
import subprocess
import unittest

from tools.lifecycle.ninfer_container import (
    LifecycleError,
    canonical_config_sha256,
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
