[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('status', 'stop', 'start')]
    [string]$Action,

    [string]$StateRoot = (Join-Path $env:ProgramData 'NInfer\gpu-owner')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Protect-StateRoot.ps1')
$StateRoot = Initialize-NInferProtectedStateRoot $StateRoot
Assert-NInferProtectedStateTree $StateRoot
$statePath = Join-Path $StateRoot 'state.json'

function Write-State([object]$Value) {
    $temporary = "$statePath.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText(
            $temporary,
            ($Value | ConvertTo-Json -Depth 8),
            [Text.UTF8Encoding]::new($false)
        )
        Move-Item -LiteralPath $temporary -Destination $statePath -Force
        Assert-NInferProtectedStateTree $StateRoot
    }
    finally { Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue }
}

function Get-ComputeOwnerStatus([bool]$ThrowOnFailure) {
    try {
        $systemDirectory = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)
        $nvidiaSmi = [IO.Path]::GetFullPath((Join-Path $systemDirectory 'nvidia-smi.exe'))
        if (-not (Test-Path -LiteralPath $nvidiaSmi -PathType Leaf)) {
            throw 'trusted System32 nvidia-smi application is missing'
        }
        $start = [Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $nvidiaSmi
        $start.Arguments = '--query-compute-apps=pid --format=csv,noheader,nounits'
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $start
        try {
            if (-not $process.Start()) { throw 'trusted nvidia-smi process did not start' }
            $stdoutTask = $process.StandardOutput.ReadToEndAsync()
            $stderrTask = $process.StandardError.ReadToEndAsync()
            $process.WaitForExit()
            $stdout = [string]$stdoutTask.GetAwaiter().GetResult()
            $stderr = [string]$stderrTask.GetAwaiter().GetResult()
            if ($process.ExitCode -ne 0) {
                throw "trusted nvidia-smi compute-owner query exited $($process.ExitCode): $($stderr.Substring(0, [Math]::Min(256, $stderr.Length)))"
            }
            if (-not [string]::IsNullOrWhiteSpace($stderr)) {
                throw "trusted nvidia-smi compute-owner query wrote stderr: $($stderr.Substring(0, [Math]::Min(256, $stderr.Length)))"
            }
        }
        finally { $process.Dispose() }

        $trimmed = $stdout.TrimEnd([char[]]@(13, 10))
        if ([string]::IsNullOrWhiteSpace($trimmed)) {
            return [pscustomobject]@{ available = $true; pids = @() }
        }
        $pids = [Collections.Generic.List[uint32]]::new()
        foreach ($row in @($trimmed -split '[\r\n]+')) {
            $value = ([string]$row).Trim()
            [uint32]$parsedPid = 0
            if ([string]::IsNullOrWhiteSpace($value) -or
                -not [uint32]::TryParse(
                    $value,
                    [Globalization.NumberStyles]::None,
                    [Globalization.CultureInfo]::InvariantCulture,
                    [ref]$parsedPid
                ) -or $parsedPid -eq 0) {
                throw "trusted nvidia-smi returned a malformed compute-owner row: $value"
            }
            $pids.Add($parsedPid)
        }
        return [pscustomobject]@{ available = $true; pids = @($pids) }
    }
    catch {
        if ($ThrowOnFailure) { throw 'failed to inspect GPU compute owners' }
        $message = [string]$_.Exception.Message
        if ($message.Length -gt 512) { $message = $message.Substring(0, 512) }
        return [pscustomobject]@{ available = $false; pids = @(); error = $message }
    }
}

function Read-State {
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
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
        $compute = Get-ComputeOwnerStatus $false
        [ordered]@{
            artifact_type = 'ninfer_generic_gpu_owner_status'
            schema_version = 1
            mode = [string]$state.mode
            paused = [bool]$state.paused -and [bool]$compute.available -and @($compute.pids).Count -eq 0
            compute_owner_query_available = [bool]$compute.available
            compute_owner_query_application = 'trusted-system32-nvidia-smi'
            compute_owner_query_error = if ([bool]$compute.available) { $null } else { [string]$compute.error }
            active_compute_owner_count = @($compute.pids).Count
        } | ConvertTo-Json -Compress
    }
    'stop' {
        $compute = Get-ComputeOwnerStatus $true
        if (@($compute.pids).Count -ne 0) {
            throw 'unmanaged GPU compute owner is active; provide an operator-specific GPU-owner controller'
        }
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
