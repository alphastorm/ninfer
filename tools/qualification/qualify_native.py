#!/usr/bin/env python3
"""Idempotent two-host build, qualification, and restoration lane for a native Windows NInfer release.

One orchestrator serves every native lane. ``--lane`` selects the lane directory under
``packaging/windows/lanes``; its release specification supplies the identity (release id,
package names, GPU, architecture, power policy, task and state-root names) and its server
configuration the model id. The builder host compiles a neutral deterministic candidate and
packages it twice; the target host installs it beside the incumbent lineage, proves the
protocol, 128K retrieval, restart continuation, rollback (when a previous release exists),
state security, the OMP client run, and the bounded C1 benchmark, then restores its found state.
When the builder and target are different hosts the package travels between them directly as
ranged HTTP (``--transfer-tool``, a script with ``serve``/``fetch`` subcommands such as
omp-ninfer's ``scripts/hosts/pscp.py``); one host copies locally.
"""

from __future__ import annotations

import argparse
import base64
import dataclasses
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import re
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
from typing import Any, Callable
import urllib.error
import urllib.request

PHASES = (
    "preflight",
    "neutral_build",
    "private_path_scan",
    "package",
    "transfer_install",
    "protocol",
    "pressure_protocol",
    "context_128k",
    "restart",
    "rollback",
    "security",
    "omp",
    "benchmark_c1",
    "receipt",
    "restore",
)
EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()
EXPECTED_LONG = "ORCHID=493817; COLOR=COBALT"
LONG_PROMPT_TOKENS = 130048
CHECKPOINT_MARKER = "CHECKPOINT-NATIVE-731942"
# The second restart marker belongs to a session nothing ever published: only the shutdown flush
# a managed stop now reaches can carry it across the restart.
FLUSH_MARKER = "MANAGED-STOP-FLUSH-408137"
# Master has a shared Host pool, not independent Host State slots. Exercise a smaller byte
# budget equivalent to the predecessor's KV budget plus eight actual native StateImages.
# This is pressure on the shared pool, not a claim that only StateImages can consume it.
PRESSURE_STATE_IMAGES = 8
# App-local imports come from the selected CUDA/MSVC/vcpkg toolchains. DirectStorage's two
# redistributables are also staged explicitly because its core DLL is loaded dynamically.
DIRECTSTORAGE_DLLS = ("dstorage.dll", "dstoragecore.dll")

NATIVE_RUNTIME_DEPENDENCIES_PS = r"""
function Copy-NativeRuntimeDependencies(
    [string]$Build, [string]$Bin, [string]$CudaCompiler, [string]$MsvcRedist,
    [string]$Dumpbin, [string[]]$DirectStorageDlls
) {
    $vcpkgBin=Join-Path $Build 'vcpkg_installed\x64-windows\bin'
    $appsBin=Join-Path $Build 'apps'
    $cudaBin=Split-Path -Parent $CudaCompiler
    if([string]::IsNullOrWhiteSpace($MsvcRedist)){throw 'selected MSVC redistributable root is unavailable'}
    $crtBin=Join-Path $MsvcRedist 'x64\Microsoft.VC143.CRT'
    if(-not(Test-Path -LiteralPath $crtBin -PathType Container)){throw 'selected MSVC x64 CRT directory is unavailable'}
    $runtimeRoots=@($crtBin,$cudaBin,(Join-Path $cudaBin 'x64'),$vcpkgBin,$appsBin)
    $pending=[Collections.Generic.Queue[string]]::new()
    foreach($exe in @('ninfer.exe','ninfer-serve.exe','ninfer_bench.exe')){$pending.Enqueue((Join-Path $Bin $exe))}
    $copied=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($name in $DirectStorageDlls){
        Copy-Item -LiteralPath (Join-Path $appsBin $name) -Destination (Join-Path $Bin $name)
        [void]$copied.Add($name);$pending.Enqueue((Join-Path $Bin $name))
    }
    while($pending.Count -gt 0){
        $file=$pending.Dequeue()
        $imports=@(&$Dumpbin /nologo /dependents $file|Where-Object{$_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$'}|ForEach-Object{$Matches[1]})
        if($LASTEXITCODE -ne 0){throw 'runtime DLL import inspection failed'}
        foreach($import in $imports){
            if($copied.Contains($import)){continue}
            $candidate=$null
            foreach($runtimeRoot in $runtimeRoots){
                $path=Join-Path $runtimeRoot $import
                if(Test-Path -LiteralPath $path -PathType Leaf){$candidate=$path;break}
            }
            if($null -eq $candidate){
                if($import -match '^(cudart64_|vcruntime|msvcp|concrt)'){throw ('selected app-local runtime DLL is unavailable: '+$import)}
                continue
            }
            [void]$copied.Add($import)
            Copy-Item -LiteralPath $candidate -Destination (Join-Path $Bin $import)
            $pending.Enqueue((Join-Path $Bin $import))
        }
    }
    return @($copied|Sort-Object)
}
"""


@dataclasses.dataclass(frozen=True)
class Lane:
    """The identity a lane's release specification and server configuration declare."""

    name: str
    spec: dict[str, Any]
    config: dict[str, Any]

    @classmethod
    def load(cls, source_root: Path, name: str, head: str) -> "Lane":
        lane_dir = f"packaging/windows/lanes/{name}"
        try:
            spec = json.loads(run(["git", "show", f"{head}:{lane_dir}/release-spec.json"], cwd=source_root).stdout)
            config = json.loads(run(["git", "show", f"{head}:{lane_dir}/server-config.json"], cwd=source_root).stdout)
        except LaneError as error:
            raise LaneError(f"lane {name} is not committed at {head[:8]}: {error}") from error
        if spec.get("artifact_type") != "ninfer_windows_release_spec" or spec.get("schema_version") != 4:
            raise LaneError("release specification is not a schema 4 native lane spec")
        if config.get("schema_version") != 4 or spec.get("lane") != name or config.get("release_id") != spec.get("release_id"):
            raise LaneError("lane specification and server configuration disagree")
        declared_mib = str(config.get("context_cache", {}).get("host_context_mib", ""))
        if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)?", declared_mib):
            raise LaneError("lane must declare an exact positive shared Host budget")
        expected_mib = host_context_mib(spec)
        if Decimal(declared_mib) != Decimal(expected_mib):
            raise LaneError("lane shared Host budget differs from its native layout translation")
        return cls(name, spec, config)

    @property
    def release_id(self) -> str:
        return self.spec["release_id"]

    @property
    def release_version(self) -> str:
        return "v" + self.spec["release_version"]

    @property
    def asset_stem(self) -> str:
        return f"{self.spec['product_prefix']}-{self.release_version}-{self.spec['platform']}"

    @property
    def package_name(self) -> str:
        return self.asset_stem + ".tar.gz"

    @property
    def source_name(self) -> str:
        return f"{self.spec['product_prefix']}-{self.release_version}-source.tar.gz"

    @property
    def spdx_name(self) -> str:
        return self.asset_stem + ".spdx.json"

    @property
    def cuda_architecture(self) -> str:
        return self.spec["gpu"]["cmake_cuda_architecture"]

    @property
    def build_profile(self) -> str:
        return self.spec["build_profile"]

    @property
    def upstream_sha(self) -> str:
        return self.spec["source"]["upstream_base_sha"]

    @property
    def lineage_sha(self) -> str:
        return self.spec["source"]["lineage_base_sha"]

    @property
    def model_sha256(self) -> str:
        return self.spec["model"]["sha256"]

    @property
    def model_id(self) -> str:
        return self.config["model_id"]

    @property
    def task_name(self) -> str:
        return self.spec["lifecycle"]["task_name"]

    @property
    def state_root(self) -> str:
        return "C:/ProgramData/NInfer/" + self.spec["lifecycle"]["state_root_name"]

    @property
    def owner_power_limit_w(self) -> int:
        return int(self.spec["lifecycle"]["gpu_owner_controller_protocol"]["owner_power_limit_w"])

    @property
    def qualified_power_limit_w(self) -> int:
        value = self.spec["lifecycle"]["gpu_owner_controller_protocol"].get("qualified_power_limit_w")
        return 0 if value is None else int(value)

    @property
    def managed_power_envelope_w(self) -> float | None:
        value = self.spec["lifecycle"]["gpu_owner_controller_protocol"].get("managed_power_envelope_w")
        return None if value is None else float(value)
CREDENTIAL_PATTERNS = (
    re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    re.compile(rb"\bAKIA[A-Z0-9]{16}\b"),
    re.compile(rb"\bgh[pousr]_[A-Za-z0-9]{36,}\b"),
)


class LaneError(RuntimeError):
    pass


def host_context_mib(spec: dict[str, Any], *, state_images: int | None = None) -> str:
    translation = spec.get("host_context_translation")
    fields = ("predecessor_host_kv_mib", "predecessor_host_state_slots", "native_state_image_bytes",
              "host_context_bytes")
    if not isinstance(translation, dict) or any(
        type(translation.get(field)) is not int or translation[field] <= 0 for field in fields
    ):
        raise LaneError("lane lacks its layout-aware shared Host budget translation")
    full_bytes = ((translation["predecessor_host_kv_mib"] << 20) +
                  translation["predecessor_host_state_slots"] * translation["native_state_image_bytes"])
    if full_bytes != translation["host_context_bytes"]:
        raise LaneError("lane shared Host byte budget translation is inconsistent")
    count = translation["predecessor_host_state_slots"] if state_images is None else state_images
    total_bytes = (translation["predecessor_host_kv_mib"] << 20) + count * translation["native_state_image_bytes"]
    return format(Decimal(total_bytes) / Decimal(1 << 20), "f")


def require_native_save_receipt(receipt: dict[str, Any]) -> None:
    expected = {
        "schema": "ninfer-durable-operation/1", "operation": "save", "state": "exported",
        "reason": "none", "committed": True, "origin": "authenticated",
        "dir_sync": "ntfs-write-through",
    }
    if any(receipt.get(key) != value for key, value in expected.items()) or receipt.get("committed") is not True:
        raise LaneError("explicit session checkpoint lacks an authenticated native publication acknowledgement")
    for field in ("frontier_tokens", "restored_tokens", "payload_bytes", "points", "states", "pages"):
        value = receipt.get(field)
        if type(value) is not int or value < (0 if field == "pages" else 1):
            raise LaneError(f"explicit session checkpoint omitted its real {field} count")
    if receipt["restored_tokens"] > receipt["frontier_tokens"]:
        raise LaneError("explicit session checkpoint has invalid restored coverage")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    timeout: int = 600,
    check: bool = True,
    text: bool = True,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        cwd=cwd,
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=text,
        timeout=timeout,
    )
    if check and result.returncode:
        stdout = result.stdout[-4000:] if isinstance(result.stdout, str) else ""
        stderr = result.stderr[-4000:] if isinstance(result.stderr, str) else ""
        raise LaneError(
            f"command failed ({result.returncode}): {command[0]}\n{stdout}\n{stderr}"
        )
    return result


def ps_encoded(script: str) -> str:
    return base64.b64encode(script.encode("utf-16le")).decode("ascii")


def remote_ps(host: str, script: str, *, timeout: int = 600) -> str:
    encoded = ps_encoded(script)
    if len(encoded) > 7000:
        digest = hashlib.sha256(script.encode("utf-8")).hexdigest()[:16]
        remote_path = f"C:/Windows/Temp/ninfer-qualification-{digest}.ps1"
        local_path: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", suffix=".ps1", delete=False
            ) as stream:
                stream.write(script)
                local_path = Path(stream.name)
            scp(str(local_path), remote_spec(host, remote_path))
            result = run(
                [
                    "ssh", "-T", host, "powershell", "-NoProfile", "-NonInteractive",
                    "-ExecutionPolicy", "Bypass", "-OutputFormat", "Text", "-File", remote_path,
                ],
                timeout=timeout,
            )
            return result.stdout
        finally:
            if local_path is not None:
                local_path.unlink(missing_ok=True)
            run(
                [
                    "ssh", "-T", host, "powershell", "-NoProfile", "-NonInteractive",
                    "-Command", f"Remove-Item -LiteralPath '{remote_path}' -Force -ErrorAction SilentlyContinue",
                ],
                timeout=60,
                check=False,
            )
    result = run(
        [
            "ssh",
            "-T",
            host,
            "powershell",
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-OutputFormat",
            "Text",
            "-EncodedCommand",
            encoded,
        ],
        timeout=timeout,
    )
    return result.stdout


def compact_json_from_output(output: str) -> dict[str, Any]:
    for line in reversed(output.splitlines()):
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            value = json.loads(line)
            if isinstance(value, dict):
                return value
    raise LaneError("command emitted no compact JSON receipt")


def ps_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


GPU_UUID = re.compile(r"GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}")

TARGET_CONTROLLER_GUARD = r"""
function Test-ScopedControllerOrQuiescent([string]$Root) {
  $state=Get-Content (Join-Path $Root 'state.json') -Raw|ConvertFrom-Json
  $owner=$state.gpu_owner
  if((Get-FileHash ([string]$owner.controller_path) -Algorithm SHA256).Hash.ToLowerInvariant() -cne [string]$owner.controller_sha256){throw 'installed owner hash mismatch'}
  if((Get-Command -Name ([string]$owner.controller_path)).Parameters.ContainsKey('GpuUuid')){return $true}
  $task=Get-ScheduledTask -TaskName ([string]$state.task_name) -ErrorAction SilentlyContinue
  if(($null -ne $task -and [string]$task.State -ceq 'Running') -or
     (Test-Path (Join-Path $Root 'runtime.json')) -or
     (Test-Path (Join-Path $Root 'gpu-owner-lease.json')) -or
     (Test-Path (Join-Path $Root 'gpu-owner-state/lease.json'))){throw 'legacy unscoped installation must be safely quiescent before qualification'}
  return $false
}
"""


def gpu_uuid_argument(value: str) -> str:
    if not GPU_UUID.fullmatch(value):
        raise argparse.ArgumentTypeError("expected a full GPU UUID (GPU-xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)")
    return value


def select_qualified_gpu(rows: list[str], spec: dict[str, Any], requested: str | None) -> dict[str, Any]:
    """Match the installer's lane name/compute-capability rule, not the packaged ordinal."""
    if requested is not None:
        gpu_uuid_argument(requested)
    qualified = []
    for row in rows:
        parts = [part.strip() for part in row.split(",")]
        if (len(parts) != 6 or not parts[0].isdigit() or not GPU_UUID.fullmatch(parts[1])
                or not re.fullmatch(r"[0-9a-fA-F]{8}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]", parts[3])):
            raise LaneError("target GPU selection returned an invalid row")
        if requested and parts[1].lower() != requested.lower():
            continue
        if parts[2] == spec["gpu"]["name"] and parts[4] == spec["gpu"]["compute_capability"]:
            qualified.append(dict(index=int(parts[0]), uuid=parts[1], name=parts[2], pci_bus_id=parts[3].upper()))
    if not qualified:
        raise LaneError("no qualified target GPU (or --gpu-uuid is not qualified for this lane)")
    if len(qualified) != 1:
        raise LaneError("more than one qualified target GPU; select one with --gpu-uuid")
    return qualified[0]


def remote_spec(host: str, path: str) -> str:
    return f"{host}:{path}"


def scp(source: str, destination: str, *, through_local: bool = False, timeout: int = 900) -> None:
    command = ["scp", "-q"]
    if through_local:
        command.append("-3")
    command.extend([source, destination])
    run(command, timeout=timeout)


