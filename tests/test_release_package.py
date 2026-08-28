from __future__ import annotations

import dataclasses
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import textwrap
import unittest

from tools.release.package import (
    ReleaseError,
    ReleaseOptions,
    package_release,
    parse_build_info,
)


_COMMIT_DATE = "2023-11-14T22:13:20+00:00"
_EPOCH = 1_700_000_000
_SUPPORT_CONTENT = {
    "VERSION": "0.6.1-rtx3090\n",
    "LICENSE": "Apache License fixture\n",
    "RELEASE_NOTES_0.6.1.md": "Release notes fixture\n",
    "docs/rtx-3090-linux.md": "RTX 3090 guide fixture\n",
    "scripts/download-qwen38.sh": "#!/bin/sh\nexit 0\n",
    "scripts/run-qwen38-c1.sh": "#!/bin/sh\nexit 0\n",
    "scripts/run-qwen38-c8.sh": "#!/bin/sh\nexit 0\n",
    "scripts/run-qwen38-vision.sh": "#!/bin/sh\nexit 0\n",
    "scripts/download-qwen36-35b-vision.sh": "#!/bin/sh\nexit 0\n",
    "scripts/run-qwen36-35b-vision.sh": "#!/bin/sh\nexit 0\n",
}


def run_git(source: Path, *arguments: str, commit_date: bool = False) -> str:
    environment = os.environ.copy()
    if commit_date:
        environment.update(
            GIT_AUTHOR_DATE=_COMMIT_DATE,
            GIT_COMMITTER_DATE=_COMMIT_DATE,
        )
    result = subprocess.run(
        ["git", "-C", str(source), *arguments],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
    )
    return result.stdout.strip()


def create_release_fixture(root: Path) -> tuple[Path, Path, str]:
    source = root / "source"
    binaries = root / "binaries"
    source.mkdir()
    binaries.mkdir()
    run_git(source, "init", "--quiet")
    run_git(source, "config", "user.name", "NInfer Test")
    run_git(source, "config", "user.email", "ninfer-test@example.invalid")
    for name, content in _SUPPORT_CONTENT.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        if name.startswith("scripts/"):
            path.chmod(0o755)
    (source / "tracked.txt").write_text("release bytes\n", encoding="utf-8")
    run_git(source, "add", ".")
    run_git(source, "commit", "--quiet", "-m", "release fixture", commit_date=True)
    return source, binaries, run_git(source, "rev-parse", "HEAD")


def write_fake_binaries(binaries: Path, head: str) -> tuple[Path, Path, Path]:
    common = (
        f"upstream_base_sha={head} patch_stack_sha={head} "
        "build_profile=omp-v0.2.0-rtx3090 build_type=Release "
        "cxx_compiler=GNU-13.3.0 cuda_compiler=NVIDIA-12.8.93 "
        "cuda_toolkit=12.8.93 source_dirty=false"
    )
    ninfer = binaries / "ninfer"
    ninfer_serve = binaries / "ninfer-serve"
    ninfer_bench = binaries / "ninfer_bench"
    ninfer.write_text(
        f"#!/bin/sh\nprintf '%s\\n' 'ninfer {common}'\n", encoding="utf-8"
    )
    ninfer_serve.write_text(
        f"#!/bin/sh\nprintf '%s\\n' 'ninfer-serve {common}'\n",
        encoding="utf-8",
    )
    ninfer_bench.write_text("benchmark fixture\n", encoding="utf-8")
    for binary in (ninfer, ninfer_serve, ninfer_bench):
        binary.chmod(0o755)
    return ninfer, ninfer_serve, ninfer_bench


def release_options(
    source: Path, binaries: Path, head: str, output: Path
) -> ReleaseOptions:
    ninfer, ninfer_serve, ninfer_bench = write_fake_binaries(binaries, head)
    return ReleaseOptions(
        source=source,
        ninfer=ninfer,
        ninfer_serve=ninfer_serve,
        ninfer_bench=ninfer_bench,
        output_dir=output,
        release_version="v0.2.0",
        platform="linux-x86_64-cuda12.8",
        upstream_base_sha=head,
        release_head_sha=head,
        build_profile="omp-v0.2.0-rtx3090",
        source_date_epoch=_EPOCH,
    )


