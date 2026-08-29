[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$StateRoot,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ControllerPath,

    [Parameter(Mandatory = $true)]
    [string]$ReceiptPath,

    [int]$FileCount = 2048,

    [int]$LogicalMiBPerFile = 1,

    [double]$MaximumStatusSeconds = 30
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$StateRoot = (Resolve-Path -LiteralPath $StateRoot).Path
$ControllerPath = (Resolve-Path -LiteralPath $ControllerPath).Path
if ($FileCount -lt 1024 -or $LogicalMiBPerFile -lt 1 -or $MaximumStatusSeconds -le 0) {
    throw 'populated-state measurement bounds are invalid'
}

if (-not ('NInferSparseStatusProbe' -as [type])) {
    Add-Type @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class NInferSparseStatusProbe {
    private const uint FsctlSetSparse = 0x000900c4;
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(
        SafeFileHandle device, uint code, IntPtr input, int inputBytes,
        IntPtr output, int outputBytes, out int returned, IntPtr overlapped);

    public static void Create(string path, long length) {
        using (FileStream stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write,
                                                   FileShare.None, 1, FileOptions.WriteThrough)) {
            int returned;
            if (!DeviceIoControl(stream.SafeFileHandle, FsctlSetSparse, IntPtr.Zero, 0,
                                 IntPtr.Zero, 0, out returned, IntPtr.Zero)) {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            stream.SetLength(length);
            stream.Flush(true);
        }
    }
}
'@
}

$state = Get-Content -LiteralPath (Join-Path $StateRoot 'state.json') -Raw | ConvertFrom-Json
$release = $state.releases.PSObject.Properties[[string]$state.active_release].Value
$probeRoot = Join-Path ([string]$release.cache_root) 'responses\status-walk-probe'
if (Test-Path -LiteralPath $probeRoot) { throw 'status-walk probe root already exists' }
$logicalBytes = [Int64]$FileCount * [Int64]$LogicalMiBPerFile * 1MB
$elapsed = $null
try {
    New-Item -ItemType Directory -Path $probeRoot -Force | Out-Null
    $fileBytes = [Int64]$LogicalMiBPerFile * 1MB
    for ($index = 0; $index -lt $FileCount; $index++) {
        [NInferSparseStatusProbe]::Create(
            (Join-Path $probeRoot ("checkpoint-{0:D6}.bin" -f $index)),
            $fileBytes
        )
    }
    $files = @(Get-ChildItem -LiteralPath $probeRoot -File)
    $observedLogicalBytes = [Int64](($files | Measure-Object Length -Sum).Sum)
    if ($files.Count -ne $FileCount -or $observedLogicalBytes -ne $logicalBytes) {
        throw 'populated-state logical quota fixture is incomplete'
    }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $status = (((& $ControllerPath -Action Status -StateRoot $StateRoot) | Out-String).Trim() |
        ConvertFrom-Json)
    $watch.Stop()
    $elapsed = $watch.Elapsed.TotalSeconds
    if ([string]$status.endpoint_state -cne 'ready' -or $elapsed -gt $MaximumStatusSeconds) {
        throw "populated-root status exceeded its bound or lost readiness: $elapsed seconds"
    }
    $receipt = [ordered]@{
        artifact_type = 'ninfer_populated_state_status_timing'
        schema_version = 1
        status = 'passed'
        source_commit = [string]$status.server.identity.patch_stack_sha
        binary_sha256 = [string]$status.server.identity.binary_sha256
        populated_file_count = $files.Count
        populated_logical_bytes = $observedLogicalBytes
        configured_checkpoint_quota_mib = 8192
        measurement_scope = 'fixed synthetic 2048-file sparse-root status regression'
        fixture_storage = 'sparse files with logical byte lengths; not realized checkpoint generations'
        worst_case_quota_file_count_claim = $false
        quota_full_generation_topology_measured = $false
        status_elapsed_seconds = $elapsed
        maximum_status_seconds = $MaximumStatusSeconds
        endpoint_state = [string]$status.endpoint_state
        raw_state_or_requests_included = $false
    }
    [IO.File]::WriteAllText($ReceiptPath, ($receipt | ConvertTo-Json -Depth 8),
        [Text.UTF8Encoding]::new($false))
    $receipt | ConvertTo-Json -Compress
}
finally {
    Remove-Item -LiteralPath $probeRoot -Recurse -Force -ErrorAction SilentlyContinue
}
