#!/usr/bin/env python3
"""Build and operate one isolated NInfer Docker candidate with measured identity."""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time
import tempfile
from typing import Any, NoReturn
import urllib.error
import urllib.request


_LIFECYCLE_LABEL = "org.ninfer.lifecycle"
_LIFECYCLE_VALUE = "tools.lifecycle.ninfer_container"
_CHECKPOINT_SECCOMP_PROFILE = Path(__file__).with_name(
    "ninfer_io_uring_seccomp.json"
)
_CHECKPOINT_SECCOMP_PROFILE_SHA256 = (
    "3b7bf1e9fa71bfd8ed536c8aaaa2d40e4f133a0c853d226efd9b9e4966dd1506"
)
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_GIT_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
_PROFILE_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
_CONTAINER_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$")
_RESTART_POLICIES = {"no", "unless-stopped"}
_RESERVED_SERVER_ARGS = {
    "--host",
    "--port",
    "--api-key",
    "--model-id",
    "--binary-sha256",
    "--artifact-sha256",
    "--config-sha256",
    "--deployment-profile",
    "--request-log-jsonl",
    "--session-checkpoint-dir",
}
_CONFIG_KEYS = {
    "image",
    "bind_host",
    "container",
    "model_path",
    "model_id",
    "deployment_profile",
    "port",
    "request_log_dir",
    "checkpoint_dir",
    "api_key_file",
    "restart_policy",
    "args",
}
_REQUIRED_CONFIG_KEYS = _CONFIG_KEYS - {
    "bind_host",
    "api_key_file",
    "checkpoint_dir",
    "restart_policy",
    "args",
}


class LifecycleError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class RuntimeConfig:
    image: str
    container: str
    model_path: Path
    model_id: str
    deployment_profile: str
    port: int
    bind_host: str
    request_log_dir: Path
    checkpoint_dir: Path | None
    api_key_file: Path | None
    restart_policy: str
    args: tuple[str, ...]

    def canonical_identity(self) -> dict[str, Any]:
        return {
            "bind_host": self.bind_host,
            "api_key_configured": self.api_key_file is not None,
            "args": list(self.args),
            "deployment_profile": self.deployment_profile,
            "model_id": self.model_id,
            "port": self.port,
            "request_log_configured": True,
            "checkpoint_configured": self.checkpoint_dir is not None,
            "restart_policy": self.restart_policy,
        }


@dataclasses.dataclass(frozen=True)
class RuntimeIdentity:
    image_id: str
    binary_sha256: str
    model_artifact_sha256: str
    config_sha256: str


@dataclasses.dataclass(frozen=True)
class ExpectedIdentity:
    image_id: str | None
    binary_sha256: str | None
    model_artifact_sha256: str | None
    config_sha256: str | None


def fail(message: str) -> NoReturn:
    raise LifecycleError(message)