def request_json(
    base_url: str,
    key: str,
    method: str,
    path: str,
    body: dict[str, Any] | None = None,
    timeout: int = 1800,
) -> dict[str, Any]:
    data = None if body is None else json.dumps(body).encode("utf-8")
    request = urllib.request.Request(
        base_url + path,
        data=data,
        method=method,
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        value = json.load(response)
    if not isinstance(value, dict):
        raise LaneError(f"{path} returned a non-object")
    return value


def read_key(path: Path) -> str:
    value = path.read_bytes().rstrip(b"\r\n")
    if not value or b"\r" in value or b"\n" in value or b"\0" in value:
        raise LaneError("API-key file must contain exactly one non-empty line")
    return value.decode("utf-8")


def response_text(value: dict[str, Any]) -> str:
    output = value.get("output")
    if not isinstance(output, list):
        raise LaneError("Responses output is missing")
    chunks: list[str] = []
    for item in output:
        if not isinstance(item, dict):
            continue
        content = item.get("content")
        if not isinstance(content, list):
            continue
        for part in content:
            if isinstance(part, dict) and isinstance(part.get("text"), str):
                chunks.append(part["text"])
    return "".join(chunks)


def digest(label: str) -> str:
    return hashlib.sha256(f"ninfer-native-orchestrated-v1/{label}".encode()).hexdigest()


def marker_findings(
    data: bytes,
    dynamic_home: bytes | None = None,
    private_markers: tuple[bytes, ...] = (),
) -> int:
    lowered = data.lower()
    markers: list[bytes] = list(private_markers)
    if dynamic_home:
        markers.append(dynamic_home.lower().rstrip(b"\\/") + b"\\")
    return sum(lowered.count(marker) for marker in markers) + sum(
        len(pattern.findall(data)) for pattern in CREDENTIAL_PATTERNS
    )


def scan_directory(path: Path) -> dict[str, Any]:
    dynamic_home = os.environ.get("USERPROFILE", "").encode()
    files = 0
    size = 0
    findings = 0
    for candidate in sorted(path.rglob("*")):
        if not candidate.is_file():
            continue
        files += 1
        size += candidate.stat().st_size
        findings += marker_findings(candidate.read_bytes(), dynamic_home)
    return {"files": files, "bytes": size, "findings": findings}


def scan_archive(path: Path, source_archive: bool = False) -> dict[str, Any]:
    dynamic_home = os.environ.get("USERPROFILE", "").encode()
    files = 0
    size = 0
    findings = 0
    generic_paths = 0
    generic = re.compile(
        rb"(?:[A-Za-z]:\\Users\\(?:<[^>]+>|USER|username)|/Users/(?:<[^>]+>|USER|username))",
        re.IGNORECASE,
    )
    with tarfile.open(path, "r:*") as archive:
        for member in archive:
            if not member.isfile():
                continue
            stream = archive.extractfile(member)
            if stream is None:
                continue
            data = stream.read()
            files += 1
            size += len(data)
            findings += marker_findings(data, dynamic_home)
            if source_archive:
                generic_paths += len(generic.findall(data))
    return {
        "files": files,
        "bytes": size,
        "findings": findings,
        "generic_home_references": generic_paths,
    }


def validate_scratch_configuration(config: Config, spec: dict[str, Any]) -> None:
    root = PureWindowsPath(config.scratch_state_root)
    if (not root.is_absolute() or ".." in root.parts or
            root.name.casefold() != spec["lifecycle"]["state_root_name"].casefold()):
        raise LaneError("scratch state root must be absolute with the lane's state-root leaf")
    protected = PureWindowsPath("C:/ProgramData/NInfer")
    if root == protected or protected in root.parents:
        raise LaneError("scratch qualification refuses the canonical product state tree")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]{0,25}", config.scratch_task_suffix):
        raise LaneError("invalid scratch task suffix (reserve six characters for the clean-install task)")
    if (config.scratch_port == config.pressure_port or
            any(type(port) is not int or not 18200 <= port <= 18219
                for port in (config.scratch_port, config.pressure_port))):
        raise LaneError("scratch and pressure ports must be distinct in the approved 18200-18219 range")
    if not config.api_key_file:
        raise LaneError("scratch qualification requires an explicit protected operator API-key file")
    if bool(config.qualified_runtime_bin) != bool(config.qualified_build_receipt):
        raise LaneError("qualified runtime-bin and build-receipt inputs must be supplied together")


def require_port_only_equivalence(
    public_config: dict[str, Any], scratch_config: dict[str, Any], scratch_port: int,
) -> None:
    restored = json.loads(json.dumps(scratch_config))
    if public_config.get("listen", {}).get("port") != 18082 or restored.get("listen", {}).get("port") != scratch_port:
        raise LaneError("public and scratch package ports do not match their declared bindings")
    restored["listen"]["port"] = 18082
    if restored != public_config:
        raise LaneError("scratch configuration differs from the public package beyond /listen/port")


def stage_lane_qualification(state: dict[str, Any], summary: dict[str, Any], lane: Lane) -> dict[str, Any]:
    """Adapt completed receipts, not asserted flags, to the product's native lane shape."""
    if (state.get("status") != "passed" or summary.get("status") != "passed" or
            summary.get("restoration", {}).get("status") != "passed"):
        raise LaneError("cannot stage an incomplete or unrestored native qualification")
    phases = state["phases"]
    if any(phases.get(name, {}).get("status") != "passed" for name in PHASES):
        raise LaneError("cannot stage a lane with a missing or non-passing phase")
    receipts = {name: phases[name]["receipt"] for name in PHASES}
    if any(
        receipt.get("status") not in (None, "passed") and
        not (name == "rollback" and receipt.get("status") == "not_applicable")
        for name, receipt in receipts.items()
    ):
        raise LaneError("cannot stage a lane containing a non-passing inner receipt")
    package = receipts["package"]
    scratch = package["scratch_package"]
    equivalence = package["scratch_equivalence"]
    if (equivalence.get("status") != "passed" or equivalence.get("public_package_executed") is not False or
            equivalence.get("configuration_delta", {}).get("path") != "/listen/port"):
        raise LaneError("native lane lacks explicit public/scratch package equivalence")
    for key, expected in {
        "public_package_sha256": package["package"]["sha256"],
        "scratch_package_sha256": scratch["package"]["sha256"],
        "public_configuration_sha256": package["config_sha256"],
        "scratch_configuration_sha256": scratch["config_sha256"],
    }.items():
        if equivalence.get(key) != expected:
            raise LaneError(f"scratch equivalence binding mismatch: {key}")
    if (package["binaries"] != scratch["binaries"] or
            package["support_assets"] != scratch["support_assets"] or
            receipts["transfer_install"]["package_sha256"] != scratch["package"]["sha256"]):
        raise LaneError("installed rehearsal is not the byte-equivalent scratch package")
    protocol = receipts["protocol"]
    identity = dict(protocol["identity"])
    expected_identity = {
        "patch_stack_sha": state["source_commit"], "source_dirty": False,
        "upstream_base_sha": package["upstream_base_sha"], "build_profile": package["build_profile"],
        "deployment_profile": lane.spec["deployment_profile"], "model_id": lane.model_id,
        "binary_sha256": package["binaries"]["server_sha256"],
        "model_artifact_sha256": package["model_sha256"], "config_sha256": scratch["config_sha256"],
    }
    if any(identity.get(key) != value for key, value in expected_identity.items()):
        raise LaneError("actual status identity does not match the qualified scratch package")
    startup = protocol["startup_attestation"]
    if (startup.get("request_log_schema") != 30 or startup.get("gpu_uuid_verified") is not True or
            startup.get("gpu_name") != lane.spec["gpu"]["name"] or type(startup.get("gpu_index")) is not int):
        raise LaneError("lane lacks the controller's completed launch-scoped GPU attestation")
    for name in ("protocol", "pressure_protocol"):
        observed = receipts[name]["protocol"]
        checks = observed.get("checks")
        if (observed.get("status") != "passed" or not isinstance(checks, dict) or
                not checks or any(value is not True for value in checks.values())):
            raise LaneError(f"{name} lacks passing real protocol checks")
    context = receipts["context_128k"]
    restart = receipts["restart"]
    stop = restart["managed_stop"]
    if context["prompt_tokens"] != LONG_PROMPT_TOKENS or context["exact_output"] != EXPECTED_LONG:
        raise LaneError("lane does not prove the advertised context oracle")
    if (restart["old_pid"] == restart["new_pid"] or stop["mode"] != "stop-event" or
            stop["outcome"] != "graceful" or stop["forced"] or restart["cached_input_tokens"] <= 0 or
            restart["managed_stop_flush"]["exact_output"] != FLUSH_MARKER):
        raise LaneError("lane does not prove process replacement and managed final-save continuation")
    security = receipts["security"]["security"]
    omp = receipts["omp"]
    benchmark = dict(receipts["benchmark_c1"])
    if (security.get("status") != "passed" or security.get("root_dacl_protected") is not True or
            security.get("low_privilege_effective_read_denials", 0) <= 0 or
            omp.get("exact_final_answer") is not True or omp.get("typed_tool_name") != "read" or
            omp.get("tool_results", 0) < 1 or not omp.get("client_version") or
            benchmark.get("completion_tokens") != 1024 or benchmark.get("decode_tokens_per_second", 0) <= 0):
        raise LaneError("lane lacks passing security, client, or bounded benchmark evidence")
    benchmark.pop("gpu_uuid", None)
    persistence = {key: value for key, value in restart.items() if key not in {"old_pid", "new_pid"}}
    persistence["process_replaced"] = True
    configuration = lane.config
    identity.update({
        "source_commit": identity["patch_stack_sha"], "server_binary_sha256": identity["binary_sha256"],
        "lineage_base_sha": package["lineage_base_sha"], "gpu_index": startup["gpu_index"],
        "gpu_name": startup["gpu_name"], "qualified_configuration_sha256": identity["config_sha256"],
        "config_sha256": package["config_sha256"], "configuration_sha256": package["config_sha256"],
        "configuration_binding": "public-package-with-port-only-scratch-equivalence",
    })
    gate_names = {
        "protocol": "protocol", "protocol_under_pool_pressure": "pressure_protocol",
        "long_session": "context_128k", "persistence": "restart", "rollback": "rollback",
        "state_security": "security", "golden_equivalent": "omp", "benchmark_c1": "benchmark_c1",
    }
    return {
        "artifact_type": "ninfer_windows_release_qualification", "schema_version": 1, "status": "passed",
        "qualified_utc": summary["qualified_utc"], "release_id": lane.release_id, "profile": "MTP3",
        "identity": identity,
        "configuration": {
            "public_model_id": lane.model_id,
            **{key: configuration["engine"][key] for key in (
                "max_context", "kv_capacity", "kv_dtype", "prefill_chunk", "admission_headroom_mib",
            )},
            "concurrency": configuration["engine"]["max_concurrency"],
            "gpu_keep_warm_ms": configuration["engine"].get("gpu_keep_warm_ms", 0),
            "device_state_slots": configuration["context_cache"]["device_state_slots"],
            "host_context_mib": configuration["context_cache"]["host_context_mib"],
            "host_context_bytes": lane.spec["host_context_translation"]["host_context_bytes"],
            "speculative_backend": configuration["speculative"]["backend"],
            "speculative_draft_tokens": configuration["speculative"]["draft_tokens"],
            "checkpoint_require_origin_auth": configuration["session_checkpoint"]["require_origin_auth"],
            "managed_stop": lane.spec["lifecycle"]["managed_stop"],
            "graceful_stop_timeout_seconds": lane.spec["lifecycle"]["graceful_stop_timeout_seconds"],
        },
        "protocol": protocol["protocol"],
        "protocol_under_pool_pressure": {
            "artifact_type": receipts["pressure_protocol"]["protocol"]["artifact_type"],
            "status": receipts["pressure_protocol"]["protocol"]["status"],
            "host_context_mib": receipts["pressure_protocol"]["host_context_mib"],
            "device_state_slots": receipts["pressure_protocol"]["device_state_slots"],
            "checks_passed": len(receipts["pressure_protocol"]["protocol"]["checks"]),
            "checks": receipts["pressure_protocol"]["protocol"]["checks"],
        },
        "long_session": context, "persistence": persistence, "rollback": receipts["rollback"],
        "state_security": security,
        "golden_equivalent": {
            "artifact_type": "ninfer_omp_golden_equivalent_receipt", "schema_version": 1,
            "status": omp["status"], "omp": {key: omp[key] for key in (
                "client_version", "events", "typed_tool_name", "tool_results",
            )},
            "oracles": {"exact_visible_final_answer": "passed", "typed_tool_invocation": "passed",
                        "tool_result_continuation": "passed"},
            "raw_transcript_included": False,
        },
        "benchmark_c1": benchmark,
        "deterministic_gates": {
            output: receipts[source].get("status", phases[source]["status"])
            for output, source in gate_names.items()
        },
        "restoration": summary["restoration"],
        "scratch_equivalence": {**equivalence, "startup_attestation": startup, "observed_identity": protocol["identity"]},
        "orchestration_evidence": {
            name: {"status": phase["status"], "receipt_sha256": phase["receipt_sha256"]}
            for name, phase in phases.items()
        },
        "limitations": [
            "Public packages retain port 18082 and were not executed. The isolated installed rehearsals differ only in listen.port.",
            "C1 and the recorded shared-Host pressure budget are the qualified profiles; no multi-GPU inference or smaller-host claim.",
            "Restart proves the two recorded short sessions, not loss-free shutdown of every live session.",
        ],
        "beta_qualified": True,
        "package": {
            **package["package"], **package["support_assets"],
            "sbom_sha256": package["sbom"]["sha256"],
            "dependency_sbom_sha256": package["dependency_sbom"]["sha256"],
            "outer_checksums_sha256": package["outer_sha256"],
        },
    }


@dataclasses.dataclass
class Config:
    source_root: Path
    state_dir: Path
    lane: str
    builder: str
    target: str
    builder_vcpkg: str
    builder_cuda_compiler: str
    builder_cxx_compiler: str
    model_path: str
    long_fixture: Path
    omp_root: str
    transfer_tool: Path | None
    incumbent_state_root: str | None
    resume: bool
    dry_run: bool
    through_phase: str | None
    candidate_source: str | None
    upgrade_orchestrator_state: bool
    dependency_sbom_tool: str
    vcpkg_resolved: str
    cuda_inventory: str
    third_party_inventory: str
    runtime_inventory: str
    scratch_state_root: str
    scratch_task_suffix: str
    scratch_port: int
    pressure_port: int
    api_key_file: str
    qualified_runtime_bin: str | None
    qualified_build_receipt: str | None
    gpu_uuid: str | None = None


