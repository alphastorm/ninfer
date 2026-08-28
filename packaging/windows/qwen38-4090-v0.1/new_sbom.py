#!/usr/bin/env python3
"""Generate a deterministic SPDX 2.3 file inventory for one Windows release ZIP."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any, IO

GIT_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
UTC_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")


def digest(stream: IO[bytes]) -> tuple[str, str]:
    sha1 = hashlib.sha1(usedforsecurity=False)
    sha256 = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        sha1.update(chunk)
        sha256.update(chunk)
    return sha1.hexdigest(), sha256.hexdigest()


def sha256_file(path: Path) -> str:
    with path.open("rb") as stream:
        return digest(stream)[1]


def normalized_path(name: str) -> str:
    value = name.replace("\\", "/")
    path = PurePosixPath(value)
    if value.startswith("/") or not path.parts or any(part in {"", ".", ".."} for part in path.parts):
        raise ValueError(f"unsafe ZIP member path: {name!r}")
    return str(path)


def spdx_id(path: str) -> str:
    value = re.sub(r"[^A-Za-z0-9.-]+", "-", path).strip("-")
    if not value:
        raise ValueError(f"ZIP member has no SPDX-safe identity: {path!r}")
    return "SPDXRef-File-" + value


def build_document(
    archive_path: Path,
    *,
    release_tag: str,
    source_commit: str,
    created: str,
) -> dict[str, Any]:
    if not GIT_SHA_RE.fullmatch(source_commit):
        raise ValueError("source commit must be a lower-case 40-character Git SHA")
    if not UTC_RE.fullmatch(created):
        raise ValueError("created must be a UTC second timestamp ending in Z")
    archive_sha256 = sha256_file(archive_path)
    files: list[dict[str, object]] = []
    identities: set[str] = set()
    sha1_values: list[str] = []

    with zipfile.ZipFile(archive_path) as archive:
        for info in sorted(archive.infolist(), key=lambda item: normalized_path(item.filename)):
            if info.is_dir():
                continue
            mode = info.external_attr >> 16
            if mode and stat.S_ISLNK(mode):
                raise ValueError(f"release ZIP contains a symbolic link: {info.filename!r}")
            path = normalized_path(info.filename)
            identity = spdx_id(path)
            if identity in identities:
                raise ValueError(f"release ZIP has a duplicate normalized member: {path!r}")
            identities.add(identity)
            with archive.open(info) as stream:
                sha1, sha256 = digest(stream)
            sha1_values.append(sha1)
            files.append(
                {
                    "SPDXID": identity,
                    "checksums": [
                        {"algorithm": "SHA1", "checksumValue": sha1},
                        {"algorithm": "SHA256", "checksumValue": sha256},
                    ],
                    "copyrightText": "NOASSERTION",
                    "fileName": "./" + path,
                    "licenseConcluded": "NOASSERTION",
                }
            )

    verification = hashlib.sha1(
        "".join(sorted(sha1_values)).encode("ascii"), usedforsecurity=False
    ).hexdigest()
    package_id = "SPDXRef-Package-NInfer-RTX4090"
    relationships = [
        {
            "relatedSpdxElement": package_id,
            "relationshipType": "DESCRIBES",
            "spdxElementId": "SPDXRef-DOCUMENT",
        },
        *[
            {
                "relatedSpdxElement": item["SPDXID"],
                "relationshipType": "CONTAINS",
                "spdxElementId": package_id,
            }
            for item in files
        ],
    ]
    return {
        "SPDXID": "SPDXRef-DOCUMENT",
        "creationInfo": {
            "created": created,
            "creators": ["Tool: ninfer-release-sbom"],
        },
        "dataLicense": "CC0-1.0",
        "documentNamespace": (
            "https://github.com/alphastorm/ninfer/releases/"
            + release_tag
            + "/sbom-"
            + archive_sha256
        ),
        "files": files,
        "name": "ninfer-rtx4090-qwen38-v0.1.0-win-x64-sbom",
        "packages": [
            {
                "SPDXID": package_id,
                "copyrightText": "NOASSERTION",
                "downloadLocation": "NOASSERTION",
                "externalRefs": [
                    {
                        "referenceCategory": "OTHER",
                        "referenceLocator": (
                            "git+https://github.com/alphastorm/ninfer.git@" + source_commit
                        ),
                        "referenceType": "vcs",
                    }
                ],
                "filesAnalyzed": True,
                "hasFiles": [item["SPDXID"] for item in files],
                "licenseConcluded": "NOASSERTION",
                "licenseDeclared": "Apache-2.0",
                "name": "NInfer RTX 4090 Qwen3.8 runtime",
                "packageFileName": archive_path.name,
                "packageVerificationCode": {
                    "packageVerificationCodeValue": verification,
                },
                "versionInfo": "0.1.0",
            }
        ],
        "relationships": relationships,
        "spdxVersion": "SPDX-2.3",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--release-tag", required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--created", required=True)
    args = parser.parse_args()
    document = build_document(
        args.archive,
        release_tag=args.release_tag,
        source_commit=args.source_commit,
        created=args.created,
    )
    encoded = (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("utf-8")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_name(args.output.name + f".{os.getpid()}.tmp")
    try:
        temporary.write_bytes(encoded)
        os.replace(temporary, args.output)
    finally:
        temporary.unlink(missing_ok=True)
    print(f"wrote {args.output} ({len(document['files'])} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