class BuildIdentityTests(unittest.TestCase):
    def test_build_rejects_source_identity_changes_after_configure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            source.mkdir()
            run_git(source, "init", "--quiet")
            run_git(source, "config", "user.name", "NInfer Test")
            run_git(source, "config", "user.email", "ninfer-test@example.invalid")
            tracked = source / "runtime.cpp"
            tracked.write_text("int runtime = 1;\n", encoding="utf-8")
            run_git(source, "add", tracked.name)
            run_git(source, "commit", "--quiet", "-m", "runtime fixture")
            configured_head = run_git(source, "rev-parse", "HEAD")
            verifier = (
                Path(__file__).resolve().parents[1]
                / "cmake"
                / "verify_build_source.cmake"
            )
            git_executable = shutil.which("git")
            self.assertIsNotNone(git_executable)

            def verify(
                *, head: str = configured_head, dirty: int = 0
            ) -> subprocess.CompletedProcess[str]:
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

            (source / "untracked.cpp").write_text("int extra;\n", encoding="utf-8")
            self.assertNotEqual(verify().returncode, 0)

            run_git(source, "add", tracked.name)
            run_git(source, "commit", "--quiet", "-m", "change runtime fixture")
            changed_head_result = verify(dirty=1)
            self.assertNotEqual(changed_head_result.returncode, 0)
            self.assertIn("source commit changed since configure", changed_head_result.stderr)

    def test_build_info_and_version_parser_compile_without_cuda(self) -> None:
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        self.assertIsNotNone(compiler)
        project = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generated = root / "generated"
            generated.mkdir()
            upstream = "1" * 40
            patch = "2" * 40
            (generated / "ninfer_build_info_config.h").write_text(
                textwrap.dedent(
                    f"""\
                    #pragma once
                    #define NINFER_BUILD_UPSTREAM_BASE_SHA "{upstream}"
                    #define NINFER_BUILD_PATCH_STACK_SHA "{patch}"
                    #define NINFER_BUILD_PROFILE "omp-v0.2.0-rtx3090"
                    #define NINFER_BUILD_TYPE "Release"
                    #define NINFER_BUILD_CXX_COMPILER "AppleClang-18.0.0"
                    #define NINFER_BUILD_CUDA_COMPILER "NVIDIA-12.8.93"
                    #define NINFER_BUILD_CUDA_TOOLKIT "12.8.93"
                    #define NINFER_BUILD_SOURCE_DIRTY 0
                    """
                ),
                encoding="utf-8",
            )
            main = root / "version_main.cpp"
            main.write_text(
                textwrap.dedent(
                    """\
                    #include "options.h"
                    #include "ninfer/build_info.h"
                    #include <iostream>

                    int main(int argc, char** argv) {
                        const ninfer::cli::Options options =
                            ninfer::cli::parse_options(argc, argv);
                        if (!options.version_requested) { return 3; }
                        std::cout << ninfer::format_build_info("ninfer") << '\\n';
                        return 0;
                    }
                    """
                ),
                encoding="utf-8",
            )
            binary = root / "ninfer"
            subprocess.run(
                [
                    str(compiler),
                    "-std=c++20",
                    "-I",
                    str(project / "include"),
                    "-I",
                    str(project / "src"),
                    "-I",
                    str(project / "apps" / "cli"),
                    "-I",
                    str(generated),
                    str(project / "src" / "core" / "build_info.cpp"),
                    str(project / "apps" / "cli" / "options.cpp"),
                    str(main),
                    "-o",
                    str(binary),
                ],
                check=True,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            values = parse_build_info(binary, "ninfer")
            self.assertEqual(values["upstream_base_sha"], upstream)
            self.assertEqual(values["patch_stack_sha"], patch)
            self.assertEqual(values["cuda_toolkit"], "12.8.93")
            self.assertEqual(values["source_dirty"], "false")


class ReleasePackageTests(unittest.TestCase):
    def test_deterministic_source_binary_checksums_and_spdx(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, binaries, head = create_release_fixture(root)
            first = package_release(
                release_options(source, binaries, head, root / "release-one")
            )
            second = package_release(
                release_options(source, binaries, head, root / "release-two")
            )
            self.assertEqual(first["binary_asset"], second["binary_asset"])
            self.assertEqual(first["source_archive"], second["source_archive"])
            self.assertEqual(first["sbom"], second["sbom"])

            output = root / "release-one"
            binary_asset = output / first["binary_asset"]["name"]
            source_asset = output / first["source_archive"]["name"]
            sbom = output / first["sbom"]["name"]
            checksums = output / first["checksums"]
            checksum_lines = checksums.read_text(encoding="ascii").splitlines()
            self.assertEqual(
                checksum_lines,
                [
                    f"{hashlib.sha256(binary_asset.read_bytes()).hexdigest()}  {binary_asset.name}",
                    f"{hashlib.sha256(source_asset.read_bytes()).hexdigest()}  {source_asset.name}",
                    f"{hashlib.sha256(sbom.read_bytes()).hexdigest()}  {sbom.name}",
                ],
            )

            binary_root = "ninfer-rtx3090-omp-v0.2.0-linux-x86_64-cuda12.8"
            with tarfile.open(binary_asset, "r:gz") as archive:
                names = archive.getnames()
                self.assertIn(f"{binary_root}/bin/ninfer", names)
                self.assertIn(f"{binary_root}/bin/ninfer-serve", names)
                self.assertIn(f"{binary_root}/bin/ninfer_bench", names)
                self.assertIn(f"{binary_root}/README.md", names)
                self.assertIn(f"{binary_root}/SHA256SUMS.txt", names)
                self.assertEqual(len(names), 17)
                for member in archive.getmembers():
                    self.assertEqual(member.uid, 0)
                    self.assertEqual(member.gid, 0)
                    self.assertEqual(member.uname, "")
                    self.assertEqual(member.gname, "")
                    self.assertEqual(member.mtime, _EPOCH)

                identity_stream = archive.extractfile(
                    f"{binary_root}/build-identity.json"
                )
                self.assertIsNotNone(identity_stream)
                identity = json.load(identity_stream)
                self.assertEqual(identity["patch_stack_sha"], head)
                self.assertFalse(identity["source_dirty"])
                self.assertEqual(
                    identity["source_archive_sha256"],
                    hashlib.sha256(source_asset.read_bytes()).hexdigest(),
                )
                self.assertNotIn(str(source), json.dumps(identity))

                inner_stream = archive.extractfile(f"{binary_root}/SHA256SUMS.txt")
                self.assertIsNotNone(inner_stream)
                inner_lines = inner_stream.read().decode("ascii").splitlines()
                self.assertEqual(len(inner_lines), 14)
                for line in inner_lines:
                    digest, relative = line.split("  ", maxsplit=1)
                    payload_stream = archive.extractfile(f"{binary_root}/{relative}")
                    self.assertIsNotNone(payload_stream)
                    self.assertEqual(
                        digest, hashlib.sha256(payload_stream.read()).hexdigest()
                    )

            source_root = "ninfer-rtx3090-omp-v0.2.0-source"
            with tarfile.open(source_asset, "r:gz") as archive:
                names = archive.getnames()
                self.assertIn(f"{source_root}/tracked.txt", names)
                self.assertIn(f"{source_root}/LICENSE", names)
                self.assertFalse(any("/.git" in name for name in names))
                tracked_stream = archive.extractfile(f"{source_root}/tracked.txt")
                self.assertIsNotNone(tracked_stream)
                self.assertEqual(tracked_stream.read(), b"release bytes\n")

            spdx = json.loads(sbom.read_text(encoding="utf-8"))
            self.assertEqual(spdx["spdxVersion"], "SPDX-2.3")
            self.assertEqual(len(spdx["files"]), 15)
            self.assertTrue(spdx["packages"][0]["filesAnalyzed"])

            (source / "VERSION").write_text("dirty\n", encoding="utf-8")
            with self.assertRaisesRegex(ReleaseError, "source tree must be clean"):
                package_release(
                    release_options(source, binaries, head, root / "dirty-release")
                )

    def test_release_requires_exact_available_commits(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, binaries, head = create_release_fixture(root)
            options = release_options(source, binaries, head, root / "wrong-head")
            with self.assertRaisesRegex(ReleaseError, "outside the source tree"):
                package_release(
                    dataclasses.replace(options, output_dir=source / "dist")
                )

            with self.assertRaisesRegex(ReleaseError, "source HEAD differs"):
                package_release(
                    dataclasses.replace(options, release_head_sha="0" * 40)
                )

            with self.assertRaisesRegex(ReleaseError, "not an ancestor"):
                package_release(
                    dataclasses.replace(
                        options,
                        output_dir=root / "wrong-upstream",
                        upstream_base_sha="0" * 40,
                    )
                )


if __name__ == "__main__":
    unittest.main()