class Orchestrator:
    def __init__(self, config: Config) -> None:
        self.config = config
        self.tool_head = run(["git", "rev-parse", "HEAD"], cwd=config.source_root).stdout.strip()
        self.head = config.candidate_source or self.tool_head
        run(["git", "cat-file", "-e", f"{self.head}^{{commit}}"], cwd=config.source_root)
        self.head8 = self.head[:8]
        self.lane = Lane.load(config.source_root, config.lane, self.head)
        validate_scratch_configuration(config, self.lane.spec)
        self.script = Path(__file__).resolve()
        self.script_sha = sha256_file(self.script)
        self.state_path = config.state_dir / "state.json"
        self.receipt_dir = config.state_dir / "receipts"
        self.local_stage = config.state_dir / "stage"
        self.builder_root = f"D:/overnight/C3/native-{self.lane.name}-{self.head8}"
        self.builder_source = f"{self.builder_root}/source"
        self.builder_build = f"{self.builder_root}/build"
        self.builder_bin = f"{self.builder_root}/neutral-bin"
        self.builder_out_a = f"{self.builder_root}/package-a"
        self.builder_out_b = f"{self.builder_root}/package-b"
        self.builder_scratch_a = f"{self.builder_root}/scratch-package-a"
        self.builder_scratch_b = f"{self.builder_root}/scratch-package-b"
        self.builder_script = f"{self.builder_root}/qualify_native.py"
        self.target_root = (
            f"C:/ProgramData/NInferQualification/C3-{config.scratch_task_suffix}-{self.lane.name}-{self.head8}"
        )
        self.state = self._load_state()

    @property
    def state_root(self) -> str:
        return self.config.scratch_state_root

    @property
    def task_name(self) -> str:
        return self.lane.task_name + "-Scratch-" + self.config.scratch_task_suffix

    @property
    def gpu_uuid(self) -> str:
        bound = self.state.get("gpu_uuid")
        if bound:
            gpu_uuid_argument(bound)
            if self.config.gpu_uuid and self.config.gpu_uuid.lower() != bound.lower():
                raise LaneError("checkpoint belongs to another GPU UUID")
            return bound
        result = self.remote_json(self.config.target, """
$ErrorActionPreference='Stop'
if(Test-Path Env:CUDA_VISIBLE_DEVICES){throw 'CUDA_VISIBLE_DEVICES must be absent'}
$rows=@(nvidia-smi.exe --query-gpu=index,uuid,name,pci.bus_id,compute_cap,driver_version --format=csv,noheader,nounits)
if($LASTEXITCODE -ne 0){throw 'target GPU selection failed'}
[Console]::Out.WriteLine((@{rows=$rows}|ConvertTo-Json -Compress))
""")
        selected = select_qualified_gpu(result["rows"], self.lane.spec, self.config.gpu_uuid)
        self.state["gpu_uuid"] = selected["uuid"]
        self._save()
        return selected["uuid"]

    def _load_state(self) -> dict[str, Any]:
        scratch_binding = {
            "state_root_sha256": hashlib.sha256(str(PureWindowsPath(self.state_root)).casefold().encode()).hexdigest(),
            "task_suffix": self.config.scratch_task_suffix,
            "port": self.config.scratch_port,
            "pressure_port": self.config.pressure_port,
        }
        if self.state_path.exists():
            state = json.loads(self.state_path.read_text(encoding="utf-8"))
            if state.get("phases") and not state.get("gpu_uuid"):
                raise LaneError("unscoped qualification checkpoints cannot be reused; use a fresh state directory")
            if state.get("source_commit") != self.head or state.get("lane") != self.lane.name:
                raise LaneError("checkpoint belongs to another source, lane, or orchestrator revision")
            if state.get("scratch_binding") != scratch_binding:
                raise LaneError("checkpoint belongs to another scratch root, task, or port binding")
            if state.get("orchestrator_sha256") != self.script_sha:
                if not self.config.upgrade_orchestrator_state:
                    raise LaneError("checkpoint belongs to another source or orchestrator revision")
                runtime_diff = run(
                    [
                        "git", "diff", "--name-only", self.head, self.tool_head, "--",
                        "src", "include", "apps", "cmake", "CMakeLists.txt", "CMakePresets.json",
                    ],
                    cwd=self.config.source_root,
                ).stdout.strip()
                if runtime_diff:
                    raise LaneError("qualification-tool advancement changed runtime source paths")
                revisions = state.setdefault("orchestrator_revisions", [])
                revisions.append(state.get("orchestrator_sha256"))
                state["orchestrator_sha256"] = self.script_sha
                state["qualification_tool_commit"] = self.tool_head
                atomic_json(self.state_path, state)
            return state
        state = {
            "artifact_type": "ninfer_native_qualification_orchestration",
            "schema_version": 2,
            "lane": self.lane.name,
            "release_id": self.lane.release_id,
            "source_commit": self.head,
            "orchestrator_sha256": self.script_sha,
            "qualification_tool_commit": self.tool_head,
            "scratch_binding": scratch_binding,
            "status": "not_started",
            "current_phase": None,
            "phases": {},
            "restoration": {"status": "required"},
        }
        atomic_json(self.state_path, state)
        return state

    def _save(self) -> None:
        atomic_json(self.state_path, self.state)

    def verify_checkpoint(self, name: str, receipt: dict[str, Any]) -> bool:
        if name == "preflight":
            return False  # Every window must re-establish target quiescence and found state.
        try:
            if name == "neutral_build":
                script = f"""
$ok=$true
$items={ps_quote(json.dumps(receipt.get('files', []), separators=(',', ':')))}|ConvertFrom-Json
foreach($item in @($items)){{
  $path=Join-Path {ps_quote(self.builder_bin)} ([string]$item.name)
  if(-not(Test-Path $path -PathType Leaf) -or (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne [string]$item.sha256){{$ok=$false}}
}}
[Console]::Out.WriteLine(([ordered]@{{ok=$ok}}|ConvertTo-Json -Compress))
"""
                return bool(self.remote_json(self.config.builder, script)["ok"])
            if name == "package":
                script = f"""
$ok=$true
$arms=@(
  @{{root={ps_quote(self.builder_out_a)};package={ps_quote(receipt["package"]["sha256"])};outer={ps_quote(receipt["outer_sha256"])}}},
  @{{root={ps_quote(self.builder_scratch_a)};package={ps_quote(receipt["scratch_package"]["package"]["sha256"])};outer={ps_quote(receipt["scratch_package"]["outer_sha256"])}}}
)
foreach($arm in $arms){{
  foreach($entry in @(@{{name={ps_quote(self.lane.package_name)};sha=$arm.package}},@{{name='SHA256SUMS';sha=$arm.outer}})){{
    $path=Join-Path $arm.root $entry.name
    if(-not(Test-Path $path -PathType Leaf) -or (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha){{$ok=$false}}
  }}
}}
[Console]::Out.WriteLine(([ordered]@{{ok=$ok}}|ConvertTo-Json -Compress))
"""
                return bool(self.remote_json(self.config.builder, script)["ok"])
            if name == "transfer_install":
                script = f"""
$s=Get-Content (Join-Path {ps_quote(self.state_root)} 'state.json') -Raw|ConvertFrom-Json
$r=$s.releases.PSObject.Properties[[string]$s.active_release].Value
[Console]::Out.WriteLine(([ordered]@{{ok=([string]$r.patch_stack_sha -eq {ps_quote(self.head)} -and [string]$r.gpu_uuid -ceq {ps_quote(self.gpu_uuid)})}}|ConvertTo-Json -Compress))
"""
                return bool(self.remote_json(self.config.target, script)["ok"])
            evidence_names = {
                "protocol": "agent-protocol.json",
                "context_128k": "long-context-128k.json",
                "restart": "checkpoint-restart-proof.json",
                "security": "state-security.json",
                "benchmark_c1": "managed-c1.json",
            }
            if name in evidence_names:
                script = f"""
$path=Join-Path {ps_quote(self.target_root)} {ps_quote('evidence/' + evidence_names[name])}
[Console]::Out.WriteLine(([ordered]@{{ok=(Test-Path $path -PathType Leaf)}}|ConvertTo-Json -Compress))
"""
                return bool(self.remote_json(self.config.target, script)["ok"])
            if name == "receipt":
                return (self.config.state_dir / "qualification-summary.json").is_file()
            if name == "rollback":
                return True
            if name == "restore":
                script = f"""
$root={ps_quote(self.state_root)}
$status=&(Join-Path $root 'Control-Release.ps1') -Action Status -StateRoot $root|ConvertFrom-Json
$limit=[int][double](nvidia-smi.exe -i {ps_quote(self.gpu_uuid)} --query-gpu=power.limit --format=csv,noheader,nounits)
[Console]::Out.WriteLine(([ordered]@{{ok=([string]$status.process_state -eq 'stopped' -and -not [bool]$status.gpu_owner.lease_active -and $limit -eq {self.lane.owner_power_limit_w})}}|ConvertTo-Json -Compress))
"""
                return bool(self.remote_json(self.config.target, script)["ok"])
            return True
        except Exception:
            return False

    def phase(self, name: str, action: Callable[[], dict[str, Any]]) -> dict[str, Any]:
        existing = self.state["phases"].get(name)
        if self.config.resume and existing and existing.get("status") == "passed":
            receipt = existing["receipt"]
            if self.verify_checkpoint(name, receipt):
                return receipt
        self.state["status"] = "running"
        self.state["current_phase"] = name
        self.state["phases"][name] = {
            "status": "running",
            "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        self._save()
        try:
            receipt = action()
            pending_summary = name == "receipt" and receipt.get("status") == "restore-pending"
            no_predecessor = name == "rollback" and receipt.get("status") == "not_applicable"
            if receipt.get("status") not in (None, "passed") and not (pending_summary or no_predecessor):
                raise LaneError(f"{name} returned a non-passing receipt")
        except Exception as error:
            self.state["status"] = "failed"
            self.state["phases"][name] = {
                "status": "failed",
                "error": str(error),
                "finished_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            }
            if name == "restore":
                self.state["restoration"] = {"status": "failed", "error": str(error)}
                self._record_summary_restoration(self.state["restoration"])
            self._save()
            raise
        receipt_path = self.receipt_dir / f"{name}.json"
        atomic_json(receipt_path, receipt)
        self.state["phases"][name] = {
            "status": "passed",
            "receipt": receipt,
            "receipt_sha256": sha256_file(receipt_path),
            "finished_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        self._save()
        return receipt

    def stage_script(self, host: str, destination: str) -> None:
        scp(str(self.script), remote_spec(host, destination))

    def remote_json(self, host: str, script: str, timeout: int = 600) -> dict[str, Any]:
        if host == self.config.target:
            script = TARGET_CONTROLLER_GUARD + script
        return compact_json_from_output(remote_ps(host, script, timeout=timeout))

    def preflight(self) -> dict[str, Any]:
        if run(["git", "status", "--porcelain"], cwd=self.config.source_root).stdout.strip():
            raise LaneError("source worktree must be clean before qualification")
        runtime_diff = run(
            [
                "git", "diff", "--name-only", self.head, self.tool_head, "--",
                "src", "include", "apps", "cmake", "CMakeLists.txt", "CMakePresets.json",
            ],
            cwd=self.config.source_root,
        ).stdout.strip()
        if runtime_diff:
            raise LaneError("qualification-tool advancement changed runtime source paths")
        if self.config.long_fixture.stat().st_size <= 0:
            raise LaneError("128K fixture is empty")
        run(["ssh", "-T", self.config.builder, "exit"], timeout=30)
        run(["ssh", "-T", self.config.target, "exit"], timeout=30)
        incumbent = self.config.incumbent_state_root
        selector = f"ninfer-client-acceptance/{self.lane.model_id}"
        # Read-only facts before anything stops: the incumbent's run state, and the models the OMP client fixture
        # declares, listed by that client's own parser (no request reaches a provider). Listing runs last because it
        # changes this session's environment.
        found = self.remote_json(
            self.config.target,
            f"""
$ErrorActionPreference='Stop'
$incumbentRoot={ps_quote(incumbent or "")}
$incumbentRunning=$false
if($incumbentRoot -and (Test-Path (Join-Path $incumbentRoot 'Control-Release.ps1')) -and (Test-ScopedControllerOrQuiescent $incumbentRoot)){{
  $incumbentStatus=&(Join-Path $incumbentRoot 'Control-Release.ps1') -Action Status -StateRoot $incumbentRoot|ConvertFrom-Json
  $incumbentRunning=([string]$incumbentStatus.process_state -eq 'running')
}}
$ompRoot={ps_quote(self.config.omp_root)}
$env:LOCALAPPDATA=Join-Path $ompRoot 'localappdata'
$env:PI_CODING_AGENT_DIR=Join-Path $ompRoot 'agent'
$env:NO_COLOR='1'
$listing=&(Join-Path $env:LOCALAPPDATA 'OMP\\omp.cmd') models ninfer-client-acceptance --json --no-extensions|Out-String
if($LASTEXITCODE -ne 0){{throw 'the OMP client fixture could not list its models'}}
$ompModels=@(@(($listing|ConvertFrom-Json).models)|ForEach-Object{{[string]$_.selector}})
[Console]::Out.WriteLine(([ordered]@{{incumbent_state_root=$incumbentRoot;incumbent_was_running=$incumbentRunning;omp_models=$ompModels}}|ConvertTo-Json -Compress))
""",
        )
        if selector not in found.pop("omp_models"):
            raise LaneError(f"the OMP client fixture does not declare {selector}: add it to {self.config.omp_root}/agent/models.yml")
        pending = self.state.get("found_incumbent")
        if pending and self.state["restoration"].get("status") != "passed":
            if pending["incumbent_state_root"] != found["incumbent_state_root"]:
                raise LaneError("restore the recorded incumbent before changing its state root")
            # A crash after Stop must not turn an outstanding restart obligation into 'found stopped'.
            found["incumbent_was_running"] |= pending["incumbent_was_running"]
        self.state["found_incumbent"] = found
        self.state["restoration"] = {"status": "required"}
        self.state["phases"].pop("restore", None)
        self._record_summary_restoration(self.state["restoration"])
        self._save()  # Write-ahead: no target-side stop is permitted before this succeeds.
        status = self.remote_json(
            self.config.target,
            f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.state_root)}
$incumbentRoot={ps_quote(incumbent or "")}
$incumbentRunning={'$true' if found['incumbent_was_running'] else '$false'}
if($incumbentRoot -and (Test-Path (Join-Path $incumbentRoot 'Control-Release.ps1')) -and (Test-ScopedControllerOrQuiescent $incumbentRoot)){{
  &(Join-Path $incumbentRoot 'Control-Release.ps1') -Action Stop -StateRoot $incumbentRoot|Out-Null
}}
$controller=Join-Path $root 'Control-Release.ps1'
if(Test-Path (Join-Path $root 'state.json')){{
  $bound=Get-Content (Join-Path $root 'state.json') -Raw|ConvertFrom-Json
  if([string]$bound.task_name -cne {ps_quote(self.task_name)}){{throw 'scratch root belongs to a different lifecycle task'}}
}}
if((Test-Path $controller) -and (Test-ScopedControllerOrQuiescent $root)){{&$controller -Action Stop -StateRoot $root|Out-Null}}
$active='';$previous='';$modelSha=''
if(Test-Path (Join-Path $root 'state.json')){{
  $s=Get-Content (Join-Path $root 'state.json') -Raw|ConvertFrom-Json
  $r=$s.releases.PSObject.Properties[[string]$s.active_release].Value
  $active=[string]$s.active_release;$previous=[string]$s.previous_release;$modelSha=[string]$r.model_artifact_sha256
}}
$owned=@(Get-CimInstance Win32_Process -Filter "Name='ninfer-serve.exe'"|Where-Object{{
  $_.ExecutablePath -and ([string]$_.ExecutablePath).StartsWith(([IO.Path]::GetFullPath($root).TrimEnd('\\')+'\\'),[StringComparison]::OrdinalIgnoreCase)
}})
if($owned.Count -ne 0){{throw 'a scratch-owned ninfer-serve process is still running'}}
$limit=[int][double](nvidia-smi.exe -i {ps_quote(self.gpu_uuid)} --query-gpu=power.limit --format=csv,noheader,nounits)
$gpu=nvidia-smi.exe -i {ps_quote(self.gpu_uuid)} --query-gpu=name,memory.total,driver_version --format=csv,noheader,nounits
$modelPath={ps_quote(self.config.model_path)}
if(-not (Test-Path -LiteralPath $modelPath -PathType Leaf)){{throw 'pinned model artifact is missing on the target'}}
[Console]::Out.WriteLine(([ordered]@{{lineage_exists=(Test-Path (Join-Path $root 'state.json'));active_release=$active;previous_release=$previous;incumbent_state_root=$incumbentRoot;incumbent_was_running=$incumbentRunning;power_limit_w=$limit;gpu=[string]$gpu;model_sha256=$modelSha}}|ConvertTo-Json -Compress))
""",
            timeout=900,
        )
        if status["power_limit_w"] != self.lane.owner_power_limit_w:
            raise LaneError(f"target is not at its {self.lane.owner_power_limit_w} W owner state")
        if status["lineage_exists"] and status["model_sha256"] != self.lane.model_sha256:
            raise LaneError("target predecessor model binding changed")
        if not status["gpu"].startswith(self.lane.spec["gpu"]["name"]):
            raise LaneError(f"target GPU is not the lane's {self.lane.spec['gpu']['name']}")
        return {
            "lane": self.lane.name,
            "release_id": self.lane.release_id,
            "source_commit": self.head,
            "builder_identity_sha256": hashlib.sha256(self.config.builder.encode()).hexdigest(),
            "target_identity_sha256": hashlib.sha256(self.config.target.encode()).hexdigest(),
            # Private write-ahead state owns the recovery path; public phase receipts bind
            # that path without exporting operator roots or SSH aliases.
            "incumbent_root_sha256": hashlib.sha256((incumbent or "").encode()).hexdigest(),
            "initial": {key: value for key, value in status.items() if key != "incumbent_state_root"},
            "long_fixture_sha256": sha256_file(self.config.long_fixture),
        }

    def neutral_build(self) -> dict[str, Any]:
        if self.config.qualified_runtime_bin:
            return self.consume_qualified_build()
        self.local_stage.mkdir(parents=True, exist_ok=True)
        bundle = self.local_stage / "source.bundle"
        run(["git", "bundle", "create", str(bundle), "HEAD"], cwd=self.config.source_root)
        bundle_sha = sha256_file(bundle)
        bootstrap = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.builder_root)}
Get-CimInstance Win32_Process | Where-Object {{
  [string]$_.CommandLine -like ('*'+$root+'*') -and $_.Name -in @('cmake.exe','ninja.exe','nvcc.exe')
}} | ForEach-Object {{ & taskkill.exe /PID $_.ProcessId /T /F 2>$null | Out-Null }}
Start-Sleep -Seconds 1
if(Test-Path $root){{Remove-Item -LiteralPath $root -Recurse -Force}}
New-Item -ItemType Directory -Path $root|Out-Null
"""
        remote_ps(self.config.builder, bootstrap)
        scp(str(bundle), remote_spec(self.config.builder, f"{self.builder_root}/source.bundle"))
        self.stage_script(self.config.builder, self.builder_script)
        focused_tests = "|".join((
            "ninfer_direct_storage_checkpoint_windows_test",
            "ninfer_direct_storage_checkpoint_backend_test",
            "ninfer_session_checkpoint_store_test",
            "ninfer_openai_responses_store_test",
            "ninfer_automatic_checkpoint_queue_test",
            "ninfer_http_error_handler_test",
            "ninfer_serve_options_test",
            "ninfer_http_server_stop_test",
        ))
        build_script = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.builder_root)}