def require_string(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value or any(ord(char) < 32 for char in value):
        fail(f"{field} must be a non-empty string without control characters")
    return value


def require_absolute_path(value: Any, field: str) -> Path:
    path = Path(require_string(value, field))
    if not path.is_absolute():
        fail(f"{field} must be an absolute path")
    return path


def load_config(path: Path) -> RuntimeConfig:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise LifecycleError("failed to read lifecycle configuration JSON") from error
    if not isinstance(raw, dict):
        fail("lifecycle configuration must be a JSON object")
    unknown = sorted(set(raw) - _CONFIG_KEYS)
    missing = sorted(_REQUIRED_CONFIG_KEYS - set(raw))
    if unknown:
        fail(f"unknown lifecycle configuration field: {unknown[0]}")
    if missing:
        fail(f"missing lifecycle configuration field: {missing[0]}")

    image = require_string(raw["image"], "image")
    container = require_string(raw["container"], "container")
    if not _CONTAINER_RE.fullmatch(container):
        fail("container must match [A-Za-z0-9][A-Za-z0-9_.-]{0,127}")
    model_id = require_string(raw["model_id"], "model_id")
    if len(model_id) > 128:
        fail("model_id must contain at most 128 characters")
    profile = require_string(raw["deployment_profile"], "deployment_profile")
    if not _PROFILE_RE.fullmatch(profile):
        fail("deployment_profile must match [A-Za-z0-9._-]{1,64}")

    port = raw["port"]
    if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
        fail("port must be an integer in 1..65535")

    bind_host = raw.get("bind_host", "127.0.0.1")
    if bind_host not in {"127.0.0.1", "0.0.0.0"}:
        fail('bind_host must be either "127.0.0.1" or "0.0.0.0"')

    raw_args = raw.get("args", [])
    if not isinstance(raw_args, list):
        fail("args must be an array of server argument strings")
    args: list[str] = []
    for index, value in enumerate(raw_args):
        argument = require_string(value, f"args[{index}]")
        if argument in _RESERVED_SERVER_ARGS:
            fail(f"args must not override lifecycle-owned option {argument}")
        args.append(argument)

    checkpoint_value = raw.get("checkpoint_dir")
    checkpoint_dir = (
        None
        if checkpoint_value is None
        else require_absolute_path(checkpoint_value, "checkpoint_dir")
    )

    api_key_value = raw.get("api_key_file")
    api_key_file = (
        None
        if api_key_value is None
        else require_absolute_path(api_key_value, "api_key_file")
    )
    if api_key_file is None:
        fail("api_key_file is required for authenticated lifecycle status verification")
    if bind_host == "0.0.0.0" and api_key_file is None:
        fail("a non-loopback bind_host requires api_key_file")
    restart_policy = raw.get("restart_policy", "no")
    if restart_policy not in _RESTART_POLICIES:
        fail('restart_policy must be either "no" or "unless-stopped"')
    return RuntimeConfig(
        image=image,
        container=container,
        model_path=require_absolute_path(raw["model_path"], "model_path"),
        model_id=model_id,
        deployment_profile=profile,
        port=port,
        bind_host=bind_host,
        request_log_dir=require_absolute_path(
            raw["request_log_dir"], "request_log_dir"
        ),
        checkpoint_dir=checkpoint_dir,
        api_key_file=api_key_file,
        restart_policy=restart_policy,
        args=tuple(args),
    )


def run(
    command: list[str], *, capture: bool = True, check: bool = True
) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            command,
            check=check,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.PIPE if capture else None,
        )
    except FileNotFoundError as error:
        raise LifecycleError(
            f"required executable is unavailable: {command[0]}"
        ) from error
    except subprocess.CalledProcessError as error:
        detail = (error.stderr or error.stdout or "").strip().splitlines()
        suffix = f": {detail[-1]}" if detail else ""
        raise LifecycleError(
            f"{command[0]} command failed with exit {error.returncode}{suffix}"
        ) from error


def docker(
    arguments: list[str], *, capture: bool = True
) -> subprocess.CompletedProcess[str]:
    return run(["docker", *arguments], capture=capture)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise LifecycleError("failed to hash required deployment file") from error
    return digest.hexdigest()

def verify_clean_source(
    source: Path, upstream_base_sha: str, expected_patch_stack_sha: str
) -> str:
    head = run(["git", "-C", os.fspath(source), "rev-parse", "HEAD"]).stdout.strip()
    if not _GIT_SHA_RE.fullmatch(head):
        fail("source HEAD is not a full lowercase Git SHA")
    if head != expected_patch_stack_sha:
        fail("source HEAD differs from --expect-patch-stack-sha")
    status = run(
        ["git", "-C", os.fspath(source), "status", "--porcelain", "--untracked-files=all"]
    ).stdout
    if status:
        fail("source tree must be clean before a release identity can report source_dirty=false")
    ancestor = run(
        [
            "git",
            "-C",
            os.fspath(source),
            "merge-base",
            "--is-ancestor",
            upstream_base_sha,
            head,
        ],
        check=False,
    )
    if ancestor.returncode != 0:
        fail("upstream base is unavailable or is not an ancestor of the release head")
    return head


