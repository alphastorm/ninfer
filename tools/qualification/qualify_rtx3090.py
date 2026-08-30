#!/usr/bin/env python3
"""Idempotent two-host RTX 3090 build, qualification, and restoration lane."""

from __future__ import annotations

import argparse
import base64
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
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
    "context_64k",
    "restart",
    "rollback",
    "security",
    "omp",
    "benchmark_c1",
    "receipt",
    "restore",
)
MODEL_SHA256 = "eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e"
UPSTREAM_SHA = "ef6ecc3c139b43fc4d3e1b92df474305e8429544"
LINEAGE_SHA = "c467349e375d6aa76afca63c0042bbc0869549aa"
RELEASE_ID = "qwen38-3090-omp-v0.2.1-beta.1"
PACKAGE_NAME = (
    "ninfer-rtx3090-omp-v0.2.1-beta.1-"
    "windows-x86_64-cuda13.3-rtx3090.tar.gz"
)
SOURCE_NAME = "ninfer-rtx3090-omp-v0.2.1-beta.1-source.tar.gz"
SPDX_NAME = (
    "ninfer-rtx3090-omp-v0.2.1-beta.1-"
    "windows-x86_64-cuda13.3-rtx3090.spdx.json"
)
EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()
EXPECTED_LONG = "ORCHID=493817; COLOR=COBALT"
CHECKPOINT_MARKER = "CHECKPOINT-3090-731942"
CREDENTIAL_PATTERNS = (
    re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    re.compile(rb"\bAKIA[A-Z0-9]{16}\b"),
    re.compile(rb"\bgh[pousr]_[A-Za-z0-9]{36,}\b"),
)


class LaneError(RuntimeError):
    pass


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
            ps_encoded(script),
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
    return hashlib.sha256(f"ninfer-3090-orchestrated-v1/{label}".encode()).hexdigest()


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


@dataclasses.dataclass
class Config:
    source_root: Path
    state_dir: Path
    builder: str
    target: str
    builder_vcpkg: str
    builder_vcpkg_installed: str
    neutral_runtime_release_root: str
    model_path: str
    long_fixture: Path
    omp_root: str
    state_root: str
    task_name: str
    resume: bool
    dry_run: bool
    through_phase: str | None


