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
from unittest.mock import patch

from tools.release.package import (
    ReleaseError,
    ReleaseOptions,
    package_release,
    parse_build_info,
)


_COMMIT_DATE = "2023-11-14T22:13:20+00:00"
_EPOCH = 1_700_000_000
_SUPPORT_CONTENT = {
    "README.md": "NInfer RTX 3090 fixture\n",
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
    "packaging/windows/qwen38-3090-omp-v0.2/Install-Release.ps1": "# installer fixture\n",
    "packaging/windows/qwen38-3090-omp-v0.2/Control-Release.ps1": "# controller fixture\n",
    "packaging/windows/qwen38-3090-omp-v0.2/New-QualificationReceipt.ps1": "# receipt fixture\n",
    "packaging/windows/qwen38-3090-omp-v0.2/agent_protocol.py": "# protocol fixture\n",
    "packaging/windows/qwen38-3090-omp-v0.2/release-spec.json": "{}\n",
    "packaging/windows/qwen38-3090-omp-v0.2/server-config.json": "{\"fixture\":true}\n",
    "tools/smoke/serve_contract.py": "# smoke contract fixture\n",
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
    run_git(source, "config", "core.autocrlf", "false")
    run_git(source, "config", "user.name", "NInfer Test")
    run_git(source, "config", "user.email", "ninfer-test@example.invalid")
    for name, content in _SUPPORT_CONTENT.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content.encode("utf-8"))
        if name.startswith("scripts/"):
            path.chmod(0o755)
    (source / "tracked.txt").write_bytes(b"release bytes\n")
    run_git(source, "add", ".")
    run_git(source, "commit", "--quiet", "-m", "release fixture", commit_date=True)
    return source, binaries, run_git(source, "rev-parse", "HEAD")


