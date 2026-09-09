[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('status', 'stop', 'start')]
    [string]$Action,

    [string]$StateRoot = (Join-Path $env:ProgramData 'NInfer\qwen38-3090-gpu-owner')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Protect-StateRoot.ps1')

$StateRoot = Initialize-NInferProtectedStateRoot $StateRoot
Assert-NInferProtectedStateTree $StateRoot
$statePath = Join-Path $StateRoot 'lease.json'
$qualifiedPowerLimitW = 300
$interactiveGpuMemoryThresholdBytes = 1GB
$nvidiaSmi = Join-Path ([Environment]::GetFolderPath('System')) 'nvidia-smi.exe'
if (-not (Test-Path -LiteralPath $nvidiaSmi -PathType Leaf)) {
    $nvidiaSmi = Join-Path $env:ProgramFiles 'NVIDIA Corporation\NVSMI\nvidia-smi.exe'
}
if (-not (Test-Path -LiteralPath $nvidiaSmi -PathType Leaf)) {
    throw 'trusted nvidia-smi installation is missing'
}

function Invoke-NvidiaSmi([string[]]$Arguments) {
    $output = @(& $nvidiaSmi @Arguments 2>&1)
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
        $usage = 0L
        if (-not [Int64]::TryParse([string]$row.DedicatedUsage, [ref]$usage) -or $usage -lt 0) {
            throw 'GPU process-memory counters returned an invalid dedicated-usage value'
        }
        if ([string]$row.Name -notmatch '^pid_(\d+)') {
            if ($usage -ge $interactiveGpuMemoryThresholdBytes) {
                throw 'GPU process-memory counters returned an unparseable active owner'
            }
            continue
        }
        if ($usage -lt $interactiveGpuMemoryThresholdBytes) { continue }
        [void]$owners.Add([int]$Matches[1])
    }
    return $owners.Count
}

function Assert-InteractiveGpuWorkloadAbsent {
    if ((Get-InteractiveGpuWorkloadCount) -ne 0) {
        throw 'GPU has an active interactive workload; close it before appliance start'
    }
}

function Read-State {
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) { return $null }
    return Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Write-State([int]$PriorPowerLimitW, [ValidateSet('prepared', 'paused')][string]$Phase) {
    $temporary = Join-Path $StateRoot ('.lease-' + [Guid]::NewGuid().ToString('N') + '.json')
    try {
        [IO.File]::WriteAllText(
            $temporary,
            (([ordered]@{
                schema_version = 1
                paused = $true
                phase = $Phase
                qualified_power_limit_w = $qualifiedPowerLimitW
                prior_power_limit_w = $PriorPowerLimitW
            } | ConvertTo-Json -Compress) + [Environment]::NewLine),
            [Text.UTF8Encoding]::new($false)
        )
        Move-Item -LiteralPath $temporary -Destination $statePath -Force
        Assert-NInferProtectedStateTree $StateRoot
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
            $phase = if ($null -eq $state.PSObject.Properties['phase']) { 'paused' } else { [string]$state.phase }
            if ([int]$state.schema_version -ne 1 -or
                -not [bool]$state.paused -or
                $phase -cnotin @('prepared', 'paused') -or
                [int]$state.qualified_power_limit_w -ne $qualifiedPowerLimitW -or
                [int]$state.prior_power_limit_w -lt 100 -or
                [int]$state.prior_power_limit_w -gt 390) {
                throw 'GPU-owner lease state is invalid or the safe power cap drifted'
            }
            if ($phase -ceq 'prepared') {
                if ($limit -eq $qualifiedPowerLimitW) {
                    Write-State ([int]$state.prior_power_limit_w) 'paused'
                    Write-Status $true $limit
                }
                elseif ($limit -eq [int]$state.prior_power_limit_w) {
                    Write-Status $false $limit
                }
                else {
                    throw 'prepared GPU-owner lease has an unrecoverable power-limit drift'
                }
            }
            elseif ($limit -ne $qualifiedPowerLimitW) {
                throw 'GPU-owner lease state is invalid or the safe power cap drifted'
            }
            else { Write-Status $true $limit }
        }
        else {
            Write-Status $false $limit
        }
    }
    'stop' {
        $state = Read-State
        if ($null -ne $state) {
            Assert-InteractiveGpuWorkloadAbsent
            $phase = if ($null -eq $state.PSObject.Properties['phase']) { 'paused' } else { [string]$state.phase }
            if ($phase -ceq 'prepared') {
                if ((Get-PowerLimitW) -ne $qualifiedPowerLimitW) {
                    Invoke-NvidiaSmi @('-pl', [string]$qualifiedPowerLimitW) | Out-Null
                }
                if ((Get-PowerLimitW) -ne $qualifiedPowerLimitW) {
                    throw 'GPU did not enter the qualified safe power cap'
                }
                Write-State ([int]$state.prior_power_limit_w) 'paused'
            }
            & $PSCommandPath -Action status -StateRoot $StateRoot
            break
        }
        Assert-InteractiveGpuWorkloadAbsent
        $prior = Get-PowerLimitW
        Write-State $prior 'prepared'
        try {
            Invoke-NvidiaSmi @('-pl', [string]$qualifiedPowerLimitW) | Out-Null
            if ((Get-PowerLimitW) -ne $qualifiedPowerLimitW) {
                throw 'GPU did not enter the qualified safe power cap'
            }
            Write-State $prior 'paused'
        }
        catch {
            $failure = $_
            try {
                Invoke-NvidiaSmi @('-pl', [string]$prior) | Out-Null
                if ((Get-PowerLimitW) -ne $prior) { throw 'prior GPU power limit restore failed' }
                Remove-Item -LiteralPath $statePath -Force
            }
            catch {
                throw [InvalidOperationException]::new(
                    "GPU cap acquisition failed and prepared lease restoration is required: $($_.Exception.Message)",
                    $failure.Exception
                )
            }
            throw $failure
        }
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