$source={ps_quote(self.builder_source)}
$build={ps_quote(self.builder_build)}
$bundle=Join-Path $root 'source.bundle'
if((Get-FileHash $bundle -Algorithm SHA256).Hash.ToLowerInvariant()-ne{ps_quote(bundle_sha)}){{throw 'bundle hash mismatch'}}
git clone --quiet --config core.autocrlf=false $bundle $source
git -C $source checkout --quiet --detach {self.head}
$env:_CL_="/experimental:deterministic /pathmap:$env:USERPROFILE=C:\\build"
# The app-local DLLs are built by vcpkg from the manifest under short neutral roots, so the
# paths they embed (__FILE__ strings, PDB references) never name the builder's profile; the
# neutral binary cache lets the second lane reuse them.
$env:VCPKG_BINARY_SOURCES='clear;files,C:\\b\\vcpkg-neutral-cache,readwrite'
New-Item -ItemType Directory -Force -Path 'C:\\b\\vcpkg-neutral-cache','C:\\b\\vt','C:\\b\\vp'|Out-Null
$pf=[Environment]::GetFolderPath('ProgramFilesX86')
$vsdev=Join-Path $pf 'Microsoft Visual Studio\\2022\\BuildTools\\Common7\\Tools\\VsDevCmd.bat'
$lines=&cmd.exe /d /s /c ('"'+$vsdev+'" -arch=x64 -host_arch=x64 >nul && set')
foreach($line in $lines){{$i=$line.IndexOf('=');if($i -gt 0){{[Environment]::SetEnvironmentVariable($line.Substring(0,$i),$line.Substring($i+1),'Process')}}}}
&cmake -S $source -B $build -G Ninja '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_CUDA_ARCHITECTURES={self.lane.cuda_architecture}' ('-DCMAKE_CUDA_COMPILER='+{ps_quote(self.config.builder_cuda_compiler)}) ('-DCMAKE_CXX_COMPILER='+{ps_quote(self.config.builder_cxx_compiler)}) ('-DCMAKE_C_COMPILER='+{ps_quote(self.config.builder_cxx_compiler)}) '-DNINFER_BUILD_APPS=ON' '-DBUILD_TESTING=ON' '-DNINFER_BUILD_BENCHMARKS=ON' '-DNINFER_BUILD_PROFILE={self.lane.build_profile}' '-DNINFER_UPSTREAM_BASE_SHA={self.lane.upstream_sha}' '-DNINFER_PATCH_STACK_SHA={self.head}' ('-DCMAKE_TOOLCHAIN_FILE='+{ps_quote(self.config.builder_vcpkg)}) '-DVCPKG_TARGET_TRIPLET=x64-windows' '-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=C:/b/vt;--x-packages-root=C:/b/vp'
if($LASTEXITCODE -ne 0){{throw 'configure failed'}}
&cmake --build $build --target {" ".join(focused_tests.split("|"))} ninfer-serve ninfer ninfer_bench --parallel 12
if($LASTEXITCODE -ne 0){{throw 'build failed'}}
&ctest --test-dir $build -C Release --output-on-failure -R '^({focused_tests})$'
if($LASTEXITCODE -ne 0){{throw 'focused tests failed'}}
$bin={ps_quote(self.builder_bin)}
New-Item -ItemType Directory -Path $bin|Out-Null
Copy-Item (Join-Path $build 'apps\\ninfer.exe'),(Join-Path $build 'apps\\ninfer-serve.exe'),(Join-Path $build 'bench\\ninfer_bench.exe') -Destination $bin
# App-local runtime: shared CUDA, /MD CRT and vcpkg imports use their selected toolchain roots,
# never the target's development-toolkit/PATH installation.
{NATIVE_RUNTIME_DEPENDENCIES_PS}
$dumpbin=(Get-Command dumpbin.exe).Source
$runtimeDlls=@(Copy-NativeRuntimeDependencies -Build $build -Bin $bin -CudaCompiler {ps_quote(self.config.builder_cuda_compiler)} -MsvcRedist ([string]$env:VCToolsRedistDir) -Dumpbin $dumpbin -DirectStorageDlls @({", ".join(ps_quote(name) for name in DIRECTSTORAGE_DLLS)}))
$items=Get-ChildItem $bin -File|Sort-Object Name|ForEach-Object{{[ordered]@{{name=$_.Name;bytes=$_.Length;sha256=(Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}}}}
[Console]::Out.WriteLine(([ordered]@{{status='passed';source_commit=(git -C $source rev-parse HEAD);bundle_sha256={ps_quote(bundle_sha)};cuda_architecture={ps_quote(self.lane.cuda_architecture)};build_profile={ps_quote(self.lane.build_profile)};runtime_dlls=$runtimeDlls;files=$items}}|ConvertTo-Json -Depth 6 -Compress))
"""
        receipt = self.remote_json(self.config.builder, build_script, timeout=5400)
        return receipt

    def consume_qualified_build(self) -> dict[str, Any]:
        """Snapshot an integrator-qualified flat runtime; never rebuild or mutate its inputs."""
        self.local_stage.mkdir(parents=True, exist_ok=True)
        bundle = self.local_stage / "source.bundle"
        run(["git", "bundle", "create", str(bundle), "HEAD"], cwd=self.config.source_root)
        bundle_sha = sha256_file(bundle)
        remote_ps(self.config.builder, f"""
$root={ps_quote(self.builder_root)}
New-Item -ItemType Directory -Path $root -Force|Out-Null
""")
        scp(str(bundle), remote_spec(self.config.builder, f"{self.builder_root}/source.bundle"))
        self.stage_script(self.config.builder, self.builder_script)
        return self.remote_json(self.config.builder, f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.builder_root)}
$source={ps_quote(self.builder_source)}
$bundle=Join-Path $root 'source.bundle'
if((Get-FileHash $bundle -Algorithm SHA256).Hash.ToLowerInvariant() -cne {ps_quote(bundle_sha)}){{throw 'source bundle hash mismatch'}}
if(-not(Test-Path $source)){{
  git clone --quiet --config core.autocrlf=false $bundle $source
  if($LASTEXITCODE -ne 0){{throw 'qualified source clone failed'}}
  git -C $source checkout --quiet --detach {self.head}
  if($LASTEXITCODE -ne 0){{throw 'qualified source checkout failed'}}
}}
if([string](&git -C $source rev-parse HEAD) -cne {ps_quote(self.head)} -or [string](&git -C $source status --porcelain)){{throw 'qualified source staging is not the exact clean candidate'}}
$inputBin={ps_quote(self.config.qualified_runtime_bin)}
$bin={ps_quote(self.builder_bin)}
New-Item -ItemType Directory -Path $bin|Out-Null
$files=@(Get-ChildItem -LiteralPath $inputBin -File|Where-Object{{$_.Extension -ieq '.dll' -or $_.Name -in @('ninfer.exe','ninfer-serve.exe','ninfer_bench.exe')}})
foreach($file in $files){{
  $hash=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
  $destination=Join-Path $bin $file.Name
  Copy-Item -LiteralPath $file.FullName -Destination $destination
  if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -cne $hash){{throw 'qualified runtime copy hash mismatch'}}
}}
&python -B {ps_quote(self.builder_script)} _verify-native-build --source-root $source --runtime-bin $bin --build-receipt {ps_quote(self.config.qualified_build_receipt)} --source-commit {self.head} --cuda-architecture {self.lane.cuda_architecture} --build-profile {ps_quote(self.lane.build_profile)} --upstream-base-sha {self.lane.upstream_sha}
if($LASTEXITCODE -ne 0){{throw 'qualified native identity verification failed'}}
""", timeout=900)

    def private_scan(self) -> dict[str, Any]:
        output = run(
            [
                "ssh",
                "-T",
                self.config.builder,
                "python",
                self.builder_script,
                "_scan-dir",
                "--path",
                self.builder_bin,
            ],
            timeout=600,
        ).stdout
        remote = compact_json_from_output(output)
        if remote["findings"]:
            raise LaneError(f"neutral binary scan found {remote['findings']} private markers")
        tracked = 0
        tracked_findings = 0
        names = subprocess.check_output(["git", "ls-files", "-z"], cwd=self.config.source_root)
        private_markers = tuple(
            value.lower().encode()
            for value in (
                self.config.builder,
                self.config.target,
                str(Path.home()) + os.sep,
            )
            if value
        )
        for raw in names.split(b"\0"):
            if not raw or raw == b"AGENTS.md":
                continue
            path = self.config.source_root / raw.decode()
            if path.is_file():
                tracked += 1
                tracked_findings += marker_findings(path.read_bytes(), private_markers=private_markers)
        if tracked_findings:
            raise LaneError("tracked source contains a private marker or credential-shaped value")
        return {"status": "passed", "neutral_bin": remote, "tracked_files": tracked, "tracked_findings": 0}

    def package(self) -> dict[str, Any]:
        epoch = run(["git", "show", "-s", "--format=%ct", self.head], cwd=self.config.source_root).stdout.strip()
        config_bytes = subprocess.check_output(
            ["git", "show", f"{self.head}:packaging/windows/lanes/{self.lane.name}/server-config.json"],
            cwd=self.config.source_root,
        )
        config_sha = hashlib.sha256(config_bytes).hexdigest()
        package_script = f"""
$ErrorActionPreference='Stop'
$source={ps_quote(self.builder_source)}
$bin={ps_quote(self.builder_bin)}
if((Get-FileHash (Join-Path $source 'packaging/windows/lanes/{self.lane.name}/server-config.json') -Algorithm SHA256).Hash.ToLowerInvariant() -cne {ps_quote(config_sha)}){{throw 'public config checkout bytes differ from the committed source'}}
$runtime=@(Get-ChildItem $bin -Filter '*.dll' -File|Sort-Object Name|ForEach-Object FullName)
function Build-One([string]$Out,[string]$Config){{
  if(Test-Path $Out){{Remove-Item $Out -Recurse -Force}}
  $parameters=@{{
    Lane={ps_quote(self.lane.name)};SourceRoot=$source;
    NInferExecutable=(Join-Path $bin 'ninfer.exe');ServerExecutable=(Join-Path $bin 'ninfer-serve.exe');
    BenchmarkExecutable=(Join-Path $bin 'ninfer_bench.exe');ReleaseHeadSha={ps_quote(self.head)};RuntimeSourceSha={ps_quote(self.head)};
    RuntimeFile=$runtime;DependencySbomTool={ps_quote(self.config.dependency_sbom_tool)};
    VcpkgResolved={ps_quote(self.config.vcpkg_resolved)};CudaInventory={ps_quote(self.config.cuda_inventory)};
    ThirdPartyInventory={ps_quote(self.config.third_party_inventory)};RuntimeInventory={ps_quote(self.config.runtime_inventory)};
    PythonExecutable='python';SourceDateEpoch={epoch};OutputDirectory=$Out
  }}
  if($Config){{$parameters['ServerConfig']=$Config}}
  &(Join-Path $source 'packaging\\windows\\New-Package.ps1') @parameters|Out-Null
}}
Build-One {ps_quote(self.builder_out_a)} ''
Build-One {ps_quote(self.builder_out_b)} ''
$a=Get-Content (Join-Path {ps_quote(self.builder_out_a)} 'package-build-receipt.json') -Raw | ConvertFrom-Json
$b=Get-Content (Join-Path {ps_quote(self.builder_out_b)} 'package-build-receipt.json') -Raw | ConvertFrom-Json
$outerA=(Get-FileHash (Join-Path {ps_quote(self.builder_out_a)} 'SHA256SUMS') -Algorithm SHA256).Hash.ToLowerInvariant()
$outerB=(Get-FileHash (Join-Path {ps_quote(self.builder_out_b)} 'SHA256SUMS') -Algorithm SHA256).Hash.ToLowerInvariant()
if([string]$a.package.sha256 -cne [string]$b.package.sha256 -or $outerA -cne $outerB){{throw 'public distribution is not deterministic'}}
$publicConfig=Get-Content (Join-Path $source 'packaging/windows/lanes/{self.lane.name}/server-config.json') -Raw|ConvertFrom-Json
$scratchConfig=Get-Content (Join-Path $source 'packaging/windows/lanes/{self.lane.name}/server-config.json') -Raw|ConvertFrom-Json
$scratchConfig.listen.port={self.config.scratch_port}
$scratchConfigPath=Join-Path {ps_quote(self.builder_root)} 'scratch-server-config.json'
[IO.File]::WriteAllText($scratchConfigPath,(($scratchConfig|ConvertTo-Json -Depth 32)+[char]10),[Text.UTF8Encoding]::new($false))
Build-One {ps_quote(self.builder_scratch_a)} $scratchConfigPath
Build-One {ps_quote(self.builder_scratch_b)} $scratchConfigPath
$sa=Get-Content (Join-Path {ps_quote(self.builder_scratch_a)} 'package-build-receipt.json') -Raw|ConvertFrom-Json
$sb=Get-Content (Join-Path {ps_quote(self.builder_scratch_b)} 'package-build-receipt.json') -Raw|ConvertFrom-Json
$scratchOuterA=(Get-FileHash (Join-Path {ps_quote(self.builder_scratch_a)} 'SHA256SUMS') -Algorithm SHA256).Hash.ToLowerInvariant()
$scratchOuterB=(Get-FileHash (Join-Path {ps_quote(self.builder_scratch_b)} 'SHA256SUMS') -Algorithm SHA256).Hash.ToLowerInvariant()
if([string]$sa.package.sha256 -cne [string]$sb.package.sha256 -or $scratchOuterA -cne $scratchOuterB){{throw 'scratch distribution is not deterministic'}}
[Console]::Out.WriteLine((@{{public=$a;scratch=$sa;outer_sha256=$outerA;scratch_outer_sha256=$scratchOuterA;public_config=$publicConfig;scratch_config=$scratchConfig}}|ConvertTo-Json -Depth 24 -Compress))
"""
        result = self.remote_json(self.config.builder, package_script, timeout=3600)
        receipt = result["public"]
        scratch = result["scratch"]
        require_port_only_equivalence(result["public_config"], result["scratch_config"], self.config.scratch_port)
        if receipt["binaries"] != scratch["binaries"] or receipt["support_assets"] != scratch["support_assets"]:
            raise LaneError("scratch package changed executable or lifecycle bytes")
        receipt["outer_sha256"] = result["outer_sha256"]
        scratch["outer_sha256"] = result["scratch_outer_sha256"]
        receipt["scratch_package"] = scratch
        receipt["scratch_equivalence"] = {
            "status": "passed",
            "configuration_delta": {"path": "/listen/port", "public": 18082, "scratch": self.config.scratch_port},
            "public_package_sha256": receipt["package"]["sha256"],
            "scratch_package_sha256": scratch["package"]["sha256"],
            "public_configuration_sha256": receipt["config_sha256"],
            "scratch_configuration_sha256": scratch["config_sha256"],
            "identical_binaries": receipt["binaries"],
            "identical_support_assets": receipt["support_assets"],
            "public_package_executed": False,
            "production_route_activation_performed": False,
        }
        scan_output = run(
            [
                "ssh",
                "-T",
                self.config.builder,
                "python",
                self.builder_script,
                "_scan-archive",
                "--path",
                f"{self.builder_out_a}/{self.lane.package_name}",
            ],
            timeout=900,
        ).stdout
        scan = compact_json_from_output(scan_output)
        if scan["findings"]:
            raise LaneError(f"packaged binary contains {scan['findings']} private markers")
        receipt["public_scan"] = scan
        return receipt

    def transfer_package(self) -> dict[str, Any]:
        """Move only the scratch rehearsal package and its lifecycle scripts to the target.

        The same host copies locally. Two Windows hosts cannot carry bulk over ssh in either
        direction, and relaying through the operator's machine (``scp -3``) moves a 0.6 GB
        package at well under 1 MB/s across two long links, so the package travels as ranged
        HTTP served by the builder and fetched by the target (95 MB/s measured on the SF LAN)
        while the small scripts still relay. Both ends verify the SHA-256; the installer
        checks it again against the package receipt.
        """
        names = (
            "Install-Release.ps1",
            "Protect-StateRoot.ps1",
            "Control-GpuOwner.ps1",
            "package-build-receipt.json",
        )
        if self.config.builder == self.config.target:
            items = ",".join(ps_quote(f"{self.builder_scratch_a}/{name}") for name in (self.lane.package_name, *names))
            remote_ps(
                self.config.target,
                f"Copy-Item -LiteralPath @({items}) -Destination {ps_quote(self.target_root)} -Force",
            )
            return {"transport": "local_copy"}
        for name in names:
            scp(
                remote_spec(self.config.builder, f"{self.builder_scratch_a}/{name}"),
                remote_spec(self.config.target, f"{self.target_root}/{name}"),
                through_local=True,
            )
        tool = self.config.transfer_tool
        if tool is None:
            raise LaneError("--transfer-tool is required when the builder and target are different hosts")
        source = f"{self.builder_scratch_a}/{self.lane.package_name}"
        destination = f"{self.target_root}/{self.lane.package_name}"
        scp(str(tool), remote_spec(self.config.builder, f"{self.builder_root}/transfer.py"))
        scp(str(tool), remote_spec(self.config.target, f"{self.target_root}/transfer.py"))
        server = subprocess.Popen(
            ["ssh", "-T", self.config.builder, "python", f"{self.builder_root}/transfer.py", "serve",
             "--local", source, "--timeout", "900"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            announced = json.loads(server.stdout.readline() or "{}")
            if announced.get("status") != "serving":
                raise LaneError(f"the builder did not start serving the package: {announced}")
            address = run(["ssh", "-G", self.config.builder]).stdout
            host = next(line.split()[1] for line in address.splitlines() if line.startswith("hostname "))
            fetched = compact_json_from_output(run(
                ["ssh", "-T", self.config.target, "python", f"{self.target_root}/transfer.py", "fetch",
                 "--url", f"http://{host}:{announced['port']}", "--token", announced["token"],
                 "--local", destination, "--timeout", "300"],
                timeout=900,
            ).stdout)
            served, _ = server.communicate(timeout=120)
        finally:
            if server.poll() is None:
                server.kill()
        if fetched.get("status") != "passed" or fetched.get("sha256") != announced.get("sha256"):
            raise LaneError(f"package fetch did not pass: {fetched}")
        if compact_json_from_output(served).get("status") != "passed":
            raise LaneError(f"package serve did not complete: {served[-400:]}")
        return {"transport": "ranged_http", "streams": fetched.get("streams"), "wall_s": fetched.get("wall_s"),
                "mb_per_s": fetched.get("mb_per_s"), "bytes": fetched.get("bytes")}

    def transfer_install(self) -> dict[str, Any]:
        package = self.state["phases"]["package"]["receipt"]["scratch_package"]["package"]
        target_root = self.target_root
        # The staging root and the clean-install root live under the qualification parent, so
        # the parent must be a protected root (SYSTEM and Administrators only, no inherited
        # DACL): a plain New-Item under ProgramData would inherit BUILTIN\Users write access,
        # and the installer refuses to create protected state beneath such an ancestor. The
        # candidate's own protection library creates or checks it before anything is staged.
        protect = self.config.source_root / "packaging" / "windows" / "Protect-StateRoot.ps1"
        helper = f"C:/Windows/Temp/ninfer-protect-{self.head8}.ps1"
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", suffix=".ps1", delete=False) as stream:
            stream.write(run(["git", "show", f"{self.head}:{protect.relative_to(self.config.source_root).as_posix()}"],
                             cwd=self.config.source_root).stdout)
            local_helper = Path(stream.name)
        try:
            scp(str(local_helper), remote_spec(self.config.target, helper))
        finally:
            local_helper.unlink(missing_ok=True)
        remote_ps(
            self.config.target,
            f"""
$ErrorActionPreference='Stop'
. {ps_quote(helper)}
$p={ps_quote(target_root)}
Initialize-NInferProtectedStateRoot (Split-Path -Parent $p)|Out-Null
New-Item -ItemType Directory -Path $p,(Join-Path $p 'evidence') -Force|Out-Null
Remove-Item -LiteralPath {ps_quote(helper)} -Force
""",
        )
        transfer = self.transfer_package()
        self.stage_script(self.config.target, f"{target_root}/qualify_native.py")
        scp(str(self.config.long_fixture), remote_spec(self.config.target, f"{target_root}/long_niah_128k.json"))
        scp(
            str(self.config.source_root / "tests" / "test_release_security.ps1"),
            remote_spec(self.config.target, f"{target_root}/test_release_security.ps1"),
        )
        # The operator explicitly supplies the protected key. No shipped state root is
        # searched for credentials, and clean-install rehearsal has its own task name.
        install_script = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.state_root)}