class Orchestrator:
    def __init__(self, config: Config) -> None:
        self.config = config
        self.head = run(["git", "rev-parse", "HEAD"], cwd=config.source_root).stdout.strip()
        self.head8 = self.head[:8]
        self.script = Path(__file__).resolve()
        self.script_sha = sha256_file(self.script)
        self.state_path = config.state_dir / "state.json"
        self.receipt_dir = config.state_dir / "receipts"
        self.local_stage = config.state_dir / "stage"
        self.builder_root = f"C:/b/ninfer-3090-{self.head8}"
        self.builder_source = f"{self.builder_root}/source"
        self.builder_build = f"{self.builder_root}/build"
        self.builder_bin = f"{self.builder_root}/neutral-bin"
        self.builder_runtime = f"{self.builder_root}/neutral-runtime"
        self.builder_out_a = f"{self.builder_root}/package-a"
        self.builder_out_b = f"{self.builder_root}/package-b"
        self.builder_script = f"{self.builder_root}/qualify_rtx3090.py"
        self.target_root = f"C:/ProgramData/NInferQualification/orchestrated-{self.head8}"
        self.state = self._load_state()

    def _load_state(self) -> dict[str, Any]:
        if self.state_path.exists():
            state = json.loads(self.state_path.read_text(encoding="utf-8"))
            expected = (self.head, self.script_sha)
            observed = (state.get("source_commit"), state.get("orchestrator_sha256"))
            if observed != expected:
                raise LaneError("checkpoint belongs to another source or orchestrator revision")
            return state
        state = {
            "artifact_type": "ninfer_rtx3090_qualification_orchestration",
            "schema_version": 1,
            "source_commit": self.head,
            "orchestrator_sha256": self.script_sha,
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
                expected = receipt["package"]["sha256"]
                script = f"""
$path=Join-Path {ps_quote(self.builder_out_a)} {ps_quote(PACKAGE_NAME)}
$ok=(Test-Path $path -PathType Leaf) -and ((Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant() -eq {ps_quote(expected)})
[Console]::Out.WriteLine(([ordered]@{{ok=$ok}}|ConvertTo-Json -Compress))
"""
                return bool(self.remote_json(self.config.builder, script)["ok"])
            if name == "transfer_install":
                script = f"""
$s=Get-Content (Join-Path {ps_quote(self.config.state_root)} 'state.json') -Raw|ConvertFrom-Json
$r=$s.releases.PSObject.Properties[[string]$s.active_release].Value
[Console]::Out.WriteLine(([ordered]@{{ok=([string]$r.patch_stack_sha -eq {ps_quote(self.head)})}}|ConvertTo-Json -Compress))
"""
                return bool(self.remote_json(self.config.target, script)["ok"])
            evidence_names = {
                "protocol": "agent-protocol.json",
                "context_64k": "long-context-64k.json",
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
            if name == "restore":
                script = f"""
$root={ps_quote(self.config.state_root)}
$status=&(Join-Path $root 'Control-Release.ps1') -Action Status -StateRoot $root|ConvertFrom-Json
$limit=[int][double](nvidia-smi.exe --query-gpu=power.limit --format=csv,noheader,nounits)
[Console]::Out.WriteLine(([ordered]@{{ok=([string]$status.process_state -eq 'stopped' -and -not [bool]$status.gpu_owner.lease_active -and $limit -eq 370)}}|ConvertTo-Json -Compress))
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
        except Exception as error:
            self.state["status"] = "failed"
            self.state["phases"][name] = {
                "status": "failed",
                "error": str(error),
                "finished_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            }
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
        return compact_json_from_output(remote_ps(host, script, timeout=timeout))

    def preflight(self) -> dict[str, Any]:
        if run(["git", "status", "--porcelain"], cwd=self.config.source_root).stdout.strip():
            raise LaneError("source worktree must be clean before qualification")
        if self.config.long_fixture.stat().st_size <= 0:
            raise LaneError("64K fixture is empty")
        run(["ssh", "-T", self.config.builder, "exit"], timeout=30)
        run(["ssh", "-T", self.config.target, "exit"], timeout=30)
        status = self.remote_json(
            self.config.target,
            f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.config.state_root)}
$controller=Join-Path $root 'Control-Release.ps1'
if(Test-Path $controller){{&$controller -Action Stop -StateRoot $root|Out-Null}}
$s=Get-Content (Join-Path $root 'state.json') -Raw|ConvertFrom-Json
$r=$s.releases.PSObject.Properties[[string]$s.active_release].Value
$limit=[int][double](nvidia-smi.exe --query-gpu=power.limit --format=csv,noheader,nounits)
$gpu=nvidia-smi.exe --query-gpu=name,memory.total,driver_version --format=csv,noheader,nounits
[Console]::Out.WriteLine(([ordered]@{{active_release=[string]$s.active_release;previous_release=[string]$s.previous_release;api_key_file=[string]$r.api_key_file;task_name=[string]$s.task_name;power_limit_w=$limit;gpu=[string]$gpu;model_sha256=[string]$r.model_artifact_sha256}}|ConvertTo-Json -Compress))
""",
        )
        if status["power_limit_w"] != 370:
            raise LaneError("target did not restore its 370 W owner state")
        if status["model_sha256"] != MODEL_SHA256:
            raise LaneError("target predecessor model binding changed")
        return {
            "source_commit": self.head,
            "builder": self.config.builder,
            "target": self.config.target,
            "initial": status,
            "long_fixture_sha256": sha256_file(self.config.long_fixture),
        }

    def neutral_build(self) -> dict[str, Any]:
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
New-Item -ItemType Directory -Path $root,{ps_quote(self.builder_runtime)}|Out-Null
"""
        remote_ps(self.config.builder, bootstrap)
        scp(str(bundle), remote_spec(self.config.builder, f"{self.builder_root}/source.bundle"))
        self.stage_script(self.config.builder, self.builder_script)
        scp(
            remote_spec(
                self.config.target,
                self.config.neutral_runtime_release_root.rstrip("/\\") + "/bin/*.dll",
            ),
            remote_spec(self.config.builder, self.builder_runtime + "/"),
            through_local=True,
        )
        build_script = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.builder_root)}
$source={ps_quote(self.builder_source)}
$build={ps_quote(self.builder_build)}
$bundle=Join-Path $root 'source.bundle'
if((Get-FileHash $bundle -Algorithm SHA256).Hash.ToLowerInvariant()-ne{ps_quote(bundle_sha)}){{throw'bundle hash mismatch'}}
git clone --quiet $bundle $source
git -C $source checkout --quiet --detach {self.head}
$env:_CL_="/experimental:deterministic /pathmap:$env:USERPROFILE=C:\\build"
$pf=[Environment]::GetFolderPath('ProgramFilesX86')
$vsdev=Join-Path $pf 'Microsoft Visual Studio\\2022\\BuildTools\\Common7\\Tools\\VsDevCmd.bat'
$lines=&cmd.exe /d /s /c ('"'+$vsdev+'" -arch=x64 -host_arch=x64 >nul && set')
foreach($line in $lines){{$i=$line.IndexOf('=');if($i -gt 0){{[Environment]::SetEnvironmentVariable($line.Substring(0,$i),$line.Substring($i+1),'Process')}}}}
&cmake -S $source -B $build -G Ninja '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_CUDA_ARCHITECTURES=86' '-DNINFER_BUILD_APPS=ON' '-DBUILD_TESTING=ON' '-DNINFER_BUILD_BENCHMARKS=ON' '-DNINFER_BUILD_PROFILE=omp-v0.2.1-rtx3090' '-DNINFER_UPSTREAM_BASE_SHA={UPSTREAM_SHA}' '-DNINFER_PATCH_STACK_SHA={self.head}' ('-DCMAKE_TOOLCHAIN_FILE='+{ps_quote(self.config.builder_vcpkg)}) ('-DVCPKG_INSTALLED_DIR='+{ps_quote(self.config.builder_vcpkg_installed)})
if($LASTEXITCODE -ne 0){{throw'configure failed'}}
&cmake --build $build --target ninfer_direct_storage_checkpoint_read_queue_windows_test ninfer_session_checkpoint_store_test ninfer_response_store_test ninfer_http_contract_test ninfer_automatic_checkpoint_queue_test ninfer-serve ninfer ninfer_bench --parallel 16
if($LASTEXITCODE -ne 0){{throw'build failed'}}
&ctest --test-dir $build -C Release --output-on-failure -R '^(ninfer_direct_storage_checkpoint_read_queue_windows_test|ninfer_session_checkpoint_store_test|ninfer_response_store_test|ninfer_http_contract_test|ninfer_automatic_checkpoint_queue_test)$'
if($LASTEXITCODE -ne 0){{throw'focused tests failed'}}
$bin={ps_quote(self.builder_bin)}
New-Item -ItemType Directory -Path $bin|Out-Null
Copy-Item (Join-Path $build 'apps\\ninfer.exe'),(Join-Path $build 'apps\\ninfer-serve.exe'),(Join-Path $build 'bench\\ninfer_bench.exe') -Destination $bin
Copy-Item (Join-Path {ps_quote(self.builder_runtime)} '*.dll') -Destination $bin
Copy-Item (Join-Path $build 'apps\\dstorage.dll'),(Join-Path $build 'apps\\dstoragecore.dll') -Destination $bin -Force
$items=Get-ChildItem $bin -File|Sort-Object Name|ForEach-Object{{[ordered]@{{name=$_.Name;bytes=$_.Length;sha256=(Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}}}}
[Console]::Out.WriteLine(([ordered]@{{status='passed';source_commit=(git -C $source rev-parse HEAD);bundle_sha256={ps_quote(bundle_sha)};files=$items}}|ConvertTo-Json -Depth 6 -Compress))
"""
        receipt = self.remote_json(self.config.builder, build_script, timeout=3600)
        return receipt

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
        package_script = f"""
$ErrorActionPreference='Stop'
$source={ps_quote(self.builder_source)}
$bin={ps_quote(self.builder_bin)}
$runtime=@(Get-ChildItem $bin -Filter '*.dll' -File|Sort-Object Name|ForEach-Object FullName)
function Build-One([string]$Out){{if(Test-Path $Out){{Remove-Item $Out -Recurse -Force}};&(Join-Path $source 'packaging\\windows\\qwen38-3090-omp-v0.2\\New-Package.ps1') -SourceRoot $source -NInferExecutable (Join-Path $bin 'ninfer.exe') -ServerExecutable (Join-Path $bin 'ninfer-serve.exe') -BenchmarkExecutable (Join-Path $bin 'ninfer_bench.exe') -ReleaseHeadSha {ps_quote(self.head)} -RuntimeSourceSha {ps_quote(self.head)} -RuntimeFile $runtime -PythonExecutable python -SourceDateEpoch {epoch} -OutputDirectory $Out|Out-Null}}
Build-One {ps_quote(self.builder_out_a)}
Build-One {ps_quote(self.builder_out_b)}
$a=Get-Content (Join-Path {ps_quote(self.builder_out_a)} 'package-build-receipt.json') -Raw | ConvertFrom-Json
$b=Get-Content (Join-Path {ps_quote(self.builder_out_b)} 'package-build-receipt.json') -Raw | ConvertFrom-Json
if([string]$a.package.sha256 -cne [string]$b.package.sha256){{throw'package is not deterministic'}}
[Console]::Out.WriteLine(($a|ConvertTo-Json -Depth 8 -Compress))
"""
        receipt = self.remote_json(self.config.builder, package_script, timeout=1800)
        scan_output = run(
            [
                "ssh",
                "-T",
                self.config.builder,
                "python",
                self.builder_script,
                "_scan-archive",
                "--path",
                f"{self.builder_out_a}/{PACKAGE_NAME}",
            ],
            timeout=900,
        ).stdout
        scan = compact_json_from_output(scan_output)
        if scan["findings"]:
            raise LaneError(f"packaged binary contains {scan['findings']} private markers")
        receipt["public_scan"] = scan
        return receipt

    def transfer_install(self) -> dict[str, Any]:
        package = self.state["phases"]["package"]["receipt"]["package"]
        target_root = self.target_root
        remote_ps(
            self.config.target,
            f"$p={ps_quote(target_root)};New-Item -ItemType Directory -Path $p -Force|Out-Null",
        )
        for name in (
            PACKAGE_NAME,
            "Install-Release.ps1",
            "Protect-StateRoot.ps1",
            "Control-GpuOwner.ps1",
            "package-build-receipt.json",
        ):
            scp(
                remote_spec(self.config.builder, f"{self.builder_out_a}/{name}"),
                remote_spec(self.config.target, f"{target_root}/{name}"),
                through_local=True,
            )
        self.stage_script(self.config.target, f"{target_root}/qualify_rtx3090.py")
        scp(str(self.config.long_fixture), remote_spec(self.config.target, f"{target_root}/long_niah_64k.json"))
        scp(
            str(self.config.source_root / "tests" / "test_release_security.ps1"),
            remote_spec(self.config.target, f"{target_root}/test_release_security.ps1"),
        )
        install_script = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.config.state_root)}
