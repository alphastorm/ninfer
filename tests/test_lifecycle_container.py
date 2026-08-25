from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from tools.lifecycle.ninfer_container import (
    LifecycleError,
    canonical_config_sha256,
    load_config,
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


if __name__ == "__main__":
    unittest.main()