def write_fake_binaries(
    binaries: Path,
    head: str,
    *,
    cuda_architecture: str = "86",
    benchmark_bound: bool = True,
) -> tuple[Path, Path, Path]:
    common = (
        f"upstream_base_sha={head} patch_stack_sha={head} "
        "build_profile=omp-v0.2.0-rtx3090 build_type=Release "
        "cxx_compiler=GNU-13.3.0 cuda_compiler=NVIDIA-12.8.93 "
        f"cuda_toolkit=12.8.93 cuda_architecture={cuda_architecture} source_dirty=false"
    )
    suffix = ".exe" if os.name == "nt" else ""
    ninfer = binaries / f"ninfer{suffix}"
    ninfer_serve = binaries / f"ninfer-serve{suffix}"
    ninfer_bench = binaries / f"ninfer_bench{suffix}"
    identities = (
        (ninfer, "ninfer"),
        (ninfer_serve, "ninfer-serve"),
        (ninfer_bench, "ninfer_bench"),
    )
    if os.name == "nt":
        compiler = shutil.which("cl.exe")
        if compiler is None:
            raise RuntimeError("cl.exe is required for native Windows release fixtures")
        fixture_id = f"{cuda_architecture}-{int(benchmark_bound)}"
        source = binaries / f"fake_build_info-{fixture_id}.cpp"
        template = binaries / f"fake_build_info-{fixture_id}.exe"
        source.write_text(
            textwrap.dedent(
                f"""\
                #include <iostream>
                #include <string>

                int main(int argc, char** argv) {{
                    std::string name = argc > 0 ? argv[0] : "ninfer";
                    const auto separator = name.find_last_of("/\\\\");
                    if (separator != std::string::npos) name.erase(0, separator + 1);
                    if (name.size() >= 4 && name.substr(name.size() - 4) == ".exe") {{
                        name.erase(name.size() - 4);
                    }}
                    if (name == "ninfer_bench" && {str(benchmark_bound).lower()} == false) {{
                        return 0;
                    }}
                    std::cout << name << " " << {json.dumps(common)} << "\\n";
                    return 0;
                }}
                """
            ),
            encoding="utf-8",
        )
        if not template.exists():
            subprocess.run(
                [compiler, "/nologo", "/EHsc", str(source), f"/Fe:{template}"],
                cwd=binaries,
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
        for binary, _ in identities:
            shutil.copyfile(template, binary)
    else:
        for binary, name in identities:
            if name == "ninfer_bench" and not benchmark_bound:
                binary.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            else:
                binary.write_text(
                    f"#!/bin/sh\nprintf '%s\\n' '{name} {common}'\n",
                    encoding="utf-8",
                )
    for binary in (ninfer, ninfer_serve, ninfer_bench):
        binary.chmod(0o755)
    return ninfer, ninfer_serve, ninfer_bench


def release_options(
    source: Path,
    binaries: Path,
    head: str,
    output: Path,
    *,
    cuda_architecture: str = "86",
    benchmark_bound: bool = True,
) -> ReleaseOptions:
    ninfer, ninfer_serve, ninfer_bench = write_fake_binaries(
        binaries,
        head,
        cuda_architecture=cuda_architecture,
        benchmark_bound=benchmark_bound,
    )
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
    def test_mainline_build_does_not_claim_release_profile(self) -> None:
        root = Path(__file__).resolve().parents[1]
        presets = json.loads((root / "CMakePresets.json").read_text(encoding="utf-8"))
        self.assertEqual(len(presets["configurePresets"]), 1)
        preset = presets["configurePresets"][0]
        self.assertEqual(preset["name"], "mainline-rtx-3090-omp-v0.2")
        self.assertEqual(
            preset["cacheVariables"]["NINFER_BUILD_PROFILE"],
            "omp-v0.2-rtx3090-mainline",
        )
        self.assertNotEqual(
            preset["cacheVariables"]["NINFER_BUILD_PROFILE"],
            "omp-v0.2.0-rtx3090",
        )

    def test_crypto_dependencies_cover_windows_and_container_runtime(self) -> None:
        root = Path(__file__).resolve().parents[1]
        manifest = json.loads((root / "vcpkg.json").read_text(encoding="utf-8"))
        dependency_names = {
            dependency if isinstance(dependency, str) else dependency["name"]
            for dependency in manifest["dependencies"]
        }
        self.assertIn("openssl", dependency_names)
        dockerfile = (root / "Dockerfile").read_text(encoding="utf-8")
        self.assertIn("libssl-dev", dockerfile)
        self.assertIn("libssl3t64", dockerfile)

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
        if compiler is None:
            self.skipTest("a host C++ compiler is unavailable")
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
                    #define NINFER_BUILD_CUDA_ARCHITECTURE "86"
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
            self.assertEqual(values["cuda_architecture"], "86")
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
                committed = subprocess.run(
                    ["git", "-C", str(source), "show", f"{head}:tracked.txt"],
                    check=True,
                    stdout=subprocess.PIPE,
                ).stdout
                self.assertEqual(tracked_stream.read(), committed)

            spdx = json.loads(sbom.read_text(encoding="utf-8"))
            self.assertEqual(spdx["spdxVersion"], "SPDX-2.3")
            self.assertEqual(len(spdx["files"]), 15)
            self.assertTrue(spdx["packages"][0]["filesAnalyzed"])

            (source / "VERSION").write_text("dirty\n", encoding="utf-8")
            with self.assertRaisesRegex(ReleaseError, "source tree must be clean"):
                package_release(
                    release_options(source, binaries, head, root / "dirty-release")
                )

    def test_publication_is_one_retryable_directory_replace(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, binaries, head = create_release_fixture(root)
            output = root / "release"
            options = release_options(source, binaries, head, output)
            with patch("tools.release.package.os.replace", side_effect=OSError("injected")):
                with self.assertRaises(OSError):
                    package_release(options)
            self.assertFalse(output.exists())

            receipt = package_release(options)
            self.assertEqual(receipt["artifact_type"], "ninfer_local_release_receipt")
            self.assertEqual(len(list(output.iterdir())), 4)

    def test_release_rejects_wrong_architecture_and_unbound_benchmark(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, binaries, head = create_release_fixture(root)
            options = release_options(
                source,
                binaries,
                head,
                root / "wrong-arch",
                cuda_architecture="89",
            )
            with self.assertRaisesRegex(ReleaseError, "cuda_architecture"):
                package_release(options)

            options = release_options(
                source,
                binaries,
                head,
                root / "unbound-bench",
                benchmark_bound=False,
            )
            with self.assertRaisesRegex(ReleaseError, "ninfer_bench"):
                package_release(options)

    def test_windows_package_includes_only_declared_app_local_dependencies(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, binaries, head = create_release_fixture(root)
            dependency_a = binaries / "libcrypto-3-x64.dll"
            dependency_b = binaries / "avcodec-62.dll"
            dependency_a.write_bytes(b"crypto fixture")
            dependency_b.write_bytes(b"codec fixture")
            output = root / "windows-release"
            options = dataclasses.replace(
                release_options(source, binaries, head, output),
                platform="windows-x86_64-cuda12.8-rtx3090",
                lineage_base_sha=head,
                runtime_dependencies=(dependency_a, dependency_b),
                windows_server_config=(
                    source
                    / "packaging/windows/qwen38-3090-omp-v0.2/server-config.json"
                ),
            )
            receipt = package_release(options)
            root_name = "ninfer-rtx3090-omp-v0.2.0-windows-x86_64-cuda12.8-rtx3090"
            archive = output / f"{root_name}.tar.gz"
            with tarfile.open(archive, "r:gz") as tar:
                names = set(tar.getnames())
                identity_stream = tar.extractfile(f"{root_name}/build-identity.json")
                self.assertIsNotNone(identity_stream)
                identity = json.load(identity_stream)
            self.assertIn(f"{root_name}/bin/ninfer.exe", names)
            self.assertIn(f"{root_name}/bin/ninfer-serve.exe", names)
            self.assertIn(f"{root_name}/bin/ninfer_bench.exe", names)
            self.assertIn(f"{root_name}/bin/libcrypto-3-x64.dll", names)
            self.assertIn(f"{root_name}/bin/avcodec-62.dll", names)
            self.assertIn(f"{root_name}/Install-Release.ps1", names)
            self.assertIn(f"{root_name}/Control-Release.ps1", names)
            self.assertIn(f"{root_name}/New-QualificationReceipt.ps1", names)
            self.assertIn(f"{root_name}/release-spec.json", names)
            self.assertIn(f"{root_name}/server-config.json", names)
            self.assertIn(f"{root_name}/smoke/agent_protocol.py", names)
            self.assertIn(f"{root_name}/smoke/serve_contract.py", names)
            self.assertNotIn(f"{root_name}/run-qwen38-c1.sh", names)
            config_bytes = (
                source
                / "packaging/windows/qwen38-3090-omp-v0.2/server-config.json"
            ).read_bytes()
            self.assertEqual(identity["lineage_base_sha"], head)
            self.assertEqual(
                identity["configuration_sha256"],
                hashlib.sha256(config_bytes).hexdigest(),
            )
            self.assertEqual(receipt["configuration_sha256"], identity["configuration_sha256"])

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
