[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ControllerPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ControllerPath = (Resolve-Path -LiteralPath $ControllerPath).Path
$principal = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent()
)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { exit 77 }
$root = Join-Path $env:ProgramData ('NInferGpuOwnerRegression-' + [Guid]::NewGuid().ToString('N'))
$fakeDirectory = Join-Path $env:TEMP ('ninfer-fake-smi-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fakeDirectory | Out-Null
$fakeNvidiaSmi = Join-Path $fakeDirectory 'nvidia-smi.exe'
Add-Type -TypeDefinition @'
using System;
public static class NInferFakeNvidiaSmi {
    public static int Main() {
        Console.Write(Environment.GetEnvironmentVariable("NINFER_TEST_GPU_PIDS") ?? "");
        return 0;
    }
}
'@ -OutputAssembly $fakeNvidiaSmi -OutputType ConsoleApplication
$previousPath = $env:PATH
$previousPids = $env:NINFER_TEST_GPU_PIDS
$env:PATH = "$fakeDirectory;$previousPath"
$env:NINFER_TEST_GPU_PIDS = ''

function Read-Status {
    return (((& $ControllerPath -Action status -StateRoot $root) | Out-String).Trim() | ConvertFrom-Json)
}
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

try {
    $initial = Read-Status
    Assert-True ([string]$initial.artifact_type -ceq 'ninfer_generic_gpu_owner_status') 'generic status envelope mismatch'
    Assert-True ([bool]$initial.paused) 'idle host did not start paused'
    & $ControllerPath -Action stop -StateRoot $root | Out-Null
    Assert-True ([bool](Read-Status).paused) 'generic stop did not remain paused'

    $env:NINFER_TEST_GPU_PIDS = "4242`n"
    $activeStatus = Read-Status
    Assert-True (-not [bool]$activeStatus.paused) 'status reported an active compute owner as released'
    Assert-True ([int]$activeStatus.active_compute_owner_count -eq 1) 'status lost the active compute-owner count'
    $activeRejected = $false
    try { & $ControllerPath -Action stop -StateRoot $root | Out-Null }
    catch { $activeRejected = $_.Exception.Message -like '*unmanaged GPU compute owner is active*' }
    Assert-True $activeRejected 'state-present stop did not re-check the active compute owner'

    $env:NINFER_TEST_GPU_PIDS = ''
    & $ControllerPath -Action stop -StateRoot $root | Out-Null
    & $ControllerPath -Action start -StateRoot $root | Out-Null
    Assert-True ([bool](Read-Status).paused) 'idle generic start manufactured an external workload'
    [ordered]@{
        artifact_type = 'ninfer_generic_gpu_owner_regression'
        schema_version = 1
        status = 'passed'
        actions = @('status', 'stop', 'start')
        default_mode = 'idle-host'
        state_present_active_compute_rejections = 1
        shipped_test_bypass = $false
    } | ConvertTo-Json -Compress
}
finally {
    $env:PATH = $previousPath
    $env:NINFER_TEST_GPU_PIDS = $previousPids
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $fakeDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
