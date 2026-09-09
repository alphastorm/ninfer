from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

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
                "cuda_toolkit=13.1.115 cuda_architecture=120a source_dirty=false"
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

            def options(output: Path) -> ReleaseOptions:
                return ReleaseOptions(
                    source=source,
                    ninfer=ninfer,
                    ninfer_serve=ninfer_serve,
                    output_dir=output,
                    release_version="v0.1.0",
                    platform="linux-x86_64-cuda13.1",
                    upstream_base_sha=head,
                    release_head_sha=head,
                    build_profile="qwen38-5090-v0.1.0",
                    source_date_epoch=1_700_000_000,
                )

            first = package_release(options(root / "release-one"))
            second = package_release(options(root / "release-two"))
            self.assertEqual(first["asset"], second["asset"])
            self.assertEqual(first["sbom"], second["sbom"])

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
            self.assertFalse(identity["source_dirty"])
            self.assertNotIn(str(source), json.dumps(identity))

            spdx = json.loads(sbom.read_text(encoding="utf-8"))
            self.assertEqual(spdx["spdxVersion"], "SPDX-2.3")
            self.assertEqual(len(spdx["files"]), 4)
            self.assertTrue(spdx["packages"][0]["filesAnalyzed"])

            (source / "LICENSE").write_text("dirty\n", encoding="utf-8")
            with self.assertRaisesRegex(ReleaseError, "source tree must be clean"):
                package_release(options(root / "dirty-release"))


_NATIVE_SUPPORT = {
    "LICENSE": "Apache License fixture\n",
    "README.md": "NInfer fixture\n",
    "packaging/windows/Install-Release.ps1": "# installer fixture\n",
    "packaging/windows/Control-Release.ps1": "# controller fixture\n",
    "packaging/windows/Control-GpuOwner.ps1": "# gpu owner fixture\n",
    "packaging/windows/Protect-StateRoot.ps1": "# state protection fixture\n",
    "packaging/windows/agent_protocol.py": "# protocol fixture\n",
    "packaging/windows/lanes/rtx3090/release-spec.json": '{"release_id":"fixture"}\n',
    "packaging/windows/RELEASE_NOTES.md": "Release notes fixture\n",
    "tools/smoke/serve_contract.py": "# smoke contract fixture\n",
}


