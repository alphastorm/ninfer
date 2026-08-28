[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('status', 'stop', 'start')]
    [string]$Action
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$stateRoot = Join-Path $env:ProgramData 'NInfer\qwen38-3090-gpu-owner'
$statePath = Join-Path $stateRoot 'lease.json'
$qualifiedPowerLimitW = 300
$interactiveGpuMemoryThresholdBytes = 1GB

function Invoke-NvidiaSmi([string[]]$Arguments) {
    $output = @(& nvidia-smi.exe @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "nvidia-smi failed with exit $LASTEXITCODE" }
    return $output
}

function Get-PowerLimitW {
    $value = ((Invoke-NvidiaSmi @(
        '--query-gpu=power.limit',
        '--format=csv,noheader,nounits'
    )) | Out-String).Trim()
    $parsed = 0.0
    if (-not [double]::TryParse(
            $value,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$parsed
        )) {
        throw 'GPU power limit is not numeric'
    }
    return [int][Math]::Round($parsed)
}

function Get-InteractiveGpuWorkloadCount {
    $rows = @(Get-CimInstance `
        -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory `
        -ErrorAction Stop)
    $owners = [Collections.Generic.HashSet[int]]::new()
    foreach ($row in $rows) {
        if ([string]$row.Name -notmatch '^pid_(\d+)') { continue }
        if ([Int64]$row.DedicatedUsage -lt $interactiveGpuMemoryThresholdBytes) { continue }
        [void]$owners.Add([int]$Matches[1])
    }
    return $owners.Count
}

function Assert-InteractiveGpuWorkloadAbsent {
    if ((Get-InteractiveGpuWorkloadCount) -ne 0) {
        throw 'GPU has an active interactive workload; close it before appliance start'
    }
}

function Protect-StateRoot {
    New-Item -ItemType Directory -Force -Path $stateRoot | Out-Null
    & icacls.exe $stateRoot '/inheritance:r' '/grant:r' `
        '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'failed to protect GPU-owner lease state' }
}

function Read-State {
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) { return $null }
    return Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Write-State([int]$PriorPowerLimitW) {
    Protect-StateRoot
    $temporary = Join-Path $stateRoot ('.lease-' + [Guid]::NewGuid().ToString('N') + '.json')
    try {
        [IO.File]::WriteAllText(
            $temporary,
            (([ordered]@{
                schema_version = 1
                paused = $true
                qualified_power_limit_w = $qualifiedPowerLimitW
                prior_power_limit_w = $PriorPowerLimitW
            } | ConvertTo-Json -Compress) + [Environment]::NewLine),
            [Text.UTF8Encoding]::new($false)
        )
        Move-Item -LiteralPath $temporary -Destination $statePath -Force
    }
    finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

function Write-Status([bool]$Paused, [int]$PowerLimitW) {
    [ordered]@{
        schema_version = 1
        paused = $Paused
        power_limit_w = $PowerLimitW
        qualified_power_limit_w = $qualifiedPowerLimitW
        interactive_gpu_workload_count = (Get-InteractiveGpuWorkloadCount)
    } | ConvertTo-Json -Compress
}

switch ($Action) {
    'status' {
        $state = Read-State
        $limit = Get-PowerLimitW
        if ($null -ne $state) {
            if ([int]$state.schema_version -ne 1 -or
                -not [bool]$state.paused -or
                [int]$state.qualified_power_limit_w -ne $qualifiedPowerLimitW -or
                [int]$state.prior_power_limit_w -lt 100 -or
                [int]$state.prior_power_limit_w -gt 390 -or
                $limit -ne $qualifiedPowerLimitW) {
                throw 'GPU-owner lease state is invalid or the safe power cap drifted'
            }
            Write-Status $true $limit
        }
        else {
            Write-Status $false $limit
        }
    }
    'stop' {
        $state = Read-State
        if ($null -ne $state) {
            & $PSCommandPath -Action status
            break
        }
        Assert-InteractiveGpuWorkloadAbsent
        $prior = Get-PowerLimitW
        Invoke-NvidiaSmi @('-pl', [string]$qualifiedPowerLimitW) | Out-Null
        if ((Get-PowerLimitW) -ne $qualifiedPowerLimitW) {
            throw 'GPU did not enter the qualified safe power cap'
        }
        Write-State $prior
        Write-Status $true $qualifiedPowerLimitW
    }
    'start' {
        $state = Read-State
        if ($null -eq $state) {
            Write-Status $false (Get-PowerLimitW)
            break
        }
        $prior = [int]$state.prior_power_limit_w
        if ($prior -lt 100 -or $prior -gt 390) { throw 'recorded prior GPU power limit is invalid' }
        Invoke-NvidiaSmi @('-pl', [string]$prior) | Out-Null
        if ((Get-PowerLimitW) -ne $prior) { throw 'GPU prior power limit was not restored' }
        Remove-Item -LiteralPath $statePath -Force
        Write-Status $false $prior
    }
}