$stage={ps_quote(target_root)}
$taskName={ps_quote(self.task_name)}
$before=''
$apiKeyFile={ps_quote(self.config.api_key_file)}
if(Test-Path (Join-Path $root 'state.json')){{
  $s=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
  if([string]$s.task_name -cne $taskName){{throw 'scratch state has a different lifecycle task binding'}}
  if(Test-ScopedControllerOrQuiescent $root){{&(Join-Path $root 'Control-Release.ps1') -Action Stop -StateRoot $root|Out-Null}}
  $before=[string]$s.active_release
}}
if(-not $apiKeyFile -or -not (Test-Path -LiteralPath $apiKeyFile -PathType Leaf)){{throw 'no operator API-key file is available for the lane'}}
$package=Join-Path $stage {ps_quote(self.lane.package_name)}
if((Get-FileHash $package -Algorithm SHA256).Hash.ToLowerInvariant() -ne {ps_quote(package['sha256'])}){{throw 'package transfer hash mismatch'}}
. (Join-Path $stage 'Protect-StateRoot.ps1')
$cleanParent=Join-Path $stage 'clean-install'
Initialize-NInferProtectedStateRoot $cleanParent|Out-Null
$cleanRoot=Join-Path $cleanParent {ps_quote(self.lane.spec['lifecycle']['state_root_name'])}
if(Test-Path $cleanRoot){{throw 'clean-install root already exists'}}
$cleanTaskName=$taskName+'-Clean'
if($null -ne (Get-ScheduledTask -TaskName $cleanTaskName -ErrorAction SilentlyContinue)){{throw 'clean-install task already exists'}}
try{{
  $cleanLines=@(&(Join-Path $stage 'Install-Release.ps1') -PackagePath $package -PackageSha256 {ps_quote(package['sha256'])} -ModelArtifactPath {ps_quote(self.config.model_path)} -ApiKeyFile $apiKeyFile -GpuUuid {ps_quote(self.gpu_uuid)} -GpuOwnerControllerPath (Join-Path $stage 'Control-GpuOwner.ps1') -StateRoot $cleanRoot -ScratchTaskSuffix {ps_quote(self.config.scratch_task_suffix + '-Clean')} -NoStart)
  $clean=[string]$cleanLines[-1]|ConvertFrom-Json
  if([string]$clean.status -cne 'passed'){{throw 'clean install did not pass'}}
}}finally{{
  $cleanController=Join-Path $cleanRoot 'Control-Release.ps1'
  if(Test-Path $cleanController){{&$cleanController -Action Uninstall -StateRoot $cleanRoot|Out-Null}}
}}
$managedArguments=@{{PackagePath=$package;PackageSha256={ps_quote(package['sha256'])};ModelArtifactPath={ps_quote(self.config.model_path)};ApiKeyFile=$apiKeyFile;StateRoot=$root;ScratchTaskSuffix={ps_quote(self.config.scratch_task_suffix)};NoStart=$true;GpuUuid={ps_quote(self.gpu_uuid)};GpuOwnerControllerPath=(Join-Path $stage 'Control-GpuOwner.ps1')}}
$upgradeLines=@(&(Join-Path $stage 'Install-Release.ps1') @managedArguments)
$upgrade=[string]$upgradeLines[-1]|ConvertFrom-Json
if([string]$upgrade.status -cnotin @('passed','already_installed')){{throw 'managed install did not pass'}}
$after=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
if([string]$after.task_name -cne $taskName){{throw 'installed scratch task binding mismatch'}}
[Console]::Out.WriteLine(([ordered]@{{status='passed';before=$before;active_release=[string]$after.active_release;previous_release=[string]$after.previous_release;package_sha256={ps_quote(package['sha256'])};clean_installs=1;managed_installs=1;first_release_of_lineage=(-not $before)}}|ConvertTo-Json -Compress))
"""
        receipt = self.remote_json(self.config.target, install_script, timeout=1800)
        receipt["transfer"] = transfer
        return receipt

    def target_internal(self, phase: str, *arguments: str, timeout: int = 1800) -> dict[str, Any]:
        self.stage_script(self.config.target, f"{self.target_root}/qualify_native.py")
        command = [
            "ssh",
            "-T",
            self.config.target,
            "python",
            f"{self.target_root}/qualify_native.py",
            phase,
            "--state-root",
            self.state_root,
            "--model-id",
            self.lane.model_id,
            "--gpu-uuid",
            self.gpu_uuid,
            "--evidence-root",
            f"{self.target_root}/evidence",
            *arguments,
        ]
        return compact_json_from_output(run(command, timeout=timeout).stdout)

    def protocol(self) -> dict[str, Any]:
        script = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.state_root)}
$stage={ps_quote(self.target_root)}
New-Item -ItemType Directory -Path (Join-Path $stage 'evidence') -Force|Out-Null
$controller=Join-Path $root 'Control-Release.ps1'
&$controller -Action Start -StateRoot $root|Out-Null
$s=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
$r=$s.releases.PSObject.Properties[[string]$s.active_release].Value
$key=(Get-Content ([string]$r.api_key_file) -Raw).Trim()
$status=Invoke-RestMethod -Method Get -Uri ('http://'+[string]$r.host+':'+[string]$r.port+'/v1/ninfer/status') -Headers @{{Authorization=('Bearer '+$key)}} -TimeoutSec 10
if([string]$status.artifact_type -cne 'ninfer_server_status' -or [int]$status.schema_version -ne 2){{throw 'protocol did not read status schema 2'}}
$launch=Get-Content (Join-Path $root 'runtime.json') -Raw|ConvertFrom-Json
if(-not [bool]$launch.gpu_uuid_verified -or [string]$launch.gpu_uuid -cne [string]$r.gpu_uuid -or
   [string]$launch.gpu_name -cne [string]$r.gpu_name){{throw 'protocol launch lacks completed GPU attestation'}}
$path=Join-Path ([string]$r.release_root) 'bin\\qualification\\agent_protocol.py'
$out=Join-Path $stage 'evidence\\agent-protocol.json'
&python $path --base-url ('http://'+[string]$r.host+':'+[string]$r.port) --model {ps_quote(self.lane.model_id)} --api-key-file ([string]$r.api_key_file) --expect-binary-sha256 ([string]$r.binary_sha256) --expect-model-artifact-sha256 ([string]$r.model_artifact_sha256) --expect-config-sha256 ([string]$r.config_sha256) --expect-deployment-profile ([string]$r.deployment_profile) 1>$out
if($LASTEXITCODE -ne 0){{throw 'protocol failed'}}
$v=Get-Content $out -Raw | ConvertFrom-Json
[Console]::Out.WriteLine(([ordered]@{{status=[string]$v.status;checks=@($v.checks.PSObject.Properties).Count;sha256=(Get-FileHash $out -Algorithm SHA256).Hash.ToLowerInvariant();protocol=$v;identity=$status.identity;runtime=$status.runtime;startup_attestation=@{{request_log_schema=30;gpu_uuid_verified=$true;gpu_index=[int]$launch.gpu_device;gpu_name=[string]$launch.gpu_name}}}}|ConvertTo-Json -Depth 16 -Compress))
"""
        return self.remote_json(self.config.target, script, timeout=1200)

    def pressure_protocol(self) -> dict[str, Any]:
        """Exercise the installed bytes with a reduced shared Host budget and scratch store.

        No independent Host slot policy is recreated, and the managed release's cache is untouched.
        """
        pressure_mib = host_context_mib(self.lane.spec, state_images=PRESSURE_STATE_IMAGES)
        script = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.state_root)}
