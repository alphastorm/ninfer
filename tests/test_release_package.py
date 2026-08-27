from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

from tools.release import package as release_package
from tools.release.package import ReleaseError, ReleaseOptions, package_release


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

            def verify(*, head: str = configured_head, dirty: int = 0) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    [
                        "cmake",
                        f"-DNINFER_SOURCE_DIR={source}",
                        f"-DNINFER_CONFIGURED_PATCH_STACK_SHA={head}",
                        f"-DNINFER_CONFIGURED_SOURCE_DIRTY={dirty}",
                        "-DNINFER_SOURCE_DIRTY_MODE=auto",
                        f"-DNINFER_GIT_EXECUTABLE={git_executable}",
                        "-P",
                        str(verifier),
                    ],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )

            self.assertEqual(verify().returncode, 0)

            tracked.write_text("int runtime = 2;\n", encoding="utf-8")
            dirty_result = verify()
            self.assertNotEqual(dirty_result.returncode, 0)
            self.assertIn("dirty state changed since configure", dirty_result.stderr)
            self.assertEqual(verify(dirty=1).returncode, 0)

            git("add", tracked.name)
            git("commit", "--quiet", "-m", "change runtime fixture")
            changed_head_result = verify(dirty=0)
            self.assertNotEqual(changed_head_result.returncode, 0)
            self.assertIn("source commit changed since configure", changed_head_result.stderr)

    def test_deterministic_asset_checksums_and_spdx(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            binaries = root / "binaries"
            source.mkdir()
            binaries.mkdir()

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
            (source / "LICENSE").write_text("Apache License fixture\n", encoding="utf-8")
            git("add", "LICENSE")
            git("commit", "--quiet", "-m", "release fixture")
            head = git("rev-parse", "HEAD")

            common = (
                f"upstream_base_sha={head} patch_stack_sha={head} "
                "build_profile=qwen38-5090-v0.1.0 build_type=Release "
                "cxx_compiler=GNU-13.3.0 cuda_compiler=NVIDIA-13.1.115 "
                "cuda_toolkit=13.1.115 source_dirty=false"
            )
            ninfer = binaries / "ninfer"
            ninfer_serve = binaries / "ninfer-serve"
            ninfer.write_text(
                f"#!/bin/sh\nprintf '%s\\n' 'ninfer {common}'\n", encoding="utf-8"
            )
            ninfer_serve.write_text(
                f"#!/bin/sh\nprintf '%s\\n' 'ninfer-serve {common}'\n",
                encoding="utf-8",
            )
            ninfer.chmod(0o755)
            ninfer_serve.chmod(0o755)

            image_id = "sha256:" + "1" * 64
            binaries_by_program = {"ninfer": ninfer, "ninfer-serve": ninfer_serve}
            real_run = release_package.run

            def fake_run(
                command: list[str], *, cwd: Path | None = None, text: bool = True
            ) -> subprocess.CompletedProcess[str] | subprocess.CompletedProcess[bytes]:
                if command[0] != "docker":
                    return real_run(command, cwd=cwd, text=text)
                if command[1:3] == ["image", "inspect"]:
                    output = image_id + "\n"
                else:
                    entrypoint_index = command.index("--entrypoint")
                    self.assertEqual(command[entrypoint_index + 2], image_id)
                    entrypoint = command[entrypoint_index + 1]
                    if entrypoint == "/usr/bin/sha256sum":
                        path = command[-1]
                        program = Path(path).name
                        digest = hashlib.sha256(
                            binaries_by_program[program].read_bytes()
                        ).hexdigest()
                        output = f"{digest}  {path}\n"
                    else:
                        program = Path(entrypoint).name
                        output = f"{program} {common}\n"
                return subprocess.CompletedProcess(command, 0, stdout=output, stderr="")

            def options(output: Path) -> ReleaseOptions:
                return ReleaseOptions(
                    source=source,
                    ninfer=ninfer,
                    ninfer_serve=ninfer_serve,
                    image="ninfer:test",
                    output_dir=output,
                    release_version="v0.1.0",
                    platform="linux-x86_64-cuda13.1",
                    upstream_base_sha=head,
                    release_head_sha=head,
                    build_profile="qwen38-5090-v0.1.0",
                    source_date_epoch=1_700_000_000,
                )

            run_patch = mock.patch.object(release_package, "run", side_effect=fake_run)
            run_patch.start()
            self.addCleanup(run_patch.stop)
            first = package_release(options(root / "release-one"))
            second = package_release(options(root / "release-two"))
            self.assertEqual(first["asset"], second["asset"])
            self.assertEqual(first["sbom"], second["sbom"])
            self.assertEqual(first["image_id"], image_id)
            self.assertEqual(first["schema_version"], 2)

            output = root / "release-one"
            asset = output / first["asset"]["name"]
            sbom = output / first["sbom"]["name"]
            checksums = output / first["checksums"]
            checksum_lines = checksums.read_text(encoding="ascii").splitlines()
            self.assertEqual(len(checksum_lines), 2)
            self.assertEqual(
                checksum_lines[0],
                f"{hashlib.sha256(asset.read_bytes()).hexdigest()}  {asset.name}",
            )
            self.assertEqual(
                checksum_lines[1],
                f"{hashlib.sha256(sbom.read_bytes()).hexdigest()}  {sbom.name}",
            )

            release_root = "ninfer-qwen38-rtx5090-v0.1.0-linux-x86_64-cuda13.1"
            with tarfile.open(asset, "r:gz") as archive:
                self.assertEqual(
                    archive.getnames(),
                    [
                        release_root,
                        f"{release_root}/bin",
                        f"{release_root}/LICENSE",
                        f"{release_root}/bin/ninfer",
                        f"{release_root}/bin/ninfer-serve",
                        f"{release_root}/build-identity.json",
                    ],
                )
                identity_stream = archive.extractfile(
                    f"{release_root}/build-identity.json"
                )
                self.assertIsNotNone(identity_stream)
                identity = json.load(identity_stream)
            self.assertEqual(identity["patch_stack_sha"], head)
            self.assertEqual(identity["image_id"], image_id)
            self.assertEqual(identity["schema_version"], 2)
            self.assertFalse(identity["source_dirty"])
            self.assertNotIn(str(source), json.dumps(identity))

            spdx = json.loads(sbom.read_text(encoding="utf-8"))
            self.assertEqual(spdx["spdxVersion"], "SPDX-2.3")
            self.assertEqual(len(spdx["files"]), 4)
            self.assertTrue(spdx["packages"][0]["filesAnalyzed"])
            self.assertTrue(
                spdx["documentNamespace"].startswith(
                    "https://github.com/alphastorm/ninfer/releases/v0.1.0/"
                )
            )
            spdx_sha256 = {
                item["fileName"].removeprefix("./"): next(
                    checksum["checksumValue"]
                    for checksum in item["checksums"]
                    if checksum["algorithm"] == "SHA256"
                )
                for item in spdx["files"]
            }
            with tarfile.open(asset, "r:gz") as archive:
                for name, expected_sha256 in spdx_sha256.items():
                    stream = archive.extractfile(name)
                    self.assertIsNotNone(stream)
                    self.assertEqual(
                        hashlib.sha256(stream.read()).hexdigest(), expected_sha256
                    )

            with mock.patch.object(
                release_package, "image_binary_sha256", return_value="0" * 64
            ):
                with self.assertRaisesRegex(
                    ReleaseError, "package binary differs from the release image"
                ):
                    package_release(options(root / "mismatched-image-release"))

            real_write_archive = release_package.write_archive

            def write_mutated_archive(
                path: Path, root_name: str, files: list[release_package.ReleaseFile], epoch: int
            ) -> None:
                value = bytearray(ninfer.read_bytes())
                value[0] ^= 1
                ninfer.write_bytes(value)
                real_write_archive(path, root_name, files, epoch)

            with mock.patch.object(
                release_package, "write_archive", side_effect=write_mutated_archive
            ):
                with self.assertRaisesRegex(
                    ReleaseError, "archive member digest differs"
                ):
                    package_release(options(root / "mutated-binary-release"))

            (source / "LICENSE").write_text("dirty\n", encoding="utf-8")
            with self.assertRaisesRegex(ReleaseError, "source tree must be clean"):
                package_release(options(root / "dirty-release"))


if __name__ == "__main__":
    unittest.main()
