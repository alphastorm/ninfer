#!/usr/bin/env python3
"""Create deterministic RTX 3090 source and binary release assets with checksums and SPDX."""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tarfile
import tempfile
from typing import NoReturn
import uuid


_GIT_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
_VERSION_RE = re.compile(r"^v[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$")
_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
_BUILD_KEYS = frozenset(
    {
        "upstream_base_sha",
        "patch_stack_sha",
        "build_profile",
        "build_type",
        "cxx_compiler",
        "cuda_compiler",
        "cuda_toolkit",
        "source_dirty",
    }
)
_MAX_GZIP_EPOCH = (1 << 32) - 1
_PRODUCT_PREFIX = "ninfer-rtx3090-omp"
_SUPPORT_FILES = (
    ("VERSION", "VERSION", 0o644),
    ("LICENSE", "LICENSE", 0o644),
    ("RELEASE_NOTES_0.6.1.md", "RELEASE_NOTES_0.6.1.md", 0o644),
    ("docs/rtx-3090-linux.md", "README.md", 0o644),
    ("scripts/download-qwen38.sh", "download-qwen38.sh", 0o755),
    ("scripts/run-qwen38-c1.sh", "run-qwen38-c1.sh", 0o755),
    ("scripts/run-qwen38-c8.sh", "run-qwen38-c8.sh", 0o755),
    ("scripts/run-qwen38-vision.sh", "run-qwen38-vision.sh", 0o755),
    (
        "scripts/download-qwen36-35b-vision.sh",
        "download-qwen36-35b-vision.sh",
        0o755,
    ),
    ("scripts/run-qwen36-35b-vision.sh", "run-qwen36-35b-vision.sh", 0o755),
)


class ReleaseError(RuntimeError):
    """A release input does not prove the requested immutable identity."""


def fail(message: str) -> NoReturn:
    raise ReleaseError(message)