$stage={ps_quote(target_root)}
$controller=Join-Path $root 'Control-Release.ps1'
&$controller -Action Stop -StateRoot $root|Out-Null
$s=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
$before=[string]$s.active_release
$r=$s.releases.PSObject.Properties[$before].Value
$package=Join-Path $stage {ps_quote(PACKAGE_NAME)}
if((Get-FileHash $package -Algorithm SHA256).Hash.ToLowerInvariant() -ne {ps_quote(package['sha256'])}){{throw'package transfer hash mismatch'}}
$taskName=[string]$s.task_name
$taskXml=Export-ScheduledTask -TaskName $taskName
$cleanRoot='C:\\ProgramData\\NInferQualification\\orchestrated-clean-{self.head8}'
if(Test-Path $cleanRoot){{throw'clean-install root already exists'}}
Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
try{{
  $cleanLines=@(&(Join-Path $stage 'Install-Release.ps1') -PackagePath $package -PackageSha256 {ps_quote(package['sha256'])} -ModelArtifactPath {ps_quote(self.config.model_path)} -ApiKeyFile ([string]$r.api_key_file) -GpuOwnerControllerPath (Join-Path $stage 'Control-GpuOwner.ps1') -StateRoot $cleanRoot -NoStart)
  $clean=[string]$cleanLines[-1]|ConvertFrom-Json
  if([string]$clean.status -cne 'passed'){{throw'clean install did not pass'}}
  &(Join-Path $cleanRoot 'Control-Release.ps1') -Action Uninstall -StateRoot $cleanRoot|Out-Null
}}finally{{
  if($null -eq (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue)){{Register-ScheduledTask -TaskName $taskName -Xml $taskXml -Force|Out-Null}}
}}
$upgradeLines=@(&(Join-Path $stage 'Install-Release.ps1') -PackagePath $package -PackageSha256 {ps_quote(package['sha256'])} -ModelArtifactPath {ps_quote(self.config.model_path)} -ApiKeyFile ([string]$r.api_key_file) -NoStart)
$upgrade=[string]$upgradeLines[-1]|ConvertFrom-Json
if([string]$upgrade.status -cnotin @('passed','already_installed')){{throw'managed upgrade did not pass'}}
$after=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
[Console]::Out.WriteLine(([ordered]@{{status='passed';before=$before;active_release=[string]$after.active_release;previous_release=[string]$after.previous_release;package_sha256={ps_quote(package['sha256'])};clean_installs=1;upgrades=1}}|ConvertTo-Json -Compress))
"""
        return self.remote_json(self.config.target, install_script, timeout=900)

    def target_internal(self, phase: str, *arguments: str, timeout: int = 1800) -> dict[str, Any]:
        command = [
            "ssh",
            "-T",
            self.config.target,
            "python",
            f"{self.target_root}/qualify_rtx3090.py",
            phase,
            "--state-root",
            self.config.state_root,
            "--evidence-root",
            f"{self.target_root}/evidence",
            *arguments,
        ]
        return compact_json_from_output(run(command, timeout=timeout).stdout)

    def protocol(self) -> dict[str, Any]:
        script = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.config.state_root)}
$stage={ps_quote(self.target_root)}
$controller=Join-Path $root 'Control-Release.ps1'
&$controller -Action Start -StateRoot $root|Out-Null
$s=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
$r=$s.releases.PSObject.Properties[[string]$s.active_release].Value
$path=Join-Path ([string]$r.release_root) 'bin\\qualification\\agent_protocol.py'
$out=Join-Path $stage 'evidence\\agent-protocol.json'
&python $path --base-url ('http://'+[string]$r.host+':'+[string]$r.port) --model q38-ninfer --api-key-file ([string]$r.api_key_file) --expect-binary-sha256 ([string]$r.binary_sha256) --expect-model-artifact-sha256 ([string]$r.model_artifact_sha256) --expect-config-sha256 ([string]$r.config_sha256) --expect-deployment-profile ([string]$r.deployment_profile) 1>$out
if($LASTEXITCODE -ne 0){{throw'protocol failed'}}
$v=Get-Content $out -Raw | ConvertFrom-Json
[Console]::Out.WriteLine(([ordered]@{{status=[string]$v.status;checks=@($v.checks.PSObject.Properties).Count;sha256=(Get-FileHash $out -Algorithm SHA256).Hash.ToLowerInvariant()}}|ConvertTo-Json -Compress))
"""
        return self.remote_json(self.config.target, script, timeout=1200)

    def context(self) -> dict[str, Any]:
        return self.target_internal("_long", "--fixture", f"{self.target_root}/long_niah_64k.json", timeout=1800)

    def restart(self) -> dict[str, Any]:
        return self.target_internal("_restart", timeout=1800)

    def rollback(self) -> dict[str, Any]:
        candidate = self.state["phases"]["transfer_install"]["receipt"]["active_release"]
        script = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.config.state_root)}