$stage={ps_quote(self.target_root)}
$controller=Join-Path $root 'Control-Release.ps1'
&$controller -Action Stop -StateRoot $root|Out-Null
$s=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
$r=$s.releases.PSObject.Properties[[string]$s.active_release].Value
$c=Get-Content ([string]$r.config_file) -Raw | ConvertFrom-Json
$scratch=Join-Path $stage 'pressure'
if(Test-Path $scratch){{Remove-Item $scratch -Recurse -Force}}
$checkpoints=Join-Path $scratch 'checkpoints'
New-Item -ItemType Directory -Force -Path $checkpoints|Out-Null
$port={self.config.pressure_port}
if(Test-Path Env:CUDA_VISIBLE_DEVICES){{throw 'CUDA_VISIBLE_DEVICES must be absent'}}
if([string]$r.gpu_uuid -cne {ps_quote(self.gpu_uuid)}){{throw 'pressure release GPU differs from qualification target'}}
$gpuRows=@(nvidia-smi.exe -i ([string]$r.gpu_uuid) --query-gpu=index,uuid --format=csv,noheader,nounits)
if($LASTEXITCODE -ne 0 -or $gpuRows.Count -ne 1){{throw 'pressure GPU identity query failed'}}
$gpuParts=@(([string]$gpuRows[0] -split ',')|ForEach-Object {{$_.Trim()}})
$gpuIndex=-1
if($gpuParts.Count -ne 2 -or -not [int]::TryParse($gpuParts[0],[ref]$gpuIndex) -or $gpuIndex -lt 0 -or $gpuParts[1] -cne [string]$r.gpu_uuid){{throw 'pressure GPU identity mismatch'}}
if($null -ne (Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue)){{throw 'pressure port is occupied'}}
$requestLog=Join-Path $scratch 'requests.jsonl'
$serverArguments=@(
  [string]$r.model_artifact,
  '--host',[string]$r.host,'--port',[string]$port,
  '--api-key-file',[string]$r.api_key_file,'--model-id',[string]$c.model_id,
  '--binary-sha256',[string]$r.binary_sha256,'--artifact-sha256',[string]$r.model_artifact_sha256,
  '--config-sha256',[string]$r.config_sha256,'--deployment-profile',[string]$r.deployment_profile,
  '--device',[string]$gpuIndex,'--max-context',[string]$c.engine.max_context,
  '--kv-capacity',[string]$c.engine.kv_capacity,'--prefill-chunk',[string]$c.engine.prefill_chunk,
  '--admission-headroom-mib',[string]$c.engine.admission_headroom_mib,
  '--kv-dtype',[string]$c.engine.kv_dtype,'--max-concurrency',[string]$c.engine.max_concurrency,
  '--max-pending-requests',[string]$c.engine.max_pending_requests,
  '--pending-timeout-ms',[string]$c.engine.pending_timeout_ms,
  '--device-state-slots',[string]$c.context_cache.device_state_slots,
  '--host-context-mib','{pressure_mib}',
  '--response-store-max-records',[string]$c.response_store.max_records,
  '--response-store-max-mib',[string]$c.response_store.max_mib,
  '--session-checkpoint-dir',$checkpoints,
  '--session-checkpoint-quota-mib',[string]$c.session_checkpoint.quota_mib,
  '--session-checkpoint-staging-mib',[string]$c.session_checkpoint.staging_mib,
  '--session-checkpoint-require-origin-auth',
  '--request-log-jsonl',$requestLog,'--log-stats-interval-ms','0','--preserve-thinking',
  '--spec','mtp','--draft-tokens',[string]$c.speculative.draft_tokens,'--lm-head-draft')