def materialize_source_archive(source: Path, head: str, destination: Path) -> str:
    archive_path = destination.parent / "source.tar"
    run(
        [
            "git",
            "-C",
            os.fspath(source),
            "archive",
            "--format=tar",
            f"--output={archive_path}",
            head,
        ]
    )
    archive_sha256 = sha256_file(archive_path)
    run(["git", "-C", os.fspath(destination), "init", "--quiet"])
    run(
        [
            "git",
            "-C",
            os.fspath(destination),
            "fetch",
            "--quiet",
            "--depth=1",
            "--no-tags",
            os.fspath(source),
            head,
        ]
    )
    run(
        [
            "git",
            "-C",
            os.fspath(destination),
            "checkout",
            "--quiet",
            "--detach",
            "FETCH_HEAD",
        ]
    )
    materialized_head = run(
        ["git", "-C", os.fspath(destination), "rev-parse", "HEAD"]
    ).stdout.strip()
    materialized_status = run(
        [
            "git",
            "-C",
            os.fspath(destination),
            "status",
            "--porcelain",
            "--untracked-files=all",
        ]
    ).stdout
    if materialized_head != head or materialized_status:
        fail("materialized Docker source is not the exact clean release commit")
    return archive_sha256



def canonical_config_sha256(config: RuntimeConfig) -> str:
    encoded = json.dumps(
        config.canonical_identity(), sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def image_id(image: str) -> str:
    value = docker(["image", "inspect", "--format", "{{.Id}}", image]).stdout.strip()
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", value):
        fail("Docker returned an invalid image identity")
    return value


def image_binary_sha256(image: str) -> str:
    output = docker(
        [
            "run",
            "--rm",
            "--entrypoint",
            "/usr/bin/sha256sum",
            image,
            "/usr/local/bin/ninfer-serve",
        ]
    ).stdout
    value = output.split(maxsplit=1)[0] if output.split() else ""
    if not _SHA256_RE.fullmatch(value):
        fail("failed to measure /usr/local/bin/ninfer-serve in the image")
    return value


def validate_secret_file(path: Path | None) -> None:
    if path is None:
        return
    try:
        value = path.read_bytes().rstrip(b"\r\n")
    except OSError as error:
        raise LifecycleError("failed to read api_key_file") from error
    if not value or b"\n" in value or b"\r" in value or b"\x00" in value:
        fail("api_key_file must contain one non-empty line")


def measure_runtime(config: RuntimeConfig) -> RuntimeIdentity:
    if not config.model_path.is_file():
        fail("model_path must name an existing regular file")
    validate_secret_file(config.api_key_file)
    return RuntimeIdentity(
        image_id=image_id(config.image),
        binary_sha256=image_binary_sha256(config.image),
        model_artifact_sha256=sha256_file(config.model_path),
        config_sha256=canonical_config_sha256(config),
    )


def parse_expected(args: argparse.Namespace) -> ExpectedIdentity:
    for field in ("binary_sha256", "model_artifact_sha256", "config_sha256"):
        value = getattr(args, f"expect_{field}")
        if value is not None and not _SHA256_RE.fullmatch(value):
            fail(
                f"--expect-{field.replace('_', '-')} must be 64 lowercase hexadecimal characters"
            )
    image = args.expect_image_id
    if image is not None and not re.fullmatch(r"sha256:[0-9a-f]{64}", image):
        fail(
            "--expect-image-id must be sha256 followed by 64 lowercase hexadecimal characters"
        )
    return ExpectedIdentity(
        image_id=image,
        binary_sha256=args.expect_binary_sha256,
        model_artifact_sha256=args.expect_model_artifact_sha256,
        config_sha256=args.expect_config_sha256,
    )


def verify_expected(identity: RuntimeIdentity, expected: ExpectedIdentity) -> None:
    for field in dataclasses.fields(ExpectedIdentity):
        wanted = getattr(expected, field.name)
        if wanted is not None and getattr(identity, field.name) != wanted:
            fail(f"measured {field.name} does not match its expected pin")


def inspect_container(name: str) -> dict[str, Any] | None:
    result = run(["docker", "container", "inspect", name], check=False)
    if result.returncode != 0:
        return None
    try:
        values = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise LifecycleError(
            "Docker returned invalid container inspection JSON"
        ) from error
    if (
        not isinstance(values, list)
        or len(values) != 1
        or not isinstance(values[0], dict)
    ):
        fail("Docker returned an invalid container inspection result")
    return values[0]


def require_managed_container(config: RuntimeConfig) -> dict[str, Any]:
    inspected = inspect_container(config.container)
    if inspected is None:
        fail("managed container does not exist")
    labels = inspected.get("Config", {}).get("Labels") or {}
    if labels.get(_LIFECYCLE_LABEL) != _LIFECYCLE_VALUE:
        fail("refusing to operate on a container not created by this lifecycle tool")
    return inspected


def require_gpu_idle() -> None:
    running = run(["docker", "ps", "--quiet"]).stdout.split()
    if running:
        try:
            inspected = json.loads(
                run(["docker", "container", "inspect", *running]).stdout
            )
        except json.JSONDecodeError as error:
            raise LifecycleError(
                "Docker returned invalid running-container JSON"
            ) from error
        conflicts: list[str] = []
        for value in inspected:
            requests = value.get("HostConfig", {}).get("DeviceRequests") or []
            if any(
                any(
                    "gpu" in capability_set
                    for capability_set in request.get("Capabilities", [])
                )
                for request in requests
            ):
                conflicts.append(str(value.get("Name", "unknown")).lstrip("/"))
        if conflicts:
            fail(
                "running GPU container prevents candidate start: "
                + ", ".join(sorted(conflicts))
            )

    nvidia_smi = shutil.which("nvidia-smi")
    if nvidia_smi is None:
        wsl_nvidia_smi = Path("/usr/lib/wsl/lib/nvidia-smi")
        if wsl_nvidia_smi.is_file() and os.access(wsl_nvidia_smi, os.X_OK):
            nvidia_smi = os.fspath(wsl_nvidia_smi)
        else:
            fail("nvidia-smi is unavailable")
    compute = run(
        [nvidia_smi, "--query-compute-apps=pid", "--format=csv,noheader"],
        check=False,
    )
    if compute.returncode != 0:
        fail("nvidia-smi failed while checking GPU exclusivity")
    if compute.stdout.strip():
        fail("active GPU compute process prevents candidate start")


def port_is_open(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as connection:
        connection.settimeout(0.2)
        return connection.connect_ex(("127.0.0.1", port)) == 0


def checkpoint_seccomp_profile(config: RuntimeConfig) -> Path | None:
    if config.checkpoint_dir is None:
        return None
    if not _CHECKPOINT_SECCOMP_PROFILE.is_file():
        fail("repository-owned io_uring seccomp profile is unavailable")
    if sha256_file(_CHECKPOINT_SECCOMP_PROFILE) != _CHECKPOINT_SECCOMP_PROFILE_SHA256:
        fail("repository-owned io_uring seccomp profile identity changed")
    return _CHECKPOINT_SECCOMP_PROFILE


def preflight(config: RuntimeConfig, expected: ExpectedIdentity) -> RuntimeIdentity:
    if shutil.which("docker") is None:
        fail("docker is unavailable")
    if inspect_container(config.container) is not None:
        fail("container already exists")
    if port_is_open(config.port):
        fail("configured loopback port is already accepting connections")
    checkpoint_seccomp_profile(config)
    identity = measure_runtime(config)
    verify_expected(identity, expected)
    return identity


def read_api_key(path: Path | None) -> str | None:
    if path is None:
        return None
    validate_secret_file(path)
    return path.read_bytes().rstrip(b"\r\n").decode("utf-8")


def request_json(url: str, api_key: str | None, *, timeout: float) -> dict[str, Any]:
    headers = {"Authorization": f"Bearer {api_key}"} if api_key is not None else {}
    request = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read()
    except (OSError, urllib.error.URLError) as error:
        raise LifecycleError("HTTP lifecycle probe failed") from error
    try:
        value = json.loads(body)
    except json.JSONDecodeError as error:
        raise LifecycleError("HTTP lifecycle probe returned invalid JSON") from error
    if not isinstance(value, dict):
        fail("HTTP lifecycle probe returned a non-object JSON value")
    return value


def wait_for_health(config: RuntimeConfig, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    url = f"http://127.0.0.1:{config.port}/health"
    while time.monotonic() < deadline:
        inspected = inspect_container(config.container)
        if inspected is None:
            fail("managed container disappeared before becoming healthy")
        state = inspected.get("State", {})
        if not state.get("Running", False):
            logs = run(
                ["docker", "logs", "--tail", "80", config.container],
                check=False,
            )
            lines = (logs.stderr or logs.stdout or "").strip().splitlines()
            detail = f": {lines[-1]}" if lines else ""
            fail("managed container exited before becoming healthy" + detail)
        try:
            if request_json(url, None, timeout=2.0) == {"status": "ok"}:
                return
        except LifecycleError:
            pass
        time.sleep(1.0)
    fail(f"server did not become healthy within {timeout:g} seconds")


def identity_from_labels(inspected: dict[str, Any]) -> RuntimeIdentity:
    labels = inspected.get("Config", {}).get("Labels") or {}
    image = inspected.get("Image")
    identity = RuntimeIdentity(
        image_id=image if isinstance(image, str) else "",
        binary_sha256=labels.get("org.ninfer.binary-sha256", ""),
        model_artifact_sha256=labels.get("org.ninfer.model-artifact-sha256", ""),
        config_sha256=labels.get("org.ninfer.config-sha256", ""),
    )
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", identity.image_id) or any(
        not _SHA256_RE.fullmatch(getattr(identity, field))
        for field in ("binary_sha256", "model_artifact_sha256", "config_sha256")
    ):
        fail("managed container has incomplete lifecycle identity labels")
    return identity


def fetch_status(config: RuntimeConfig) -> dict[str, Any]:
    return request_json(
        f"http://127.0.0.1:{config.port}/v1/ninfer/status",
        read_api_key(config.api_key_file),
        timeout=5.0,
    )


def verify_status(
    config: RuntimeConfig, identity: RuntimeIdentity, status: dict[str, Any]
) -> None:
    if (
        status.get("artifact_type") != "ninfer_server_status"
        or status.get("schema_version") != 1
        or status.get("status") != "ok"
    ):
        fail("status endpoint returned the wrong envelope")
    reported = status.get("identity")
    runtime = status.get("runtime")
    if not isinstance(reported, dict) or not isinstance(runtime, dict):
        fail("status endpoint omitted identity or runtime fields")
    expected = {
        "binary_sha256": identity.binary_sha256,
        "model_artifact_sha256": identity.model_artifact_sha256,
        "config_sha256": identity.config_sha256,
        "deployment_profile": config.deployment_profile,
    }
    for field, value in expected.items():
        if reported.get(field) != value:
            fail(
                f"status identity field {field} differs from the lifecycle measurement"
            )
    if runtime.get("public_model_id") != config.model_id:
        fail("status public_model_id differs from lifecycle configuration")


def receipt(
    event: str,
    config: RuntimeConfig,
    identity: RuntimeIdentity,
    **extra: Any,
) -> dict[str, Any]:
    value: dict[str, Any] = {
        "artifact_type": "ninfer_lifecycle_receipt",
        "schema_version": 1,
        "event": event,
        "container": config.container,
        "model_id": config.model_id,
        "deployment_profile": config.deployment_profile,
        "port": config.port,
        "bind_host": config.bind_host,
        "restart_policy": config.restart_policy,
        "image_id": identity.image_id,
        "binary_sha256": identity.binary_sha256,
        "model_artifact_sha256": identity.model_artifact_sha256,
        "config_sha256": identity.config_sha256,
    }
    value.update(extra)
    return value


def command_build(args: argparse.Namespace) -> None:
    source = args.source.resolve()
    if not (source / "Dockerfile").is_file():
        fail("source must contain the NInfer Dockerfile")
    if not _GIT_SHA_RE.fullmatch(args.upstream_base_sha):
        fail("--upstream-base-sha must be a full lowercase Git SHA")
    if not _GIT_SHA_RE.fullmatch(args.expect_patch_stack_sha):
        fail("--expect-patch-stack-sha must be a full lowercase Git SHA")
    if not _PROFILE_RE.fullmatch(args.build_profile):
        fail("--build-profile must match [A-Za-z0-9._-]{1,64}")
    head = verify_clean_source(
        source, args.upstream_base_sha, args.expect_patch_stack_sha
    )

    with tempfile.TemporaryDirectory(prefix="ninfer-build-context-") as directory:
        temporary = Path(directory)
        context = temporary / "source"
        context.mkdir()
        archive_sha256 = materialize_source_archive(source, head, context)
        docker(
            [
                "build",
                "--tag",
                args.image,
                "--build-arg",
                f"NINFER_BUILD_PROFILE={args.build_profile}",
                "--build-arg",
                f"NINFER_UPSTREAM_BASE_SHA={args.upstream_base_sha}",
                "--build-arg",
                f"NINFER_PATCH_STACK_SHA={head}",
                os.fspath(context),
            ],
            capture=False,
        )
    value = {
        "artifact_type": "ninfer_build_receipt",
        "schema_version": 2,
        "image": args.image,
        "image_id": image_id(args.image),
        "binary_sha256": image_binary_sha256(args.image),
        "upstream_base_sha": args.upstream_base_sha,
        "patch_stack_sha": head,
        "source_archive_sha256": archive_sha256,
        "source_dirty": False,
        "source_clean_verified": True,
        "build_profile": args.build_profile,
    }
    print(json.dumps(value, sort_keys=True))


def command_preflight(args: argparse.Namespace) -> None:
    config = load_config(args.config)
    identity = preflight(config, parse_expected(args))
    print(json.dumps(receipt("preflight", config, identity), sort_keys=True))


def command_start(args: argparse.Namespace) -> None:
    config = load_config(args.config)
    require_gpu_idle()
    identity = preflight(config, parse_expected(args))
    try:
        config.request_log_dir.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise LifecycleError("failed to create request_log_dir") from error
    if config.checkpoint_dir is not None:
        try:
            config.checkpoint_dir.mkdir(parents=True, exist_ok=True)
            if config.checkpoint_dir.is_symlink() or not config.checkpoint_dir.is_dir():
                fail("checkpoint_dir must be a non-symlink directory")
            config.checkpoint_dir.chmod(0o700)
        except OSError as error:
            raise LifecycleError("failed to prepare checkpoint_dir") from error
    timestamp = dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%S.%fZ")
    request_log_name = f"requests-{timestamp}.jsonl"
    labels = [
        (_LIFECYCLE_LABEL, _LIFECYCLE_VALUE),
        ("org.ninfer.binary-sha256", identity.binary_sha256),
        ("org.ninfer.model-artifact-sha256", identity.model_artifact_sha256),
        ("org.ninfer.config-sha256", identity.config_sha256),
        ("org.ninfer.deployment-profile", config.deployment_profile),
        ("org.ninfer.model-id", config.model_id),
    ]
    command = [
        "run",
        "--detach",
        "--name",
        config.container,
        "--gpus",
        "all",
        "--ipc",
        "host",
        "--restart",
        config.restart_policy,
        "--publish",
        f"{config.bind_host}:{config.port}:8080",
    ]
    for key, value in labels:
        command.extend(("--label", f"{key}={value}"))
    command.extend(
        (
            "--volume",
            f"{config.model_path}:/models/model.ninfer:ro",
            "--volume",
            f"{config.request_log_dir}:/logs",
        )
    )
    if config.checkpoint_dir is not None:
        command.extend(("--volume", f"{config.checkpoint_dir}:/checkpoints"))
        seccomp_profile = checkpoint_seccomp_profile(config)
        if seccomp_profile is None:
            fail("checkpoint seccomp profile was not resolved")
        command.extend(("--security-opt", f"seccomp={seccomp_profile}"))
    server_args = [
        "/models/model.ninfer",
        "--host",
        "0.0.0.0",
        "--port",
        "8080",
        "--model-id",
        config.model_id,
        "--binary-sha256",
        identity.binary_sha256,
        "--artifact-sha256",
        identity.model_artifact_sha256,
        "--config-sha256",
        identity.config_sha256,
        "--deployment-profile",
        config.deployment_profile,
        "--request-log-jsonl",
        f"/logs/{request_log_name}",
    ]
    if config.checkpoint_dir is not None:
        server_args.extend(("--session-checkpoint-dir", "/checkpoints"))
    server_args.extend(config.args)
    if config.api_key_file is None:
        command.extend((config.image, "/usr/local/bin/ninfer-serve", *server_args))
    else:
        command.extend(
            (
                "--volume",
                f"{config.api_key_file}:/run/secrets/ninfer_api_key:ro",
                config.image,
                "/bin/sh",
                "-c",
                'exec /usr/local/bin/ninfer-serve "$@" --api-key "$(cat /run/secrets/ninfer_api_key)"',
                "sh",
                *server_args,
            )
        )

    container_id = docker(command).stdout.strip()
    try:
        wait_for_health(config, args.timeout)
        status = fetch_status(config)
        verify_status(config, identity, status)
    except (Exception, KeyboardInterrupt):
        inspected = inspect_container(config.container)
        if inspected is not None:
            labels = inspected.get("Config", {}).get("Labels") or {}
            if labels.get(_LIFECYCLE_LABEL) == _LIFECYCLE_VALUE:
                run(["docker", "rm", "--force", config.container], check=False)
        raise
    print(
        json.dumps(
            receipt(
                "started",
                config,
                identity,
                container_id=container_id,
                request_log=request_log_name,
            ),
            sort_keys=True,
        )
    )


def command_health(args: argparse.Namespace) -> None:
    config = load_config(args.config)
    require_managed_container(config)
    value = request_json(
        f"http://127.0.0.1:{config.port}/health", None, timeout=args.timeout
    )
    if value != {"status": "ok"}:
        fail("health endpoint returned the wrong response")
    print(json.dumps(value, sort_keys=True))


def command_status(args: argparse.Namespace) -> None:
    config = load_config(args.config)
    inspected = require_managed_container(config)
    restart_policy = (
        inspected.get("HostConfig", {}).get("RestartPolicy", {}).get("Name")
    )
    if restart_policy != config.restart_policy:
        fail("managed container restart policy differs from lifecycle configuration")
    identity = identity_from_labels(inspected)
    verify_expected(identity, parse_expected(args))
    value = fetch_status(config)
    verify_status(config, identity, value)
    print(json.dumps(value, sort_keys=True))


def command_stop(args: argparse.Namespace) -> None:
    config = load_config(args.config)
    inspected = inspect_container(config.container)
    if inspected is None:
        print(
            json.dumps(
                {
                    "artifact_type": "ninfer_lifecycle_receipt",
                    "schema_version": 1,
                    "event": "already_stopped",
                    "container": config.container,
                },
                sort_keys=True,
            )
        )
        return
    require_managed_container(config)
    if args.timeout < 0:
        fail("--timeout must be non-negative")
    docker(["stop", "--time", str(args.timeout), config.container])
    docker(["rm", config.container])
    print(
        json.dumps(
            {
                "artifact_type": "ninfer_lifecycle_receipt",
                "schema_version": 1,
                "event": "stopped",
                "container": config.container,
            },
            sort_keys=True,
        )
    )


def add_expected_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--expect-image-id")
    parser.add_argument("--expect-binary-sha256")
    parser.add_argument("--expect-model-artifact-sha256")
    parser.add_argument("--expect-config-sha256")


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    build = subparsers.add_parser(
        "build", help="build an identity-stamped runtime image"
    )
    build.add_argument("--source", type=Path, default=Path.cwd())
    build.add_argument("--image", required=True)
    build.add_argument("--upstream-base-sha", required=True)
    build.add_argument("--expect-patch-stack-sha", required=True)
    build.add_argument("--build-profile", required=True)
    build.set_defaults(handler=command_build)

    preflight_parser = subparsers.add_parser(
        "preflight", help="measure files and reject identity or port conflicts"
    )
    preflight_parser.add_argument("--config", type=Path, required=True)
    add_expected_arguments(preflight_parser)
    preflight_parser.set_defaults(handler=command_preflight)

    start = subparsers.add_parser(
        "start", help="start and validate an isolated candidate"
    )
    start.add_argument("--config", type=Path, required=True)
    start.add_argument("--timeout", type=float, default=300.0)
    add_expected_arguments(start)
    start.set_defaults(handler=command_start)

    health = subparsers.add_parser(
        "health", help="validate the managed candidate health route"
    )
    health.add_argument("--config", type=Path, required=True)
    health.add_argument("--timeout", type=float, default=5.0)
    health.set_defaults(handler=command_health)

    status = subparsers.add_parser(
        "status", help="validate and print authenticated server status"
    )
    status.add_argument("--config", type=Path, required=True)
    add_expected_arguments(status)
    status.set_defaults(handler=command_status)

    stop = subparsers.add_parser("stop", help="stop only a lifecycle-owned candidate")
    stop.add_argument("--config", type=Path, required=True)
    stop.add_argument("--timeout", type=int, default=30)
    stop.set_defaults(handler=command_stop)
    return parser


def main() -> int:
    parser = create_parser()
    args = parser.parse_args()
    try:
        args.handler(args)
    except (LifecycleError, UnicodeDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