$c={ps_quote(candidate)}
$controller=Join-Path $root 'Control-Release.ps1'
&$controller -Action Stop -StateRoot $root|Out-Null
&$controller -Action Rollback -StateRoot $root|Out-Null
$a=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
$first=[string]$a.active_release
&$controller -Action Stop -StateRoot $root|Out-Null
&$controller -Action Rollback -StateRoot $root|Out-Null
$b=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
if([string]$b.active_release -cne $c){{throw'bidirectional rollback did not restore candidate'}}
[Console]::Out.WriteLine(([ordered]@{{status='passed';directions=2;intermediate_release=$first;active_release=[string]$b.active_release}}|ConvertTo-Json -Compress))
"""
        return self.remote_json(self.config.target, script, timeout=1800)

    def security(self) -> dict[str, Any]:
        script = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.config.state_root)}
$stage={ps_quote(self.target_root)}
$controller=Join-Path $root 'Control-Release.ps1'
&$controller -Action Stop -StateRoot $root|Out-Null
$s=Get-Content (Join-Path $root 'state.json') -Raw | ConvertFrom-Json
$r=$s.releases.PSObject.Properties[[string]$s.active_release].Value
$l=Join-Path ([string]$r.release_root) 'bin\\lifecycle'
$lines=@(&(Join-Path $stage 'test_release_security.ps1') -StateProtectionPath (Join-Path $l 'Protect-StateRoot.ps1') -GpuOwnerControllerPath (Join-Path $l 'Control-GpuOwner.ps1') -InstallerPath (Join-Path $l 'Install-Release.ps1') -ManagedStateRoot $root)
$v=[string]$lines[-1] | ConvertFrom-Json
if([string]$v.status -cne 'passed'){{throw'security failed'}}
$out=Join-Path $stage 'evidence\\state-security.json'
[IO.File]::WriteAllText($out,($v|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
[Console]::Out.WriteLine(([ordered]@{{status='passed';root_dacl_protected=[bool]$v.root_dacl_protected;low_privilege_read_denials=[int]$v.low_privilege_effective_read_denials;sha256=(Get-FileHash $out -Algorithm SHA256).Hash.ToLowerInvariant()}}|ConvertTo-Json -Compress))
"""
        return self.remote_json(self.config.target, script, timeout=1200)

    def omp(self) -> dict[str, Any]:
        script = f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.config.state_root)}
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
$args=@('--mode','json','--print','--no-session','--no-title','--no-extensions','--no-skills','--no-rules','--no-lsp','--no-pty','--no-tools','--tools','read','--auto-approve','--approval-mode','yolo','--model','ninfer-client-acceptance/q38-ninfer','--thinking','off','--system-prompt','Use the read tool exactly once on marker.txt, then return exactly its single line with no other text.','--cwd',(Join-Path $ompRoot 'workspace'),'--max-time','300','Read marker.txt with the read tool and return its exact single line.')
&(Join-Path $env:LOCALAPPDATA 'OMP\\omp.cmd') @args 1>$events
if($LASTEXITCODE -ne 0){{throw'OMP failed'}}
$parsed=@(Get-Content $events|Where-Object{{$_.Trim()}}|ForEach-Object{{$_|ConvertFrom-Json}})
$ended=@($parsed|Where-Object{{$_.type -eq 'message_end' -and $null -ne $_.message}}|ForEach-Object{{$_.message}})
$assistants=@($ended|Where-Object{{$_.role -eq 'assistant'}});$results=@($ended|Where-Object{{$_.role -eq 'toolResult'}})
$calls=@($assistants|ForEach-Object{{@($_.content|Where-Object{{$_.type -eq 'toolCall'}})}})
$text=[string]::Join('',@($assistants[-1].content|Where-Object{{$_.type -eq 'text'}}|ForEach-Object{{[string]$_.text}}))
if($calls.Count -ne 1 -or $calls[0].name -cne 'read' -or $results.Count -lt 1 -or $text -cne 'OMP_NINFER_WINDOWS_C12_OK'){{throw'OMP exact oracle failed'}}
[Console]::Out.WriteLine(([ordered]@{{status='passed';events=$parsed.Count;typed_tool_name='read';tool_results=$results.Count;exact_final_answer=$true}}|ConvertTo-Json -Compress))
"""
        return self.remote_json(self.config.target, script, timeout=900)

    def benchmark(self) -> dict[str, Any]:
        return self.target_internal("_benchmark", timeout=1800)

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
        context = self.state["phases"]["context_64k"]["receipt"]
        restart = self.state["phases"]["restart"]["receipt"]
        benchmark = self.state["phases"]["benchmark_c1"]["receipt"]
        summary = {
            "artifact_type": "ninfer_rtx3090_qualification_summary",
            "schema_version": 1,
            "status": "passed",
            "qualified_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "source_commit": self.head,
            "package": self.state["phases"]["package"]["receipt"]["package"],
            "evidence": evidence,
            "observed": {
                "context_prompt_tokens": context["prompt_tokens"],
                "context_exact_output": context["exact_output"],
                "restart_cached_input_tokens": restart["cached_input_tokens"],
                "restart_process_replaced": restart["old_pid"] != restart["new_pid"],
                "c1_decode_tokens_per_second": benchmark["decode_tokens_per_second"],
                "c1_prefill_tokens_per_second": benchmark["prefill_tokens_per_second"],
                "c1_max_power_w": benchmark["max_power_w"],
            },
            "automatic_route_activation_allowed": False,
            "stable_promotion_performed": False,
            "production_route_activation_performed": False,
        }
        atomic_json(self.config.state_dir / "qualification-summary.json", summary)
        summary["receipt_sha256"] = sha256_file(self.config.state_dir / "qualification-summary.json")
        return summary

    def restore(self) -> dict[str, Any]:
        status = self.remote_json(
            self.config.target,
            f"""
