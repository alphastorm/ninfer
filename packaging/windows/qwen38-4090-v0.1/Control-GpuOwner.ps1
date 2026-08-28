[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('status', 'stop', 'start')]
    [string]$Action,

    [string]$StateRoot = (Join-Path $env:ProgramData 'NInfer\gpu-owner')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$statePath = Join-Path $StateRoot 'state.json'

function Write-State([object]$Value) {
    New-Item -ItemType Directory -Force -Path $StateRoot | Out-Null
    $temporary = "$statePath.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText(
            $temporary,
            ($Value | ConvertTo-Json -Depth 8),
            [Text.UTF8Encoding]::new($false)
        )
        Move-Item -LiteralPath $temporary -Destination $statePath -Force
    }
    finally { Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue }
}

function Read-State {
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
        $testMode = [string]$env:NINFER_GPU_OWNER_TEST_MODE -ceq 'idle'
        $pids = @()
        if (-not $testMode) {
            $nvidiaSmi = (Get-Command nvidia-smi.exe -ErrorAction Stop).Source
            $rows = @(& $nvidiaSmi --query-compute-apps=pid --format=csv,noheader,nounits 2>$null)
            if ($LASTEXITCODE -ne 0) { throw 'failed to inspect GPU compute owners' }
            $pids = @($rows | ForEach-Object { ([string]$_).Trim() } | Where-Object { $_ -match '^\d+$' })
        }
        if ($pids.Count -ne 0) {
            throw 'unmanaged GPU compute owner is active; provide an operator-specific GPU-owner controller'
        }
        $initial = [ordered]@{
            artifact_type = 'ninfer_generic_gpu_owner_state'
            schema_version = 1
            mode = 'idle-host'
            paused = $true
        }
        Write-State $initial
    }
    $state = Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($state.artifact_type -cne 'ninfer_generic_gpu_owner_state' -or
        [int]$state.schema_version -ne 1 -or
        [string]$state.mode -cne 'idle-host' -or
        $state.paused -isnot [bool]) {
        throw 'generic GPU-owner state envelope mismatch'
    }
    return $state
}

$state = Read-State
switch ($Action) {
    'status' {
        [ordered]@{
            artifact_type = 'ninfer_generic_gpu_owner_status'
            schema_version = 1
            mode = [string]$state.mode
            paused = [bool]$state.paused
        } | ConvertTo-Json -Compress
    }
    'stop' {
        $state.paused = $true
        Write-State $state
        [ordered]@{ status = 'stopped'; paused = $true } | ConvertTo-Json -Compress
    }
    'start' {
        # The generic published controller represents an idle host with no predecessor workload.
        # There is intentionally nothing to start; it remains paused and safe for a clean install.
        $state.paused = $true
        Write-State $state
        [ordered]@{ status = 'idle'; paused = $true } | ConvertTo-Json -Compress
    }
}
