[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ControllerPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ControllerPath = (Resolve-Path -LiteralPath $ControllerPath).Path
$root = Join-Path ([IO.Path]::GetTempPath()) ('ninfer-gpu-owner-' + [Guid]::NewGuid().ToString('N'))
$previousMode = $env:NINFER_GPU_OWNER_TEST_MODE
$env:NINFER_GPU_OWNER_TEST_MODE = 'idle'

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
    & $ControllerPath -Action start -StateRoot $root | Out-Null
    Assert-True ([bool](Read-Status).paused) 'idle generic start manufactured an external workload'
    [ordered]@{
        artifact_type = 'ninfer_generic_gpu_owner_regression'
        schema_version = 1
        status = 'passed'
        actions = @('status', 'stop', 'start')
        default_mode = 'idle-host'
    } | ConvertTo-Json -Compress
}
finally {
    $env:NINFER_GPU_OWNER_TEST_MODE = $previousMode
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