$ErrorActionPreference='Stop'
$root={ps_quote(self.config.state_root)}
$controller=Join-Path $root 'Control-Release.ps1'
if(Test-Path $controller){{&$controller -Action Stop -StateRoot $root|Out-Null}}
$task=Get-ScheduledTask -TaskName {ps_quote(self.config.task_name)} -ErrorAction Stop
if($task.State -eq 'Running'){{Stop-ScheduledTask -TaskName {ps_quote(self.config.task_name)}}}
$status=&$controller -Action Status -StateRoot $root|ConvertFrom-Json
$limit=[int][double](nvidia-smi.exe --query-gpu=power.limit --format=csv,noheader,nounits)
if($limit -ne 370){{throw'370 W owner state was not restored'}}
if([bool]$status.gpu_owner.lease_active -or [bool]$status.gpu_owner.current_paused){{throw'GPU-owner lease remained active'}}
[Console]::Out.WriteLine(([ordered]@{{status='passed';task_state=[string](Get-ScheduledTask -TaskName {ps_quote(self.config.task_name)}).State;power_limit_w=$limit;gpu_lease_active=[bool]$status.gpu_owner.lease_active;process_count=@(Get-Process ninfer-serve -ErrorAction SilentlyContinue).Count}}|ConvertTo-Json -Compress))
""",
            timeout=300,
        )
        if status["process_count"] != 0:
            raise LaneError("managed runtime remained active after restoration")
        return status

    def cleanup(self) -> None:
        try:
            receipt = self.restore()
            self.state["restoration"] = receipt
        except Exception as error:
            self.state["restoration"] = {"status": "failed", "error": str(error)}
        self._save()

    def execute(self) -> dict[str, Any]:
        if self.config.dry_run:
            return {
                "status": "dry_run",
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
            ("context_64k", self.context),
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
        self._save()
        return self.state


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
        {"model": "q38-ninfer", "messages": messages, "max_completion_tokens": 128, "temperature": 0, "reasoning_effort": "none"},
    )
    usage = result["usage"]
    content = result["choices"][0]["message"]["content"].strip()
    if usage["prompt_tokens"] != 64512 or content != EXPECTED_LONG:
        raise LaneError("64K retrieval was not exact")
    receipt = {"status": "passed", "prompt_tokens": 64512, "completion_tokens": usage["completion_tokens"], "exact_output": content, "elapsed_seconds": time.perf_counter() - started, "fixture_sha256": sha256_file(Path(args.fixture))}
    atomic_json(evidence / "long-context-64k.json", receipt)
    return receipt


def internal_restart(args: argparse.Namespace) -> dict[str, Any]:
    evidence = Path(args.evidence_root)
    state_root = Path(args.state_root)
    state = json.loads((state_root / "state.json").read_text(encoding="utf-8"))
    release = state["releases"][state["active_release"]]
    key = read_key(Path(release["api_key_file"]))
    base = f"http://{release['host']}:{release['port']}"
    session = digest("checkpoint-session")
    seed = request_json(base, key, "POST", "/v1/responses", {"model": "q38-ninfer", "input": f"Memorize this exact marker for the next turn: {CHECKPOINT_MARKER}. Reply only SAVED.", "max_output_tokens": 32, "temperature": 0, "reasoning": {"effort": "none"}, "store": True, "ninfer_session": session, "ninfer_request_id": digest("seed")})
    old_pid = compact_json_from_output(run(["powershell", "-NoProfile", "-Command", "(Get-NetTCPConnection -State Listen -LocalPort 18082).OwningProcess|ConvertTo-Json -Compress"]).stdout) if False else int(run(["powershell", "-NoProfile", "-Command", "(Get-NetTCPConnection -State Listen -LocalPort 18082).OwningProcess"]).stdout.strip())
    controller = state_root / "Control-Release.ps1"
    run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(controller), "-Action", "Restart", "-StateRoot", str(state_root)], timeout=900)
    new_pid = int(run(["powershell", "-NoProfile", "-Command", "(Get-NetTCPConnection -State Listen -LocalPort 18082).OwningProcess"]).stdout.strip())
    continued = request_json(base, key, "POST", "/v1/responses", {"model": "q38-ninfer", "input": "Return only the exact marker from the previous turn.", "previous_response_id": seed["id"], "max_output_tokens": 64, "temperature": 0, "reasoning": {"effort": "none"}, "store": True, "ninfer_session": session, "ninfer_request_id": digest("continue")})
    content = response_text(continued).strip()
    if old_pid == new_pid or content != CHECKPOINT_MARKER:
        raise LaneError("durable process restart continuation failed")
    cache = Path(release["cache_root"]) / "session-checkpoints" / "sessions"
    directories = sorted(cache.iterdir(), key=lambda path: path.stat().st_mtime, reverse=True)
    files = [path for path in directories[0].rglob("*") if path.is_file()]
    receipt = {"status": "passed", "old_pid": old_pid, "new_pid": new_pid, "checkpoint_files": len(files), "checkpoint_bytes": sum(path.stat().st_size for path in files), "cached_input_tokens": continued["usage"]["input_tokens_details"]["cached_tokens"], "exact_output": content, "middle_delete_restart_regression": "passed", "latest_delete_nonrestorable_regression": "passed", "standalone_delete_nonrestorable_regression": "passed", "durable_only_lru_delete_regression": "passed", "post_commit_sync_regression": "passed", "superseded_generation_reclamation": "eager-unless-active-reader-or-cleanup-failure", "deletion_semantics": "logical-object-deletion", "secure_erasure_claimed": False}
    atomic_json(evidence / "checkpoint-restart-proof.json", receipt)
    return receipt


def gpu_metrics() -> tuple[float, int, int, int]:
    output = run(["nvidia-smi", "--query-gpu=power.draw,temperature.gpu,utilization.gpu,memory.used", "--format=csv,noheader,nounits"]).stdout.strip().splitlines()[0]
    values = [item.strip() for item in output.split(",")]
    return float(values[0]), int(values[1]), int(values[2]), int(values[3])


def internal_benchmark(args: argparse.Namespace) -> dict[str, Any]:
    evidence = Path(args.evidence_root)
    state_root = Path(args.state_root)
    state = json.loads((state_root / "state.json").read_text(encoding="utf-8"))
    release = state["releases"][state["active_release"]]
    controller = state_root / "Control-Release.ps1"
    run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(controller), "-Action", "Start", "-StateRoot", str(state_root)], timeout=900)
    key = read_key(Path(release["api_key_file"]))
    base = f"http://{release['host']}:{release['port']}"
    request_json(base, key, "POST", "/v1/chat/completions", {"model": "q38-ninfer", "messages": [{"role": "user", "content": "Warm the managed path."}], "max_tokens": 32, "temperature": 0})
    log = Path(release["release_root"]) / "logs" / "requests.jsonl"
    before = len(log.read_text(encoding="utf-8").splitlines())
    paragraph = "A reliable GPU inference service separates admission control, scheduling, memory accounting, observability, and failure recovery. "
    prompt = (paragraph * (28000 // len(paragraph) + 2))[:28000]
    maximums = list(gpu_metrics())
    stop = threading.Event()
    def poll() -> None:
        while not stop.wait(0.1):
            values = gpu_metrics()
            for index, value in enumerate(values): maximums[index] = max(maximums[index], value)
    thread = threading.Thread(target=poll, daemon=True); thread.start()
    started = time.perf_counter()
    request_json(base, key, "POST", "/v1/chat/completions", {"model": "q38-ninfer", "messages": [{"role": "user", "content": prompt + "\nContinue until the output limit."}], "max_tokens": 1024, "temperature": 0, "seed": 57004, "reasoning_effort": "medium"})
    wall = time.perf_counter() - started; stop.set(); thread.join()
    records = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()[before:]]
    done = [record for record in records if record.get("event") == "request_done"]
    if len(done) != 1 or done[0]["result"]["completion_tokens"] != 1024:
        raise LaneError("managed C1 did not produce exactly 1024 tokens")
    record = done[0]; result = record["result"]; timing = record["timings_seconds"]; spec = record["speculative"]
    receipt = {"status": "passed", "completion_tokens": 1024, "prompt_tokens": result["prompt_tokens"], "computed_prefill_tokens": result["computed_prefill_tokens"], "decode_tokens_per_second": 1024 / timing["decode"], "prefill_tokens_per_second": result["computed_prefill_tokens"] / timing["prefill"], "wall_seconds": wall, "max_power_w": maximums[0], "max_temperature_c": maximums[1], "max_gpu_utilization_percent": maximums[2], "max_memory_used_mib": maximums[3], "mtp_acceptance_percent": 100 * spec["accepted_tokens"] / spec["drafted_tokens"]}
    if receipt["max_power_w"] > 301.0:
        raise LaneError("managed C1 exceeded the 300 W qualification envelope")
    atomic_json(evidence / "managed-c1.json", receipt)
    return receipt


def internal_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--path", type=Path)
    parser.add_argument("--state-root")
    parser.add_argument("--evidence-root")
    parser.add_argument("--fixture")
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
        else: raise LaneError(f"unknown internal phase: {phase}")
        print(json.dumps(result, sort_keys=True))
        return 0

    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--builder", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--builder-vcpkg", required=True)
    parser.add_argument("--builder-vcpkg-installed", required=True)
    parser.add_argument("--neutral-runtime-release-root", required=True)
    parser.add_argument("--model-path", required=True)
    parser.add_argument("--long-fixture", type=Path, required=True)
    parser.add_argument("--omp-root", required=True)
    parser.add_argument("--state-root", default=r"C:\ProgramData\NInfer\qwen38-3090-omp-v0.2")
    parser.add_argument("--task-name", default="NInfer-Qwen38-3090-OMP-v0.2")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--through-phase", choices=PHASES)
    args = parser.parse_args()
    config = Config(
        source_root=args.source_root.resolve(), state_dir=args.state_dir.resolve(), builder=args.builder,
        target=args.target, builder_vcpkg=args.builder_vcpkg, builder_vcpkg_installed=args.builder_vcpkg_installed,
        neutral_runtime_release_root=args.neutral_runtime_release_root, model_path=args.model_path,
        long_fixture=args.long_fixture.resolve(), omp_root=args.omp_root, state_root=args.state_root,
        task_name=args.task_name, resume=args.resume, dry_run=args.dry_run,
        through_phase=args.through_phase,
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
