from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ReleasePackageTests(unittest.TestCase):
    def test_build_rejects_source_identity_changes_after_configure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
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
            tracked = source / "runtime.cpp"
            tracked.write_text("int runtime = 1;\n", encoding="utf-8")
            git("add", tracked.name)
            git("commit", "--quiet", "-m", "runtime fixture")
            configured_head = git("rev-parse", "HEAD")
            verifier = Path(__file__).resolve().parents[1] / "cmake" / "verify_build_source.cmake"
            git_executable = shutil.which("git")
            self.assertIsNotNone(git_executable)

            def verify(
                *, head: str = configured_head, dirty: int = 0, mode: str = "auto"
            ) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    [
                        "cmake",
                        f"-DNINFER_SOURCE_DIR={source}",
                        f"-DNINFER_CONFIGURED_PATCH_STACK_SHA={head}",
                        f"-DNINFER_CONFIGURED_SOURCE_DIRTY={dirty}",
                        f"-DNINFER_SOURCE_DIRTY_MODE={mode}",
                        f"-DNINFER_GIT_EXECUTABLE={git_executable}",
                        "-P",
                        str(verifier),
                    ],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )

            self.assertEqual(verify().returncode, 0)
            self.assertEqual(verify(mode="OFF").returncode, 0)

            tracked.write_text("int runtime = 2;\n", encoding="utf-8")
            dirty_result = verify()
            self.assertNotEqual(dirty_result.returncode, 0)
            self.assertIn("dirty state changed since configure", dirty_result.stderr)
            self.assertEqual(verify(dirty=1).returncode, 0)

            explicit_clean_result = verify(mode="OFF")
            self.assertNotEqual(explicit_clean_result.returncode, 0)
            self.assertIn("dirty state changed since configure", explicit_clean_result.stderr)

            git("add", tracked.name)
            git("commit", "--quiet", "-m", "change runtime fixture")
            changed_head_result = verify(mode="OFF")
            self.assertNotEqual(changed_head_result.returncode, 0)
            self.assertIn("source commit changed since configure", changed_head_result.stderr)


if __name__ == "__main__":
    unittest.main()