$keepWarm=$c.engine.PSObject.Properties['gpu_keep_warm_ms']
if($null -ne $keepWarm -and [int]$keepWarm.Value -gt 0){{$serverArguments+=@('--gpu-keep-warm-ms',[string]$keepWarm.Value)}}
$stdout=Join-Path $scratch 'stdout.log'
$stderr=Join-Path $scratch 'stderr.log'
$startInfo=[Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName=[string]$r.server_executable
# Windows argv quoting: double backslashes before quotes and at the quoted argument's end.
$startInfo.Arguments=(@($serverArguments|ForEach-Object {{'"'+[regex]::Replace([regex]::Replace($_,'(\\\\*)"','$1$1\\"'),'(\\\\+)$','$1$1')+'"'}}) -join ' ')
$startInfo.WorkingDirectory=[string]$r.release_root
$startInfo.UseShellExecute=$false
$startInfo.RedirectStandardOutput=$true;$startInfo.RedirectStandardError=$true
$startInfo.EnvironmentVariables['CUDA_DEVICE_ORDER']='PCI_BUS_ID'
$stdoutStream=$null;$stderrStream=$null;$stdoutCopy=$null;$stderrCopy=$null;$process=$null
try{{
  $stdoutStream=[IO.File]::Open($stdout,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
  $stderrStream=[IO.File]::Open($stderr,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
  $process=[Diagnostics.Process]::Start($startInfo)
  $stdoutCopy=$process.StandardOutput.BaseStream.CopyToAsync($stdoutStream)
  $stderrCopy=$process.StandardError.BaseStream.CopyToAsync($stderrStream)
  $key=(Get-Content ([string]$r.api_key_file) -Raw).Trim()
  $deadline=(Get-Date).AddSeconds(600)
  $ready=$false
  while((Get-Date) -lt $deadline){{
    if($process.HasExited){{throw "pressure server exited with code $($process.ExitCode)"}}
    try{{
      Invoke-RestMethod -Method Get -Uri ('http://'+[string]$r.host+':'+[string]$port+'/v1/ninfer/status') -Headers @{{Authorization=('Bearer '+$key)}} -TimeoutSec 5|Out-Null
      $ready=$true;break
    }}catch{{Start-Sleep -Seconds 2}}
  }}
  if(-not $ready){{throw 'pressure server did not become ready'}}
  $start=Get-Content $requestLog -TotalCount 1|ConvertFrom-Json
  if([string]$start.artifact_type -cne 'ninfer_serve_request_log' -or [int]$start.schema_version -ne 30 -or
     [string]$start.event -cne 'server_start' -or [string]$start.environment.gpu_uuid -cne [string]$r.gpu_uuid -or
     [int]$start.environment.device -ne $gpuIndex -or [string]$start.environment.gpu_name -cne [string]$r.gpu_name -or
     [string]$start.server_instance_id -cnotmatch ('^serve-'+$process.Id+'-[0-9]+$')){{throw 'pressure server_start GPU UUID mismatch'}}
  $path=Join-Path ([string]$r.release_root) 'bin\\qualification\\agent_protocol.py'
  $out=Join-Path $stage 'evidence\\agent-protocol-pressure.json'
  &python $path --base-url ('http://'+[string]$r.host+':'+[string]$port) --model {ps_quote(self.lane.model_id)} --api-key-file ([string]$r.api_key_file) --expect-binary-sha256 ([string]$r.binary_sha256) --expect-model-artifact-sha256 ([string]$r.model_artifact_sha256) --expect-config-sha256 ([string]$r.config_sha256) --expect-deployment-profile ([string]$r.deployment_profile) 1>$out
  if($LASTEXITCODE -ne 0){{throw ('protocol under shared Host pressure failed: '+((Get-Content $stderr -Tail 2) -join ' | '))}}
  $v=Get-Content $out -Raw | ConvertFrom-Json
  [Console]::Out.WriteLine(([ordered]@{{status=[string]$v.status;host_context_mib='{pressure_mib}';device_state_slots=[int]$c.context_cache.device_state_slots;checks=@($v.checks.PSObject.Properties).Count;sha256=(Get-FileHash $out -Algorithm SHA256).Hash.ToLowerInvariant();protocol=$v}}|ConvertTo-Json -Depth 16 -Compress))
}}
finally{{
  try{{
    if($null -ne $process -and -not $process.HasExited){{Stop-Process -Id $process.Id -Force;$process.WaitForExit(30000)|Out-Null}}
    if($null -ne $stdoutCopy){{$stdoutCopy.GetAwaiter().GetResult()}}
    if($null -ne $stderrCopy){{$stderrCopy.GetAwaiter().GetResult()}}
  }}finally{{
    if($null -ne $stdoutStream){{$stdoutStream.Dispose()}}
    if($null -ne $stderrStream){{$stderrStream.Dispose()}}
    Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
  }}
}}
"""
        return self.remote_json(self.config.target, script, timeout=1800)

    def context(self) -> dict[str, Any]:
        return self.target_internal("_long", "--fixture", f"{self.target_root}/long_niah_128k.json", timeout=1800)

    def restart(self) -> dict[str, Any]:
        return self.target_internal("_restart", timeout=1800)

    def rollback(self) -> dict[str, Any]:
        install = self.state["phases"]["transfer_install"]["receipt"]
        candidate = install["active_release"]
        if not install.get("previous_release"):
            # The first release of a lineage has nothing to roll back to; the bidirectional
            # rollback is proven on the next release of the same lineage.
            return {"status": "not_applicable", "reason": "first release of the lineage",
                    "active_release": candidate, "directions": 0}
        # Each direction records how its stop reached the server. The candidate declares the
        # stop-event channel, so its stop is a signal; the predecessor predates the channel and
        # its record says so, which is exactly why the shared controller must not pass the flag
        # to a binary that would refuse it.
        script = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.state_root)}
$c={ps_quote(candidate)}
$controller=Join-Path $root 'Control-Release.ps1'
function Stop-And-Report {{
  &$controller -Action Stop -StateRoot $root|Out-Null
  $s=Get-Content (Join-Path $root 'last-stop.json') -Raw | ConvertFrom-Json
  return [ordered]@{{release=[string]$s.release_id;mode=[string]$s.mode;outcome=[string]$s.outcome;forced=[bool]$s.forced;exit_code=$s.exit_code}}
}}
$stops=[Collections.Generic.List[object]]::new()
&$controller -Action Start -StateRoot $root|Out-Null
$stops.Add((Stop-And-Report))
&$controller -Action Rollback -StateRoot $root|Out-Null
$a=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
$first=[string]$a.active_release
$stops.Add((Stop-And-Report))
&$controller -Action Rollback -StateRoot $root|Out-Null
$b=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
if([string]$b.active_release -cne $c){{throw 'bidirectional rollback did not restore candidate'}}
$candidateStop=@($stops|Where-Object{{[string]$_.release -ceq $c}})
if($candidateStop.Count -lt 1){{throw 'no recorded stop belongs to the candidate'}}
if([string]$candidateStop[0].mode -cne 'stop-event' -or [string]$candidateStop[0].outcome -cne 'graceful'){{
  throw ('the candidate was not stopped by its signal: '+([string]$candidateStop[0].mode)+'/'+([string]$candidateStop[0].outcome))
}}
$predecessorStop=@($stops|Where-Object{{[string]$_.release -cne $c}})
$predecessorMode=$null
if($predecessorStop.Count -ge 1){{
  $predecessorMode=[string]$predecessorStop[0].mode
  $record=$b.releases.PSObject.Properties[[string]$predecessorStop[0].release].Value
  $declared=if($null -eq $record.PSObject.Properties['managed_stop']){{'terminate'}}else{{[string]$record.managed_stop}}
  if($predecessorMode -cne $declared){{
    throw ('the predecessor stop used '+$predecessorMode+' against a release that declares '+$declared)
  }}
}}
[Console]::Out.WriteLine(([ordered]@{{status='passed';directions=2;intermediate_release=$first;active_release=[string]$b.active_release;stops=$stops;candidate_stop_mode=[string]$candidateStop[0].mode;predecessor_stop_mode=$predecessorMode}}|ConvertTo-Json -Depth 6 -Compress))
"""
        return self.remote_json(self.config.target, script, timeout=1800)

    def security(self) -> dict[str, Any]:
        script = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.state_root)}
$stage={ps_quote(self.target_root)}
$controller=Join-Path $root 'Control-Release.ps1'
&$controller -Action Stop -StateRoot $root|Out-Null
$s=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
$r=$s.releases.PSObject.Properties[[string]$s.active_release].Value
$l=Join-Path ([string]$r.release_root) 'bin\\lifecycle'
$lines=@(&(Join-Path $stage 'test_release_security.ps1') -StateProtectionPath (Join-Path $l 'Protect-StateRoot.ps1') -GpuOwnerControllerPath (Join-Path $l 'Control-GpuOwner.ps1') -InstallerPath (Join-Path $l 'Install-Release.ps1') -ManagedStateRoot $root -GpuUuid {ps_quote(self.gpu_uuid)} -QualifiedPowerLimitW {self.lane.qualified_power_limit_w} -OwnerPowerLimitW {self.lane.owner_power_limit_w})
$v=[string]$lines[-1] | ConvertFrom-Json
if([string]$v.status -cne 'passed'){{throw 'security failed'}}
$out=Join-Path $stage 'evidence\\state-security.json'
[IO.File]::WriteAllText($out,($v|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
[Console]::Out.WriteLine(([ordered]@{{status='passed';root_dacl_protected=[bool]$v.root_dacl_protected;low_privilege_read_denials=[int]$v.low_privilege_effective_read_denials;sha256=(Get-FileHash $out -Algorithm SHA256).Hash.ToLowerInvariant();security=$v}}|ConvertTo-Json -Depth 16 -Compress))
"""
        return self.remote_json(self.config.target, script, timeout=1200)

    def omp(self) -> dict[str, Any]:
        script = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.state_root)}
$stage={ps_quote(self.target_root)}
$ompRoot={ps_quote(self.config.omp_root)}
$controller=Join-Path $root 'Control-Release.ps1'
&$controller -Action Start -StateRoot $root|Out-Null
$s=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
$r=$s.releases.PSObject.Properties[[string]$s.active_release].Value
$env:LOCALAPPDATA=Join-Path $ompRoot 'localappdata'
$env:PI_CODING_AGENT_DIR=Join-Path $ompRoot 'agent'
$env:NINFER_ACCEPTANCE_API_KEY=(Get-Content ([string]$r.api_key_file) -Raw).Trim()
$env:NO_COLOR='1'
$events=Join-Path $stage 'evidence\\omp-events.jsonl'
$clientVersion=(&(Join-Path $env:LOCALAPPDATA 'OMP\\omp.cmd') --version|Out-String).Trim()
if($LASTEXITCODE -ne 0 -or -not $clientVersion){{throw 'OMP version evidence is missing'}}
$args=@('--mode','json','--print','--no-session','--no-title','--no-extensions','--no-skills','--no-rules','--no-lsp','--no-pty','--no-tools','--tools','read','--auto-approve','--approval-mode','yolo','--model',('ninfer-client-acceptance/'+{ps_quote(self.lane.model_id)}),'--thinking','off','--system-prompt','Use the read tool exactly once on marker.txt, then return exactly its single line with no other text.','--cwd',(Join-Path $ompRoot 'workspace'),'--max-time','300','Read marker.txt with the read tool and return its exact single line.')
&(Join-Path $env:LOCALAPPDATA 'OMP\\omp.cmd') @args 1>$events
if($LASTEXITCODE -ne 0){{throw 'OMP failed'}}
$parsed=@(Get-Content $events|Where-Object{{$_.Trim()}}|ForEach-Object{{$_|ConvertFrom-Json}})
$ended=@($parsed|Where-Object{{$_.type -eq 'message_end' -and $null -ne $_.message}}|ForEach-Object{{$_.message}})
$assistants=@($ended|Where-Object{{$_.role -eq 'assistant'}});$results=@($ended|Where-Object{{$_.role -eq 'toolResult'}})
$calls=@($assistants|ForEach-Object{{@($_.content|Where-Object{{$_.type -eq 'toolCall'}})}})
$text=[string]::Join('',@($assistants[-1].content|Where-Object{{$_.type -eq 'text'}}|ForEach-Object{{[string]$_.text}}))
if($calls.Count -ne 1 -or $calls[0].name -cne 'read' -or $results.Count -lt 1 -or $text -cne 'OMP_NINFER_WINDOWS_C12_OK'){{throw 'OMP exact oracle failed'}}
[Console]::Out.WriteLine(([ordered]@{{status='passed';client_version=$clientVersion;events=$parsed.Count;typed_tool_name='read';tool_results=$results.Count;exact_final_answer=$true}}|ConvertTo-Json -Compress))
"""
        return self.remote_json(self.config.target, script, timeout=900)

    def benchmark(self) -> dict[str, Any]:
        arguments: list[str] = []
        if self.lane.managed_power_envelope_w is not None:
            arguments += ["--power-envelope-w", str(self.lane.managed_power_envelope_w)]
        return self.target_internal("_benchmark", *arguments, timeout=1800)

    def receipt(self) -> dict[str, Any]:
        evidence = {
            name: {
                "status": self.state["phases"][name]["status"],
                "receipt_sha256": self.state["phases"][name]["receipt_sha256"],
            }
            for name in PHASES
            if name in self.state["phases"]
            and name not in {"preflight", "receipt", "restore"}
        }
        context = self.state["phases"]["context_128k"]["receipt"]
        restart = self.state["phases"]["restart"]["receipt"]
        benchmark = self.state["phases"]["benchmark_c1"]["receipt"]
        package_receipt = self.state["phases"]["package"]["receipt"]
        build = self.state["phases"]["neutral_build"]["receipt"]
        summary = {
            "artifact_type": "ninfer_native_qualification_summary",
            "schema_version": 1,
            "gpu_uuid": self.gpu_uuid,
            "lane": self.lane.name,
            "release_id": self.lane.release_id,
            "deployment_profile": self.lane.spec["deployment_profile"],
            "cuda_architecture": self.lane.spec["gpu"]["cuda_architecture"],
            "status": "restore-pending",
            "qualified_utc": None,
            "restoration": {"status": "required"},
            "source_commit": self.head,
            "qualification_tool_commit": self.tool_head,
            "package": package_receipt["package"],
            "build_profile": package_receipt["build_profile"],
            "upstream_base_sha": package_receipt["upstream_base_sha"],
            "lineage_base_sha": package_receipt["lineage_base_sha"],
            "model_sha256": package_receipt["model_sha256"],
            "server_binary_sha256": package_receipt["binaries"]["server_sha256"],
            "configuration_sha256": package_receipt["config_sha256"],
            "support_assets": package_receipt["support_assets"],
            "runtime_dlls": build.get("runtime_dlls", []),
            "evidence": evidence,
            "observed": {
                "context_prompt_tokens": context["prompt_tokens"],
                "context_exact_output": context["exact_output"],
                "restart_cached_input_tokens": restart["cached_input_tokens"],
                "restart_process_replaced": restart["old_pid"] != restart["new_pid"],
                "managed_stop_mode": restart["managed_stop"]["mode"],
                "managed_stop_outcome": restart["managed_stop"]["outcome"],
                "managed_stop_forced": restart["managed_stop"]["forced"],
                "unsaved_session_survived_managed_stop": restart["managed_stop_flush"]["exact_output"] == FLUSH_MARKER,
                "unsaved_session_cached_input_tokens": restart["managed_stop_flush"]["cached_input_tokens"],
                "c1_decode_tokens_per_second": benchmark["decode_tokens_per_second"],
                "c1_prefill_tokens_per_second": benchmark["prefill_tokens_per_second"],
                "c1_max_power_w": benchmark["max_power_w"],
                "c1_max_memory_used_mib": benchmark["max_memory_used_mib"],
                "c1_mtp_acceptance_percent": benchmark["mtp_acceptance_percent"],
                "context_elapsed_seconds": context["elapsed_seconds"],
                "protocol_checks": self.state["phases"]["protocol"]["receipt"]["checks"],
                "rollback": self.state["phases"]["rollback"]["receipt"]["status"],
                "rollback_predecessor_stop_mode": self.state["phases"]["rollback"]["receipt"].get("predecessor_stop_mode"),
                "security_low_privilege_read_denials": self.state["phases"]["security"]["receipt"]["low_privilege_read_denials"],
                "omp_exact_final_answer": self.state["phases"]["omp"]["receipt"]["exact_final_answer"],
            },
            "automatic_route_activation_allowed": False,
            "stable_promotion_performed": False,
            "production_route_activation_performed": False,
        }
        atomic_json(self.config.state_dir / "qualification-summary.json", summary)
        summary["receipt_sha256"] = sha256_file(self.config.state_dir / "qualification-summary.json")
        return summary

    def _record_summary_restoration(self, restoration: dict[str, Any]) -> None:
        path = self.config.state_dir / "qualification-summary.json"
        if not path.is_file():
            return
        summary = json.loads(path.read_text(encoding="utf-8"))
        checks_passed = all(
            self.state["phases"].get(name, {}).get("status") == "passed"
            for name in PHASES if name != "restore"
        )
        if restoration["status"] == "passed":
            summary["status"] = "passed" if checks_passed else "failed"
        else:
            summary["status"] = "restore-failed" if restoration["status"] == "failed" else "restore-pending"
        summary["qualified_utc"] = (
            time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()) if summary["status"] == "passed" else None
        )
        public_restoration = dict(restoration)
        detail = public_restoration.pop("error", None)
        if detail is not None:
            public_restoration["reason"] = "operator_failure"
            public_restoration["error_sha256"] = hashlib.sha256(str(detail).encode()).hexdigest()
        summary["restoration"] = public_restoration
        atomic_json(path, summary)
        summary["receipt_sha256"] = sha256_file(path)
        receipt_phase = self.state["phases"].get("receipt")
        if receipt_phase and "receipt" in receipt_phase:
            # Keep the phase receipt and its hash consistent with the finalized summary.
            receipt_path = self.receipt_dir / "receipt.json"
            atomic_json(receipt_path, summary)
            receipt_phase["receipt"] = summary
            receipt_phase["receipt_sha256"] = sha256_file(receipt_path)

    def restore(self) -> dict[str, Any]:
        self._record_summary_restoration({"status": "required"})
        found = self.state.get("found_incumbent", {})
        incumbent = found.get("incumbent_state_root", "")
        restart_incumbent = bool(found.get("incumbent_was_running")) and bool(incumbent)
        status = self.remote_json(
            self.config.target,
            f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.state_root)}
$taskName={ps_quote(self.task_name)}
$controller=Join-Path $root 'Control-Release.ps1'
if(Test-Path (Join-Path $root 'state.json')){{
  $bound=Get-Content (Join-Path $root 'state.json') -Raw|ConvertFrom-Json
  if([string]$bound.task_name -cne $taskName){{throw 'refusing restoration through a foreign lifecycle task'}}
}}
$scoped=(Test-Path $controller) -and (Test-ScopedControllerOrQuiescent $root)
if($scoped){{&$controller -Action Stop -StateRoot $root|Out-Null}}
$task=Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if($null -ne $task -and $task.State -eq 'Running'){{throw 'managed wrapper did not finish restoration'}}
$leaseActive=$false
if($scoped){{$status=&$controller -Action Status -StateRoot $root|ConvertFrom-Json;if([string]$status.gpu_owner.status -cne 'ok'){{throw 'GPU-owner restoration status failed'}};$leaseActive=([bool]$status.gpu_owner.lease_active -or [bool]$status.gpu_owner.current_paused)}}
$limit=[int][double](nvidia-smi.exe -i {ps_quote(self.gpu_uuid)} --query-gpu=power.limit --format=csv,noheader,nounits)
if($limit -ne {self.lane.owner_power_limit_w}){{throw '{self.lane.owner_power_limit_w} W owner state was not restored'}}
if($leaseActive){{throw 'GPU-owner lease remained active'}}
$incumbentRoot={ps_quote(incumbent)}
$incumbentState='untouched'
if({'$true' if restart_incumbent else '$false'}){{if(-not (Test-ScopedControllerOrQuiescent $incumbentRoot)){{throw 'refusing to restart legacy unscoped incumbent'}};&(Join-Path $incumbentRoot 'Control-Release.ps1') -Action Start -StateRoot $incumbentRoot|Out-Null;$incumbentState='running'}}
$taskState=if($null -eq (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue)){{'absent'}}else{{[string](Get-ScheduledTask -TaskName $taskName).State}}
$owned=@(Get-CimInstance Win32_Process -Filter "Name='ninfer-serve.exe'"|Where-Object{{
  $_.ExecutablePath -and ([string]$_.ExecutablePath).StartsWith(([IO.Path]::GetFullPath($root).TrimEnd('\\')+'\\'),[StringComparison]::OrdinalIgnoreCase)
}})
[Console]::Out.WriteLine(([ordered]@{{status='passed';task_state=$taskState;power_limit_w=$limit;gpu_lease_active=$leaseActive;incumbent_state=$incumbentState;process_count=$owned.Count}}|ConvertTo-Json -Compress))
""",
            timeout=900,
        )
        if status["process_count"] != 0:
            raise LaneError("scratch-owned process remained after restoration")
        self.state["restoration"] = status
        self._record_summary_restoration(status)
        return status

    def cleanup(self) -> None:
        try:
            receipt = self.restore()
            self.state["restoration"] = receipt
        except Exception as error:
            self.state["restoration"] = {"status": "failed", "error": str(error)}
            self._record_summary_restoration(self.state["restoration"])
        self._save()

    def execute(self) -> dict[str, Any]:
        if self.config.dry_run:
            return {
                "status": "dry_run",
                "lane": self.lane.name,
                "release_id": self.lane.release_id,
                "package_name": self.lane.package_name,
                "state_root": self.state_root,
                "source_commit": self.head,
                "phases": list(PHASES),
                "builder_root": self.builder_root,
                "target_root": self.target_root,
            }
        actions: tuple[tuple[str, Callable[[], dict[str, Any]]], ...] = (
            ("preflight", self.preflight),
            ("neutral_build", self.neutral_build),
            ("private_path_scan", self.private_scan),
            ("package", self.package),
            ("transfer_install", self.transfer_install),
            ("protocol", self.protocol),
            ("pressure_protocol", self.pressure_protocol),
            ("context_128k", self.context),
            ("restart", self.restart),
            ("rollback", self.rollback),
            ("security", self.security),
            ("omp", self.omp),
            ("benchmark_c1", self.benchmark),
            ("receipt", self.receipt),
            ("restore", self.restore),
        )
        completed_all = False
        try:
            for name, action in actions:
                self.phase(name, action)
                if self.config.through_phase == name:
                    break
            else:
                completed_all = True
        finally:
            restored = self.state["phases"].get("restore", {}).get("status") == "passed"
            if not restored:
                self.cleanup()
        self.state["status"] = "passed" if completed_all else "paused"
        if completed_all:
            try:
                lane_receipt = stage_lane_qualification(
                    self.state, json.loads((self.config.state_dir / "qualification-summary.json").read_text(encoding="utf-8")),
                    self.lane,
                )
                atomic_json(self.config.state_dir / f"{self.lane.name}.json", lane_receipt)
            except Exception:
                self.state["status"] = "failed"
                self._save()
                raise
        self._save()
        return self.state


def internal_verify_native_build(args: argparse.Namespace) -> dict[str, Any]:
    sys.path.insert(0, str(Path(args.source_root)))
    from tools.release import package as release_package

    root = Path(args.runtime_bin)
    programs = ("ninfer", "ninfer-serve", "ninfer_bench")
    identities = [release_package.parse_build_info(root / f"{name}.exe", name) for name in programs]
    if any(identity != identities[0] for identity in identities[1:]):
        raise LaneError("qualified native executables carry different build identities")
    identity = identities[0]
    for key, expected in {
        "patch_stack_sha": args.source_commit, "upstream_base_sha": args.upstream_base_sha,
        "build_profile": args.build_profile, "cuda_architecture": args.cuda_architecture,
        "source_dirty": "false", "build_type": "Release",
    }.items():
        if identity[key] != expected:
            raise LaneError(f"qualified native build identity mismatch: {key}")
    build_receipt = Path(args.build_receipt)
    if not isinstance(json.loads(build_receipt.read_text(encoding="utf-8-sig")), dict):
        raise LaneError("integrator build receipt is not a JSON object")
    files = [
        {"name": path.name, "bytes": path.stat().st_size, "sha256": sha256_file(path)}
        for path in sorted(root.iterdir(), key=lambda path: path.name)
        if path.is_file() and (path.suffix.lower() == ".dll" or path.name in {f"{name}.exe" for name in programs})
    ]
    runtime = [item for item in files if item["name"].lower().endswith(".dll")]
    if not set(DIRECTSTORAGE_DLLS).issubset({item["name"].lower() for item in runtime}):
        raise LaneError("qualified native runtime omitted DirectStorage redistributables")
    return {
        "status": "passed", "evidence_class": "integrator-qualified-native-input-copy",
        "build_tests_rerun": False, "qualified_build_receipt_sha256": sha256_file(build_receipt),
        "source_commit": args.source_commit, "cuda_architecture": args.cuda_architecture,
        "build_profile": args.build_profile, "identity": identity, "runtime_dlls": runtime, "files": files,
    }


def internal_long(args: argparse.Namespace) -> dict[str, Any]:
    evidence = Path(args.evidence_root)
    evidence.mkdir(parents=True, exist_ok=True)
    state = json.loads((Path(args.state_root) / "state.json").read_text(encoding="utf-8"))
    release = state["releases"][state["active_release"]]
    controller = Path(args.state_root) / "Control-Release.ps1"
    run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(controller), "-Action", "Start", "-StateRoot", args.state_root], timeout=600)
    key = read_key(Path(release["api_key_file"]))
    messages = json.loads(Path(args.fixture).read_text(encoding="utf-8"))
    started = time.perf_counter()
    result = request_json(
        f"http://{release['host']}:{release['port']}", key, "POST", "/v1/chat/completions",
        {"model": args.model_id, "messages": messages, "max_completion_tokens": 128, "temperature": 0, "reasoning_effort": "none"},
    )
    usage = result["usage"]
    content = result["choices"][0]["message"]["content"].strip()
    if usage["prompt_tokens"] != LONG_PROMPT_TOKENS or content != EXPECTED_LONG:
        raise LaneError("128K retrieval was not exact")
    receipt = {"status": "passed", "prompt_tokens": LONG_PROMPT_TOKENS, "completion_tokens": usage["completion_tokens"], "exact_output": content, "elapsed_seconds": time.perf_counter() - started, "fixture_sha256": sha256_file(Path(args.fixture))}
    atomic_json(evidence / "long-context-128k.json", receipt)
    return receipt


def controller_status(controller: Path, state_root: Path, action: str, timeout: int = 900) -> dict[str, Any]:
    """Runs a controller action and returns the lifecycle status object it prints."""
    text = run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(controller),
         "-Action", action, "-StateRoot", str(state_root)],
        timeout=timeout,
    ).stdout
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end <= start:
        raise LaneError(f"controller -Action {action} printed no status object")
    return json.loads(text[start:end + 1])


def checkpoint_state(base: str, key: str, session: str) -> str:
    try:
        status = request_json(base, key, "GET", f"/v1/ninfer/checkpoints/{session}/status")
        if (status.get("artifact_type") != "ninfer_session_checkpoint_status" or
                status.get("schema_version") != 3):
            raise LaneError("checkpoint status does not describe the new schema-3 store")
        return str(status.get("state"))
    except urllib.error.HTTPError as error:
        return f"http_{error.code}"


def internal_restart(args: argparse.Namespace) -> dict[str, Any]:
    evidence = Path(args.evidence_root)
    state_root = Path(args.state_root)
    state = json.loads((state_root / "state.json").read_text(encoding="utf-8"))
    release = state["releases"][state["active_release"]]
    controller = state_root / "Control-Release.ps1"
    run(
        [
            "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            str(controller), "-Action", "Start", "-StateRoot", str(state_root),
        ],
        timeout=900,
    )
    key = read_key(Path(release["api_key_file"]))
    base = f"http://{release['host']}:{release['port']}"
    listener = ["powershell", "-NoProfile", "-Command",
                f"(Get-NetTCPConnection -State Listen -LocalPort {release['port']}).OwningProcess"]

    def seed(session: str, marker: str, label: str) -> dict[str, Any]:
        return request_json(base, key, "POST", "/v1/responses", {"model": args.model_id, "input": f"Memorize this exact marker for the next turn: {marker}. Reply only SAVED.", "max_output_tokens": 32, "temperature": 0, "reasoning": {"effort": "none"}, "store": True, "ninfer_session": session, "ninfer_request_id": digest(label)})

    def resume(session: str, previous: str, label: str) -> tuple[str, int]:
        value = request_json(base, key, "POST", "/v1/responses", {"model": args.model_id, "input": "Return only the exact marker from the previous turn.", "max_output_tokens": 64, "temperature": 0, "reasoning": {"effort": "none"}, "store": True, "ninfer_session": session, "previous_response_id": previous, "ninfer_request_id": digest(label)})
        return response_text(value).strip(), int(value["usage"]["input_tokens_details"]["cached_tokens"])

    # Two sessions, both far below the 32,768-token automatic gate. The control is published the
    # documented explicit way; the second is never saved at all, so only the managed stop's
    # shutdown flush can carry it across the restart.
    control_session = digest("checkpoint-session")
    flush_session = digest("managed-stop-session")
    control_seed = seed(control_session, CHECKPOINT_MARKER, "seed")
    saved = request_json(base, key, "POST", "/v1/ninfer/checkpoints", {"session_sha256": control_session})
    require_native_save_receipt(saved)
    if checkpoint_state(base, key, control_session) != "available":
        raise LaneError("published session checkpoint did not become available")
    # The proof is that nothing published this session, and this cache root outlives the phase:
    # a generation an earlier run left behind is exactly the state the precondition denies. Own
    # it rather than assume it, so the phase is as re-runnable as the rest of the lane.
    try:
        request_json(base, key, "DELETE", f"/v1/ninfer/checkpoints/{flush_session}")
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
    flush_seed = seed(flush_session, FLUSH_MARKER, "flush-seed")
    flush_before = checkpoint_state(base, key, flush_session)
    if flush_before == "available":
        raise LaneError("the managed-stop session was already published before the stop")

    old_pid = int(run(listener).stdout.strip())
    restart_started = time.perf_counter()
    status = controller_status(controller, state_root, "Restart")
    restart_seconds = time.perf_counter() - restart_started
    new_pid = int(run(listener).stdout.strip())
    stop = status.get("last_stop") or {}
    if stop.get("mode") != "stop-event" or stop.get("outcome") != "graceful" or stop.get("forced"):
        raise LaneError(f"the managed stop was not a graceful signal: {json.dumps(stop)}")
    restarted = request_json(base, key, "GET", "/v1/ninfer/status")
    if (restarted.get("schema_version") != 2 or
            restarted.get("scheduler", {}).get("committed_decode_tokens") != 0 or
            restarted.get("scheduler", {}).get("computed_prefill_tokens") != 0 or
            restarted.get("cache", {}).get("reused_prompt_tokens") != 0):
        raise LaneError("restart did not reset the real scheduler and cache counters")

    control_answer, control_cached = resume(control_session, control_seed["id"], "continue")
    flush_answer, flush_cached = resume(flush_session, flush_seed["id"], "flush-continue")
    if old_pid == new_pid:
        raise LaneError("the managed restart did not replace the server process")
    if control_answer != CHECKPOINT_MARKER or control_cached <= 0:
        raise LaneError("explicitly published session did not resume across the restart")
    if flush_answer != FLUSH_MARKER or flush_cached <= 0:
        raise LaneError("the managed stop did not flush the unsaved session")

    cache = Path(release["cache_root"]) / "session-checkpoints-v3" / "sessions"
    directories = sorted(cache.iterdir(), key=lambda path: path.stat().st_mtime, reverse=True)
    files = [path for path in directories[0].rglob("*") if path.is_file()]
    receipt = {
        "status": "passed",
        "old_pid": old_pid,
        "new_pid": new_pid,
        "restart_seconds": restart_seconds,
        "status_schema_version": 2,
        "restart_counters_reset": True,
        "managed_stop": {
            "mode": stop.get("mode"),
            "outcome": stop.get("outcome"),
            "forced": bool(stop.get("forced")),
            "exit_code": stop.get("exit_code"),
            "elapsed_seconds": stop.get("elapsed_seconds"),
            "graceful_wait_seconds": stop.get("graceful_wait_seconds"),
        },
        "explicit_control": {
            "checkpoint_save": "explicit_endpoint",
            "save_bytes": saved["payload_bytes"],
            "frontier_tokens": saved["frontier_tokens"],
            "cached_input_tokens": control_cached,
            "exact_output": control_answer,
        },
        "managed_stop_flush": {
            "checkpoint_save": "shutdown_flush",
            "state_before_stop": flush_before,
            "state_after_restart": checkpoint_state(base, key, flush_session),
            "cached_input_tokens": flush_cached,
            "exact_output": flush_answer,
        },
        "checkpoint_files": len(files),
        "checkpoint_bytes": sum(path.stat().st_size for path in files),
        "cached_input_tokens": control_cached,
        "exact_output": control_answer,
        "deletion_semantics": "logical-object-deletion",
        "secure_erasure_claimed": False,
    }
    atomic_json(evidence / "checkpoint-restart-proof.json", receipt)
    return receipt


def gpu_metrics(gpu_uuid: str) -> tuple[float, int, int, int]:
    rows = run(["nvidia-smi", "-i", gpu_uuid, "--query-gpu=power.draw,temperature.gpu,utilization.gpu,memory.used", "--format=csv,noheader,nounits"]).stdout.strip().splitlines()
    if len(rows) != 1:
        raise LaneError("selected GPU metrics returned more or fewer than one row")
    output = rows[0]
    values = [item.strip() for item in output.split(",")]
    return float(values[0]), int(values[1]), int(values[2]), int(values[3])


def internal_benchmark(args: argparse.Namespace) -> dict[str, Any]:
    evidence = Path(args.evidence_root)
    state_root = Path(args.state_root)
    state = json.loads((state_root / "state.json").read_text(encoding="utf-8"))
    release = state["releases"][state["active_release"]]
    if args.gpu_uuid and release["gpu_uuid"] != args.gpu_uuid:
        raise LaneError("benchmark release GPU differs from qualification target")
    controller = state_root / "Control-Release.ps1"
    run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(controller), "-Action", "Start", "-StateRoot", str(state_root)], timeout=900)
    key = read_key(Path(release["api_key_file"]))
    base = f"http://{release['host']}:{release['port']}"
    request_json(base, key, "POST", "/v1/chat/completions", {"model": args.model_id, "messages": [{"role": "user", "content": "Warm the managed path."}], "max_tokens": 32, "temperature": 0})
    log = Path(release["release_root"]) / "logs" / "requests.jsonl"
    before = len(log.read_text(encoding="utf-8").splitlines())
    paragraph = "A reliable GPU inference service separates admission control, scheduling, memory accounting, observability, and failure recovery. "
    prompt = (paragraph * (28000 // len(paragraph) + 2))[:28000]
    maximums = list(gpu_metrics(release["gpu_uuid"]))
    stop = threading.Event()
    poll_errors: list[Exception] = []
    def poll() -> None:
        try:
            while not stop.wait(0.1):
                values = gpu_metrics(release["gpu_uuid"])
                for index, value in enumerate(values): maximums[index] = max(maximums[index], value)
        except Exception as error:
            poll_errors.append(error)
            stop.set()
    thread = threading.Thread(target=poll, daemon=True); thread.start()
    started = time.perf_counter()
    try:
        request_json(base, key, "POST", "/v1/chat/completions", {"model": args.model_id, "messages": [{"role": "user", "content": prompt + "\nContinue until the output limit."}], "max_tokens": 1024, "temperature": 0, "seed": 57004, "reasoning_effort": "medium"})
    finally:
        stop.set()
        thread.join()
    wall = time.perf_counter() - started
    if poll_errors:
        raise LaneError("selected GPU metrics failed during the benchmark") from poll_errors[0]
    records = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()[before:]]
    done = [record for record in records if record.get("event") == "request_done"]
    if len(done) != 1 or done[0]["result"]["completion_tokens"] != 1024:
        raise LaneError("managed C1 did not produce exactly 1024 tokens")
    if (done[0].get("artifact_type") != "ninfer_serve_request_log" or
            done[0].get("schema_version") != 30):
        raise LaneError("managed C1 request log is not schema 30")
    record = done[0]; result = record["result"]; timing = record["timings_seconds"]; spec = record["speculative"]
    receipt = {"status": "passed", "completion_tokens": 1024, "prompt_tokens": result["prompt_tokens"], "computed_prefill_tokens": result["computed_prefill_tokens"], "decode_tokens_per_second": 1024 / timing["decode"], "prefill_tokens_per_second": result["computed_prefill_tokens"] / timing["prefill"], "wall_seconds": wall, "max_power_w": maximums[0], "max_temperature_c": maximums[1], "max_gpu_utilization_percent": maximums[2], "max_memory_used_mib": maximums[3], "mtp_acceptance_percent": 100 * spec["accepted_tokens"] / spec["drafted_tokens"]}
    if args.power_envelope_w is not None:
        receipt["power_envelope_w"] = args.power_envelope_w
        if receipt["max_power_w"] > args.power_envelope_w:
            raise LaneError(f"managed C1 exceeded the {args.power_envelope_w:g} W qualification envelope")
    receipt["gpu_uuid"] = release["gpu_uuid"]
    atomic_json(evidence / "managed-c1.json", receipt)
    return receipt


def internal_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--path", type=Path)
    parser.add_argument("--state-root")
    parser.add_argument("--evidence-root")
    parser.add_argument("--fixture")
    parser.add_argument("--model-id", default="q38-ninfer")
    parser.add_argument("--power-envelope-w", type=float)
    parser.add_argument("--gpu-uuid", type=gpu_uuid_argument)
    parser.add_argument("--source-root")
    parser.add_argument("--runtime-bin")
    parser.add_argument("--build-receipt")
    parser.add_argument("--source-commit")
    parser.add_argument("--cuda-architecture")
    parser.add_argument("--build-profile")
    parser.add_argument("--upstream-base-sha")
    return parser


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1].startswith("_"):
        phase = sys.argv[1]
        args = internal_parser().parse_args(sys.argv[2:])
        if phase == "_scan-dir": result = scan_directory(args.path)
        elif phase == "_scan-archive": result = scan_archive(args.path)
        elif phase == "_long": result = internal_long(args)
        elif phase == "_restart": result = internal_restart(args)
        elif phase == "_benchmark": result = internal_benchmark(args)
        elif phase == "_verify-native-build": result = internal_verify_native_build(args)
        else: raise LaneError(f"unknown internal phase: {phase}")
        print(json.dumps(result, sort_keys=True))
        return 0

    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--lane", required=True, help="lane directory name under packaging/windows/lanes")
    parser.add_argument("--builder", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--gpu-uuid", type=gpu_uuid_argument,
                        help="target GPU UUID; otherwise require exactly one lane-qualified card")
    parser.add_argument("--builder-vcpkg", required=True, help="vcpkg.cmake toolchain file on the builder")
    parser.add_argument("--builder-cuda-compiler", required=True, help="nvcc.exe on the builder")
    parser.add_argument("--builder-cxx-compiler", required=True, help="cl.exe on the builder")
    parser.add_argument("--model-path", required=True)
    parser.add_argument("--dependency-sbom-tool", required=True, help="P4 generator path on the builder")
    parser.add_argument("--vcpkg-resolved", required=True, help="resolved vcpkg inventory on the builder")
    parser.add_argument("--cuda-inventory", required=True, help="packaged CUDA DLL inventory on the builder")
    parser.add_argument("--third-party-inventory", required=True, help="vendored source inventory on the builder")
    parser.add_argument("--runtime-inventory", required=True, help="genuine CRT/DirectStorage inventory on the builder")
    parser.add_argument("--scratch-state-root", required=True, help="protected non-product root, with the lane's root leaf")
    parser.add_argument("--scratch-task-suffix", required=True, help="isolated task suffix, e.g. I6 (at most 26 safe characters)")
    parser.add_argument("--scratch-port", required=True, type=int)
    parser.add_argument("--pressure-port", required=True, type=int)
    parser.add_argument("--api-key-file", required=True, help="protected target-side operator key; never discovered in shipped roots")
    parser.add_argument("--qualified-runtime-bin", help="consume this exact integrator-qualified builder-side flat runtime; do not rebuild")
    parser.add_argument("--qualified-build-receipt", help="builder-side integrator receipt binding the qualified runtime input")
    parser.add_argument("--long-fixture", type=Path, required=True)
    parser.add_argument("--omp-root", required=True)
    parser.add_argument("--transfer-tool", type=Path,
                        help="ranged-HTTP transfer script with serve/fetch subcommands (omp-ninfer "
                             "scripts/hosts/pscp.py); required when the builder and target differ")
    parser.add_argument("--incumbent-state-root",
                        help="another lineage's state root on the target, stopped for the window and "
                             "restarted afterwards when it was running")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--through-phase", choices=PHASES)
    parser.add_argument("--candidate-source")
    parser.add_argument("--upgrade-orchestrator-state", action="store_true")
    args = parser.parse_args()
    config = Config(
        source_root=args.source_root.resolve(), state_dir=args.state_dir.resolve(), lane=args.lane,
        builder=args.builder, target=args.target, builder_vcpkg=args.builder_vcpkg,
        builder_cuda_compiler=args.builder_cuda_compiler, builder_cxx_compiler=args.builder_cxx_compiler,
        model_path=args.model_path, long_fixture=args.long_fixture.resolve(), omp_root=args.omp_root,
        transfer_tool=args.transfer_tool.resolve() if args.transfer_tool else None,
        incumbent_state_root=args.incumbent_state_root, resume=args.resume, dry_run=args.dry_run,
        through_phase=args.through_phase,
        candidate_source=args.candidate_source,
        upgrade_orchestrator_state=args.upgrade_orchestrator_state,
        dependency_sbom_tool=args.dependency_sbom_tool,
        vcpkg_resolved=args.vcpkg_resolved,
        cuda_inventory=args.cuda_inventory,
        third_party_inventory=args.third_party_inventory,
        runtime_inventory=args.runtime_inventory,
        scratch_state_root=args.scratch_state_root,
        scratch_task_suffix=args.scratch_task_suffix,
        scratch_port=args.scratch_port,
        pressure_port=args.pressure_port,
        api_key_file=args.api_key_file,
        qualified_runtime_bin=args.qualified_runtime_bin,
        qualified_build_receipt=args.qualified_build_receipt,
        gpu_uuid=args.gpu_uuid,
    )
    result = Orchestrator(config).execute()
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (LaneError, OSError, subprocess.SubprocessError, urllib.error.URLError) as error:
        print(json.dumps({"status": "failed", "error": str(error)}, sort_keys=True), file=sys.stderr)
        raise SystemExit(1)