class NativeWindowsPackageTests(unittest.TestCase):
    """The native lane package: one packager, lane names from the caller."""

    def fixture(self, root: Path) -> tuple[Path, Path, str]:
        source = root / "source"
        binaries = root / "binaries"
        source.mkdir()
        binaries.mkdir()

        def git(*arguments: str) -> str:
            return subprocess.run(
                ["git", "-C", str(source), *arguments],
                check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            ).stdout.strip()

        git("init", "--quiet")
        git("config", "user.name", "NInfer Test")
        git("config", "user.email", "ninfer-test@example.invalid")
        for name, content in _NATIVE_SUPPORT.items():
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        git("add", "-A")
        git("commit", "--quiet", "-m", "runtime fixture")
        runtime_head = git("rev-parse", "HEAD")
        (source / "packaging/windows/README-lane.md").write_text("packaging only\n", encoding="utf-8")
        git("add", "-A")
        git("commit", "--quiet", "-m", "packaging fixture")
        head = git("rev-parse", "HEAD")
        common = (
            f"upstream_base_sha={runtime_head} patch_stack_sha={runtime_head} "
            "build_profile=native-v0.6.0-rtx3090 build_type=Release "
            "cxx_compiler=MSVC-19.44.35207 cuda_compiler=NVIDIA-13.3.52 "
            "cuda_toolkit=13.3.52 cuda_architecture=86 source_dirty=false"
        )
        for program in ("ninfer", "ninfer-serve", "ninfer_bench"):
            path = binaries / program
            path.write_text(f"#!/bin/sh\nprintf '%s\\n' '{program} {common}'\n", encoding="utf-8")
            path.chmod(0o755)
        for dll in ("libcrypto-3-x64.dll", "avcodec-62.dll"):
            (binaries / dll).write_bytes(dll.encode("ascii"))
        (root / "server-config.json").write_text('{"engine":{"kv_dtype":"int8"}}\n', encoding="utf-8")
        self.runtime_head = runtime_head
        return source, binaries, head

    def options(self, source: Path, binaries: Path, head: str, output: Path, **overrides) -> ReleaseOptions:
        values = dict(
            source=source,
            ninfer=binaries / "ninfer",
            ninfer_serve=binaries / "ninfer-serve",
            ninfer_bench=binaries / "ninfer_bench",
            output_dir=output,
            release_version="v0.6.0",
            platform="windows-x86_64-cuda13.3-rtx3090",
            upstream_base_sha=self.runtime_head,
            release_head_sha=head,
            runtime_source_sha=self.runtime_head,
            build_profile="native-v0.6.0-rtx3090",
            product_prefix="ninfer-rtx3090-omp",
            cuda_architecture="86",
            runtime_dependencies=(binaries / "libcrypto-3-x64.dll", binaries / "avcodec-62.dll"),
            windows_server_config=output.parent / "server-config.json",
            lane_dir="packaging/windows/lanes/rtx3090",
            release_notes="packaging/windows/RELEASE_NOTES.md",
            source_date_epoch=1_700_000_000,
        )
        values.update(overrides)
        return ReleaseOptions(**values)

    def test_deterministic_native_package_with_trailing_runtime_source(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, binaries, head = self.fixture(root)
            first = package_release(self.options(source, binaries, head, root / "one"))
            second = package_release(self.options(source, binaries, head, root / "two"))
            self.assertEqual(first["binary_asset"], second["binary_asset"])
            self.assertEqual(first["source_archive"], second["source_archive"])
            self.assertEqual(first["sbom"], second["sbom"])
            self.assertEqual(first["patch_stack_sha"], self.runtime_head)
            self.assertEqual(first["package_source_sha"], head)
            self.assertEqual(first["cuda_architecture"], "86")

            output = root / "one"
            binary_root = "ninfer-rtx3090-omp-v0.6.0-windows-x86_64-cuda13.3-rtx3090"
            asset = output / f"{binary_root}.tar.gz"
            self.assertEqual(sorted(path.name for path in output.iterdir()), sorted([
                asset.name, "ninfer-rtx3090-omp-v0.6.0-source.tar.gz",
                f"{binary_root}.spdx.json", f"{binary_root}.SHA256SUMS",
            ]))
            with tarfile.open(asset, "r:gz") as archive:
                names = set(archive.getnames())
                expected = {
                    f"{binary_root}/bin/ninfer.exe", f"{binary_root}/bin/ninfer-serve.exe",
                    f"{binary_root}/bin/ninfer_bench.exe", f"{binary_root}/bin/libcrypto-3-x64.dll",
                    f"{binary_root}/bin/avcodec-62.dll", f"{binary_root}/Install-Release.ps1",
                    f"{binary_root}/Control-Release.ps1", f"{binary_root}/Control-GpuOwner.ps1",
                    f"{binary_root}/Protect-StateRoot.ps1", f"{binary_root}/smoke/agent_protocol.py",
                    f"{binary_root}/smoke/serve_contract.py", f"{binary_root}/release-spec.json",
                    f"{binary_root}/server-config.json", f"{binary_root}/build-identity.json",
                    f"{binary_root}/SHA256SUMS.txt", f"{binary_root}/VERSION", f"{binary_root}/LICENSE",
                    f"{binary_root}/README.md", f"{binary_root}/RELEASE_NOTES.md",
                }
                self.assertTrue(expected <= names, expected - names)
                self.assertNotIn(f"{binary_root}/README-lane.md", names)
                identity = json.load(archive.extractfile(f"{binary_root}/build-identity.json"))
                self.assertEqual(identity["schema_version"], 2)
                self.assertEqual(identity["patch_stack_sha"], self.runtime_head)
                self.assertEqual(identity["cuda_architecture"], "86")
                self.assertEqual(sorted(identity["runtime_dependencies"]), ["avcodec-62.dll", "libcrypto-3-x64.dll"])
                self.assertEqual(identity["configuration_sha256"], first["configuration_sha256"])
                self.assertEqual(archive.extractfile(f"{binary_root}/VERSION").read(), b"0.6.0\n")
                for line in archive.extractfile(f"{binary_root}/SHA256SUMS.txt").read().decode("ascii").splitlines():
                    digest, relative = line.split("  ", maxsplit=1)
                    self.assertEqual(digest, hashlib.sha256(archive.extractfile(f"{binary_root}/{relative}").read()).hexdigest())
                for member in archive.getmembers():
                    self.assertEqual((member.uid, member.gid, member.mtime), (0, 0, 1_700_000_000))

    def test_native_package_binds_architecture_and_lane_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, binaries, head = self.fixture(root)
            with self.assertRaisesRegex(ReleaseError, "cuda_architecture"):
                package_release(self.options(source, binaries, head, root / "arch", cuda_architecture="89"))
            with self.assertRaisesRegex(ReleaseError, "runtime dependencies"):
                package_release(self.options(source, binaries, head, root / "deps", runtime_dependencies=()))
            with self.assertRaisesRegex(ReleaseError, "release-spec.json"):
                package_release(self.options(source, binaries, head, root / "lane", lane_dir=None))
            with self.assertRaisesRegex(ReleaseError, "only accepted for Windows"):
                package_release(self.options(source, binaries, head, root / "linux", platform="linux-x86_64-cuda13.1"))


if __name__ == "__main__":
    unittest.main()
