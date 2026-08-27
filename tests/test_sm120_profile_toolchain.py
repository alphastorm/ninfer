from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
ENSURE_TOOLCHAIN = REPOSITORY / "tools" / "bench" / "ensure_sm120_profile_toolchain.sh"
DOCKERFILE = REPOSITORY / "tools" / "bench" / "Dockerfile.sm120-profile-python311"
BASE_ID = "sha256:" + "a" * 64
IMAGE_ID = "sha256:" + "b" * 64
PYTHON_SOURCE_SHA256 = "2a9920c7a0cd236de33644ed980a13cbbc21058bfdc528febb6081575ed73be3"


def write_executable(path: Path, content: str) -> None:
    path.write_text(textwrap.dedent(content).lstrip(), encoding="utf-8")
    path.chmod(0o755)


class Sm120ProfileToolchainTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "source"
        bench = self.source / "tools" / "bench"
        bench.mkdir(parents=True)
        shutil.copy2(DOCKERFILE, bench / DOCKERFILE.name)
        self.log = self.root / "docker.jsonl"
        self.built = self.root / "built"
        self.fake_docker = self.root / "docker"
        write_executable(
            self.fake_docker,
            f"""
            #!{sys.executable}
            import json
            import os
            import sys
            from pathlib import Path

            args = sys.argv[1:]
            with Path(os.environ["MOCK_DOCKER_LOG"]).open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(args) + "\\n")
            if args[:2] != ["--context", "default"]:
                raise SystemExit(90)
            args = args[2:]
            if args[:2] == ["context", "inspect"]:
                print("unix:///var/run/docker.sock")
                raise SystemExit(0)
            if args[0] == "info":
                print("mock-daemon")
                raise SystemExit(0)
            if args[:2] == ["image", "inspect"]:
                image = args[2]
                format_value = args[args.index("--format") + 1] if "--format" in args else ""
                if image == "profile-base:fixed":
                    print("{BASE_ID}")
                    raise SystemExit(0)
                exists = os.environ["MOCK_IMAGE_STATE"] != "missing" or Path(os.environ["MOCK_BUILT"]).exists()
                if not exists:
                    raise SystemExit(1)
                if not format_value:
                    print("[]")
                elif format_value == "{{{{.Id}}}}":
                    print("{IMAGE_ID}")
                elif "base-image-id" in format_value:
                    print("sha256:" + ("c" if os.environ["MOCK_IMAGE_STATE"] == "mismatch" else "a") * 64)
                elif "python-version" in format_value:
                    print("3.11.11")
                elif "python-source-sha256" in format_value:
                    print("{PYTHON_SOURCE_SHA256}")
                else:
                    raise SystemExit(91)
                raise SystemExit(0)
            if args[0] == "run":
                raise SystemExit(0)
            if args[0] == "build":
                Path(os.environ["MOCK_BUILT"]).touch()
                raise SystemExit(0)
            raise SystemExit(f"unsupported Docker invocation: {{args!r}}")
            """,
        )

    def run_helper(self, state: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [ENSURE_TOOLCHAIN, self.source],
            check=False,
            capture_output=True,
            text=True,
            env={
                **os.environ,
                "PROFILE_DOCKER_CLI": str(self.fake_docker),
                "PROFILE_DOCKER_CONTEXT": "default",
                "PROFILE_DOCKER_ENDPOINT": "unix:///var/run/docker.sock",
                "PROFILE_DOCKER_DAEMON_ID": "mock-daemon",
                "PROFILE_TOOLCHAIN_BASE_IMAGE": "profile-base:fixed",
                "PROFILE_TOOLCHAIN_BASE_IMAGE_ID": BASE_ID,
                "PROFILE_TOOLCHAIN_IMAGE": "profile-python311:fixed",
                "MOCK_DOCKER_LOG": str(self.log),
                "MOCK_IMAGE_STATE": state,
                "MOCK_BUILT": str(self.built),
            },
        )

    def docker_calls(self) -> list[list[str]]:
        return [json.loads(line) for line in self.log.read_text(encoding="utf-8").splitlines()]

    def test_reuses_exact_provenance_image(self) -> None:
        completed = self.run_helper("matching")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stdout.strip(), IMAGE_ID)
        self.assertFalse(any(call[2:3] == ["build"] for call in self.docker_calls()))

    def test_builds_absent_image_then_returns_exact_id(self) -> None:
        completed = self.run_helper("missing")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stdout.strip(), IMAGE_ID)
        build = next(call for call in self.docker_calls() if call[2:3] == ["build"])
        self.assertIn("--pull=false", build)
        self.assertIn(f"BASE_IMAGE_ID={BASE_ID}", build)
        self.assertTrue(self.built.is_file())

    def test_rejects_existing_image_with_wrong_provenance(self) -> None:
        completed = self.run_helper("mismatch")
        self.assertEqual(completed.returncode, 2)
        self.assertIn("does not match its pinned base", completed.stderr)
        self.assertFalse(any(call[2:3] == ["build"] for call in self.docker_calls()))


if __name__ == "__main__":
    unittest.main()
