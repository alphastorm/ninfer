#!/usr/bin/env python3
"""Create a deterministic local NInfer binary asset, checksum file, and SPDX SBOM."""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import gzip
import hashlib
import json
import io
import os
from pathlib import Path
import re
import shlex
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
        if isinstance(stderr, bytes):
            detail = stderr.decode("utf-8", errors="replace")
        else:
            detail = stderr or ""
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
    ancestor = subprocess.run(
        ["git", "merge-base", "--is-ancestor", upstream_sha, release_sha],
        cwd=source,
        text=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if ancestor.returncode != 0:
        fail("upstream base is unavailable or is not an ancestor of the release head")
    timestamp = str(
        run(["git", "show", "-s", "--format=%ct", release_sha], cwd=source).stdout
    ).strip()
    try:
        epoch = int(timestamp)
    except ValueError as error:
        raise ReleaseError("release commit timestamp is invalid") from error
    if epoch < 0:
        fail("release commit timestamp is invalid")
    return epoch


def read_committed_license(source: Path, release_sha: str) -> bytes:
    result = run(
        ["git", "show", f"{release_sha}:LICENSE"], cwd=source, text=False
    )
    assert isinstance(result.stdout, bytes)
    if not result.stdout:
        fail("the release commit contains an empty LICENSE")
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
    if not _NAME_RE.fullmatch(options.platform):
        fail("platform must contain only release-safe name characters")
    if not _NAME_RE.fullmatch(options.build_profile):
        fail("build profile must contain only release-safe name characters")
    source = options.source.resolve()
    commit_epoch = verify_source(
        source, options.upstream_base_sha, options.release_head_sha
    )
    identities = [
        parse_build_info(options.ninfer.resolve(), "ninfer"),
        parse_build_info(options.ninfer_serve.resolve(), "ninfer-serve"),
    ]
    if identities[0] != identities[1]:
        fail("ninfer and ninfer-serve carry different build identities")
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
                raise ReleaseError("SOURCE_DATE_EPOCH must be a nonnegative integer") from error
        else:
            epoch = commit_epoch
    if epoch < 0:
        fail("source date epoch must be a nonnegative integer")
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


def write_archive(path: Path, root: str, files: list[ReleaseFile], epoch: int) -> None:
    with path.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as compressed:
            with tarfile.open(
                fileobj=compressed, mode="w", format=tarfile.GNU_FORMAT
            ) as archive:
                add_tar_directory(archive, root, epoch)
                add_tar_directory(archive, f"{root}/bin", epoch)
                for item in sorted(files, key=lambda value: value.archive_name):
                    add_tar_file(archive, item, epoch)


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
        "documentNamespace": f"https://github.com/neroued/NInfer/releases/{release_version}/sbom-{namespace_id}",
        "creationInfo": {
            "created": created,
            "creators": ["Tool: ninfer-release-package"],
        },
        "packages": [
            {
                "name": "NInfer",
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


def package_release(options: ReleaseOptions) -> dict[str, object]:
    identity, epoch = validate_options(options)
    source = options.source.resolve()
    license_bytes = read_committed_license(source, options.release_head_sha)
    root = f"ninfer-qwen38-rtx5090-{options.release_version}-{options.platform}"
    binary_files = [
        ReleaseFile.from_path(
            f"{root}/bin/ninfer", 0o755, options.ninfer.resolve()
        ),
        ReleaseFile.from_path(
            f"{root}/bin/ninfer-serve", 0o755, options.ninfer_serve.resolve()
        ),
    ]
    identity_value = {
        "artifact_type": "ninfer_release_build_identity",
        "schema_version": 1,
        "release_version": options.release_version,
        "platform": options.platform,
        **identity,
        "source_dirty": False,
        "binaries": {
            "ninfer": binary_files[0].sha256,
            "ninfer-serve": binary_files[1].sha256,
        },
    }
    identity_bytes = (
        json.dumps(identity_value, sort_keys=True, separators=(",", ":")) + "\n"
    ).encode("utf-8")
    files = [
        *binary_files,
        ReleaseFile.from_bytes(f"{root}/LICENSE", 0o644, license_bytes),
        ReleaseFile.from_bytes(
            f"{root}/build-identity.json", 0o644, identity_bytes
        ),
    ]
    spdx_bytes = build_spdx(
        root=root,
        release_version=options.release_version,
        identity_bytes=identity_bytes,
        files=files,
        epoch=epoch,
    )

    output = options.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    archive_name = f"{root}.tar.gz"
    sbom_name = f"{root}.spdx.json"
    checksums_name = f"{root}.SHA256SUMS"
    destinations = [output / archive_name, output / sbom_name, output / checksums_name]
    existing = [path.name for path in destinations if path.exists()]
    if existing:
        fail("release outputs already exist: " + ", ".join(existing))

    with tempfile.TemporaryDirectory(prefix=".ninfer-release-", dir=output) as directory:
        temporary = Path(directory)
        archive_path = temporary / archive_name
        sbom_path = temporary / sbom_name
        checksums_path = temporary / checksums_name
        write_archive(archive_path, root, files, epoch)
        sbom_path.write_bytes(spdx_bytes)
        archive_sha256 = hash_path(archive_path)[0]
        sbom_sha256 = hash_path(sbom_path)[0]
        checksums_path.write_text(
            f"{archive_sha256}  {archive_name}\n{sbom_sha256}  {sbom_name}\n",
            encoding="ascii",
        )
        os.replace(archive_path, destinations[0])
        os.replace(sbom_path, destinations[1])
        os.replace(checksums_path, destinations[2])

    return {
        "artifact_type": "ninfer_local_release_receipt",
        "schema_version": 1,
        "release_version": options.release_version,
        "platform": options.platform,
        "upstream_base_sha": options.upstream_base_sha,
        "patch_stack_sha": options.release_head_sha,
        "source_dirty": False,
        "build_profile": options.build_profile,
        "source_date_epoch": epoch,
        "asset": {"name": archive_name, "sha256": archive_sha256},
        "sbom": {"name": sbom_name, "sha256": sbom_sha256, "format": "SPDX-2.3"},
        "checksums": checksums_name,
    }


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path.cwd())
    parser.add_argument("--ninfer", type=Path, required=True)
    parser.add_argument("--ninfer-serve", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--release-version", required=True)
    parser.add_argument("--platform", default="linux-x86_64-cuda13.1")
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