def run(
    command: list[str], *, cwd: Path | None = None, text: bool = True
) -> subprocess.CompletedProcess[str] | subprocess.CompletedProcess[bytes]:
    try:
        return subprocess.run(
            command,
            cwd=cwd,
            check=True,
            text=text,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except FileNotFoundError as error:
        raise ReleaseError(f"required executable is unavailable: {command[0]}") from error
    except subprocess.CalledProcessError as error:
        stderr = error.stderr
        detail = (
            stderr.decode("utf-8", errors="replace")
            if isinstance(stderr, bytes)
            else stderr or ""
        )
        lines = detail.strip().splitlines()
        suffix = f": {lines[-1]}" if lines else ""
        raise ReleaseError(
            f"{command[0]} command failed with exit {error.returncode}{suffix}"
        ) from error


def hash_path(path: Path) -> tuple[str, str, int]:
    sha256 = hashlib.sha256()
    sha1 = hashlib.sha1(usedforsecurity=False)
    size = 0
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                size += len(block)
                sha256.update(block)
                sha1.update(block)
    except OSError as error:
        raise ReleaseError(f"failed to read release input {path.name}") from error
    return sha256.hexdigest(), sha1.hexdigest(), size


def hash_bytes(value: bytes) -> tuple[str, str, int]:
    return (
        hashlib.sha256(value).hexdigest(),
        hashlib.sha1(value, usedforsecurity=False).hexdigest(),
        len(value),
    )


def verify_source(source: Path, upstream_sha: str, release_sha: str) -> int:
    if not _GIT_SHA_RE.fullmatch(upstream_sha):
        fail("expected upstream base must be a full lowercase Git SHA")
    if not _GIT_SHA_RE.fullmatch(release_sha):
        fail("expected release head must be a full lowercase Git SHA")
    head = str(run(["git", "rev-parse", "HEAD"], cwd=source).stdout).strip()
    if head != release_sha:
        fail("source HEAD differs from the expected release head")
    status = str(
        run(
            ["git", "status", "--porcelain", "--untracked-files=all"], cwd=source
        ).stdout
    )
    if status:
        fail("source tree must be clean before packaging a release")
    try:
        ancestor = subprocess.run(
            ["git", "merge-base", "--is-ancestor", upstream_sha, release_sha],
            cwd=source,
            text=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except FileNotFoundError as error:
        raise ReleaseError("required executable is unavailable: git") from error
    if ancestor.returncode != 0:
        fail("upstream base is unavailable or is not an ancestor of the release head")
    timestamp = str(
        run(["git", "show", "-s", "--format=%ct", release_sha], cwd=source).stdout
    ).strip()
    try:
        epoch = int(timestamp)
    except ValueError as error:
        raise ReleaseError("release commit timestamp is invalid") from error
    if not 0 <= epoch <= _MAX_GZIP_EPOCH:
        fail("release commit timestamp is outside the deterministic archive range")
    return epoch


def read_committed_file(source: Path, release_sha: str, name: str) -> bytes:
    result = run(["git", "show", f"{release_sha}:{name}"], cwd=source, text=False)
    assert isinstance(result.stdout, bytes)
    if not result.stdout:
        fail(f"the release commit contains an empty {name}")
    return result.stdout


def parse_build_info(binary: Path, expected_program: str) -> dict[str, str]:
    if not binary.is_file():
        fail(f"{expected_program} binary does not exist")
    result = run([os.fspath(binary), "--version"])
    assert isinstance(result.stdout, str)
    lines = result.stdout.strip().splitlines()
    if len(lines) != 1:
        fail(f"{expected_program} --version must emit exactly one line")
    try:
        tokens = shlex.split(lines[0])
    except ValueError as error:
        raise ReleaseError(f"{expected_program} emitted malformed build identity") from error
    if not tokens or tokens[0] != expected_program:
        fail(f"{expected_program} emitted the wrong program identity")
    values: dict[str, str] = {}
    for token in tokens[1:]:
        key, separator, value = token.partition("=")
        if not separator or not key or not value or key in values:
            fail(f"{expected_program} emitted malformed build identity")
        values[key] = value
    if values.keys() != _BUILD_KEYS:
        fail(f"{expected_program} emitted an unexpected build identity schema")
    return values


@dataclasses.dataclass(frozen=True, slots=True)
class ReleaseOptions:
    source: Path
    ninfer: Path
    ninfer_serve: Path
    ninfer_bench: Path
    output_dir: Path
    release_version: str
    platform: str
    upstream_base_sha: str
    release_head_sha: str
    build_profile: str
    source_date_epoch: int | None = None


@dataclasses.dataclass(frozen=True, slots=True)
class ReleaseFile:
    archive_name: str
    mode: int
    sha256: str
    sha1: str
    size: int
    source: Path | None = None
    data: bytes | None = None

    @classmethod
    def from_path(cls, archive_name: str, mode: int, source: Path) -> "ReleaseFile":
        sha256, sha1, size = hash_path(source)
        return cls(archive_name, mode, sha256, sha1, size, source=source)

    @classmethod
    def from_bytes(cls, archive_name: str, mode: int, data: bytes) -> "ReleaseFile":
        sha256, sha1, size = hash_bytes(data)
        return cls(archive_name, mode, sha256, sha1, size, data=data)


def validate_options(options: ReleaseOptions) -> tuple[dict[str, str], int]:
    if not _VERSION_RE.fullmatch(options.release_version):
        fail("release version must be a complete vMAJOR.MINOR.PATCH value")
    if not _NAME_RE.fullmatch(options.platform) or not options.platform.startswith("linux-"):
        fail("platform must be a release-safe Linux platform name")
    if not _NAME_RE.fullmatch(options.build_profile):
        fail("build profile must contain only release-safe name characters")
    source = options.source.resolve()
    output = options.output_dir.resolve()
    if output == source or source in output.parents:
        fail("output directory must be outside the source tree")
    commit_epoch = verify_source(
        source, options.upstream_base_sha, options.release_head_sha
    )
    identities = [
        parse_build_info(options.ninfer.resolve(), "ninfer"),
        parse_build_info(options.ninfer_serve.resolve(), "ninfer-serve"),
    ]
    if identities[0] != identities[1]:
        fail("ninfer and ninfer-serve carry different build identities")
    if not options.ninfer_bench.resolve().is_file():
        fail("ninfer_bench binary does not exist")
    identity = identities[0]
    expected = {
        "upstream_base_sha": options.upstream_base_sha,
        "patch_stack_sha": options.release_head_sha,
        "build_profile": options.build_profile,
        "build_type": "Release",
        "source_dirty": "false",
    }
    for key, value in expected.items():
        if identity[key] != value:
            fail(f"binary build identity mismatch for {key}")
    epoch = options.source_date_epoch
    if epoch is None:
        environment_epoch = os.environ.get("SOURCE_DATE_EPOCH")
        if environment_epoch is not None:
            try:
                epoch = int(environment_epoch)
            except ValueError as error:
                raise ReleaseError(
                    "SOURCE_DATE_EPOCH must be an archive-range integer"
                ) from error
        else:
            epoch = commit_epoch
    if isinstance(epoch, bool) or not 0 <= epoch <= _MAX_GZIP_EPOCH:
        fail("source date epoch must be in 0..4294967295")
    return identity, epoch


def add_tar_directory(archive: tarfile.TarFile, name: str, epoch: int) -> None:
    info = tarfile.TarInfo(name=name.rstrip("/") + "/")
    info.type = tarfile.DIRTYPE
    info.mode = 0o755
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = epoch
    archive.addfile(info)


def add_tar_file(archive: tarfile.TarFile, item: ReleaseFile, epoch: int) -> None:
    info = tarfile.TarInfo(name=item.archive_name)
    info.mode = item.mode
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = epoch
    info.size = item.size
    if item.source is not None:
        with item.source.open("rb") as stream:
            archive.addfile(info, stream)
        return
    assert item.data is not None
    archive.addfile(info, io.BytesIO(item.data))


def write_binary_archive(
    path: Path, root: str, files: list[ReleaseFile], epoch: int
) -> None:
    with path.open("wb") as raw:
        with gzip.GzipFile(
            filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=epoch
        ) as compressed:
            with tarfile.open(
                fileobj=compressed, mode="w", format=tarfile.GNU_FORMAT
            ) as archive:
                add_tar_directory(archive, root, epoch)
                add_tar_directory(archive, f"{root}/bin", epoch)
                for item in sorted(files, key=lambda value: value.archive_name):
                    add_tar_file(archive, item, epoch)


def write_source_archive(
    path: Path, source: Path, release_sha: str, root: str, epoch: int
) -> None:
    raw_tar = path.with_name(path.name + ".raw.tar")
    try:
        run(
            [
                "git",
                "archive",
                "--format=tar",
                f"--prefix={root}/",
                f"--output={raw_tar}",
                release_sha,
            ],
            cwd=source,
        )
        with raw_tar.open("rb") as source_stream, path.open("wb") as destination:
            with gzip.GzipFile(
                filename="",
                mode="wb",
                fileobj=destination,
                compresslevel=9,
                mtime=epoch,
            ) as compressed:
                shutil.copyfileobj(source_stream, compressed, length=8 * 1024 * 1024)
    except OSError as error:
        raise ReleaseError("failed to create the deterministic source archive") from error
    finally:
        raw_tar.unlink(missing_ok=True)


def spdx_id(name: str) -> str:
    return "SPDXRef-File-" + re.sub(r"[^A-Za-z0-9.-]", "-", name)


def build_spdx(
    *,
    root: str,
    release_version: str,
    identity_bytes: bytes,
    files: list[ReleaseFile],
    epoch: int,
) -> bytes:
    identity_digest = hashlib.sha256(identity_bytes).hexdigest()
    namespace_id = uuid.uuid5(uuid.NAMESPACE_URL, f"ninfer:{root}:{identity_digest}")
    package_id = "SPDXRef-Package-NInfer"
    spdx_files = []
    relationships = [
        {
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": package_id,
        }
    ]
    for item in sorted(files, key=lambda value: value.archive_name):
        file_id = spdx_id(item.archive_name)
        spdx_files.append(
            {
                "SPDXID": file_id,
                "fileName": f"./{item.archive_name}",
                "checksums": [
                    {"algorithm": "SHA1", "checksumValue": item.sha1},
                    {"algorithm": "SHA256", "checksumValue": item.sha256},
                ],
                "licenseConcluded": "NOASSERTION",
                "copyrightText": "NOASSERTION",
            }
        )
        relationships.append(
            {
                "spdxElementId": package_id,
                "relationshipType": "CONTAINS",
                "relatedSpdxElement": file_id,
            }
        )
    verification_code = hashlib.sha1(
        "".join(sorted(item.sha1 for item in files)).encode("ascii"),
        usedforsecurity=False,
    ).hexdigest()
    created = dt.datetime.fromtimestamp(epoch, tz=dt.UTC).strftime("%Y-%m-%dT%H:%M:%SZ")
    document = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"{root}-sbom",
        "documentNamespace": (
            "https://github.com/Don-Chad/ninfer-3090/releases/"
            f"{release_version}/sbom-{namespace_id}"
        ),
        "creationInfo": {
            "created": created,
            "creators": ["Tool: ninfer-release-package"],
        },
        "packages": [
            {
                "name": "NInfer RTX 3090 OMP runtime",
                "SPDXID": package_id,
                "versionInfo": release_version.removeprefix("v"),
                "downloadLocation": "NOASSERTION",
                "filesAnalyzed": True,
                "packageVerificationCode": {
                    "packageVerificationCodeValue": verification_code
                },
                "licenseConcluded": "NOASSERTION",
                "licenseDeclared": "Apache-2.0",
                "copyrightText": "NOASSERTION",
                "hasFiles": [spdx_id(item.archive_name) for item in files],
            }
        ],
        "files": spdx_files,
        "relationships": relationships,
    }
    return (json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n").encode(
        "utf-8"
    )


def inner_checksums(root: str, files: list[ReleaseFile]) -> bytes:
    prefix = root + "/"
    lines = []
    for item in sorted(files, key=lambda value: value.archive_name):
        if not item.archive_name.startswith(prefix):
            fail("release file escaped the binary archive root")
        lines.append(f"{item.sha256}  {item.archive_name.removeprefix(prefix)}\n")
    return "".join(lines).encode("ascii")


def stage_binary(source: Path, destination: Path, name: str) -> Path:
    try:
        shutil.copyfile(source.resolve(), destination)
        destination.chmod(0o755)
    except OSError as error:
        raise ReleaseError(f"failed to stage {name} binary") from error
    return destination


def package_release(options: ReleaseOptions) -> dict[str, object]:
    identity, epoch = validate_options(options)
    source = options.source.resolve()
    release_base = f"{_PRODUCT_PREFIX}-{options.release_version}"
    binary_root = f"{release_base}-{options.platform}"
    source_root = f"{release_base}-source"
    binary_name = f"{binary_root}.tar.gz"
    source_name = f"{source_root}.tar.gz"
    sbom_name = f"{binary_root}.spdx.json"
    checksums_name = f"{binary_root}.SHA256SUMS"

    output = options.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    destinations = [
        output / binary_name,
        output / source_name,
        output / sbom_name,
        output / checksums_name,
    ]
    existing = [path.name for path in destinations if path.exists()]
    if existing:
        fail("release outputs already exist: " + ", ".join(existing))

    with tempfile.TemporaryDirectory(prefix=".ninfer-release-", dir=output) as directory:
        temporary = Path(directory)
        binary_path = temporary / binary_name
        source_path = temporary / source_name
        sbom_path = temporary / sbom_name
        checksums_path = temporary / checksums_name

        staged_ninfer = stage_binary(options.ninfer, temporary / "ninfer", "ninfer")
        staged_ninfer_serve = stage_binary(
            options.ninfer_serve, temporary / "ninfer-serve", "ninfer-serve"
        )
        staged_ninfer_bench = stage_binary(
            options.ninfer_bench, temporary / "ninfer_bench", "ninfer_bench"
        )
        if (
            parse_build_info(staged_ninfer, "ninfer") != identity
            or parse_build_info(staged_ninfer_serve, "ninfer-serve") != identity
        ):
            fail("binary build identity changed while staging release inputs")

        write_source_archive(
            source_path, source, options.release_head_sha, source_root, epoch
        )
        source_sha256 = hash_path(source_path)[0]

        binary_files = [
            ReleaseFile.from_path(
                f"{binary_root}/bin/ninfer", 0o755, staged_ninfer
            ),
            ReleaseFile.from_path(
                f"{binary_root}/bin/ninfer-serve",
                0o755,
                staged_ninfer_serve,
            ),
            ReleaseFile.from_path(
                f"{binary_root}/bin/ninfer_bench",
                0o755,
                staged_ninfer_bench,
            ),
        ]
        support_files = [
            ReleaseFile.from_bytes(
                f"{binary_root}/{destination}",
                mode,
                read_committed_file(source, options.release_head_sha, committed),
            )
            for committed, destination, mode in _SUPPORT_FILES
        ]
        identity_value = {
            "artifact_type": "ninfer_release_build_identity",
            "schema_version": 2,
            "release_version": options.release_version,
            "platform": options.platform,
            **identity,
            "source_dirty": False,
            "source_archive_sha256": source_sha256,
            "binaries": {
                "ninfer": binary_files[0].sha256,
                "ninfer-serve": binary_files[1].sha256,
                "ninfer_bench": binary_files[2].sha256,
            },
        }
        identity_bytes = (
            json.dumps(identity_value, sort_keys=True, separators=(",", ":")) + "\n"
        ).encode("utf-8")
        payload = [
            *binary_files,
            *support_files,
            ReleaseFile.from_bytes(
                f"{binary_root}/build-identity.json", 0o644, identity_bytes
            ),
        ]
        payload.append(
            ReleaseFile.from_bytes(
                f"{binary_root}/SHA256SUMS.txt",
                0o644,
                inner_checksums(binary_root, payload),
            )
        )
        spdx_bytes = build_spdx(
            root=binary_root,
            release_version=options.release_version,
            identity_bytes=identity_bytes,
            files=payload,
            epoch=epoch,
        )
        write_binary_archive(binary_path, binary_root, payload, epoch)
        sbom_path.write_bytes(spdx_bytes)

        binary_sha256 = hash_path(binary_path)[0]
        sbom_sha256 = hash_path(sbom_path)[0]
        checksums_path.write_text(
            f"{binary_sha256}  {binary_name}\n"
            f"{source_sha256}  {source_name}\n"
            f"{sbom_sha256}  {sbom_name}\n",
            encoding="ascii",
        )
        for temporary_path, destination in zip(
            [binary_path, source_path, sbom_path, checksums_path],
            destinations,
            strict=True,
        ):
            os.replace(temporary_path, destination)

    return {
        "artifact_type": "ninfer_local_release_receipt",
        "schema_version": 2,
        "release_version": options.release_version,
        "platform": options.platform,
        "upstream_base_sha": options.upstream_base_sha,
        "patch_stack_sha": options.release_head_sha,
        "source_dirty": False,
        "build_profile": options.build_profile,
        "source_date_epoch": epoch,
        "binary_asset": {"name": binary_name, "sha256": binary_sha256},
        "source_archive": {"name": source_name, "sha256": source_sha256},
        "sbom": {"name": sbom_name, "sha256": sbom_sha256, "format": "SPDX-2.3"},
        "checksums": checksums_name,
    }


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path.cwd())
    parser.add_argument("--ninfer", type=Path, required=True)
    parser.add_argument("--ninfer-serve", type=Path, required=True)
    parser.add_argument("--ninfer-bench", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--release-version", required=True)
    parser.add_argument("--platform", default="linux-x86_64-cuda12.8")
    parser.add_argument("--upstream-base-sha", required=True)
    parser.add_argument("--release-head-sha", required=True)
    parser.add_argument("--build-profile", required=True)
    parser.add_argument("--source-date-epoch", type=int)
    return parser


def main() -> None:
    args = create_parser().parse_args()
    try:
        value = package_release(
            ReleaseOptions(
                source=args.source,
                ninfer=args.ninfer,
                ninfer_serve=args.ninfer_serve,
                ninfer_bench=args.ninfer_bench,
                output_dir=args.output_dir,
                release_version=args.release_version,
                platform=args.platform,
                upstream_base_sha=args.upstream_base_sha,
                release_head_sha=args.release_head_sha,
                build_profile=args.build_profile,
                source_date_epoch=args.source_date_epoch,
            )
        )
    except ReleaseError as error:
        print(f"error: {error}", file=os.sys.stderr)
        raise SystemExit(2) from error
    print(json.dumps(value, sort_keys=True))


if __name__ == "__main__":
    main()
