[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Run', 'Status', 'Start', 'Stop', 'Restart', 'Rollback')]
    [string]$Action,

    [string]$StateRoot = (Join-Path $env:ProgramData 'NInfer\qwen38-4090')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-JsonFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "missing state file" }
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Write-JsonAtomic([string]$Path, [object]$Value) {
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    [IO.File]::WriteAllText(
        $temporary,
        ($Value | ConvertTo-Json -Depth 12),
        [Text.UTF8Encoding]::new($false)
    )
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

function Get-State {
    return Read-JsonFile (Join-Path $StateRoot 'state.json')
}

function Get-Release([object]$State, [string]$ReleaseId) {
    $property = $State.releases.PSObject.Properties[$ReleaseId]
    if ($null -eq $property) { throw "release is not installed: $ReleaseId" }
    return $property.Value
}

function Read-OneLineSecret([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $length = $bytes.Length
    while ($length -gt 0 -and ($bytes[$length - 1] -eq 10 -or $bytes[$length - 1] -eq 13)) {
        $length--
    }
    if ($length -eq 0) { throw 'API-key file is empty' }
    for ($index = 0; $index -lt $length; $index++) {
        if ($bytes[$index] -eq 0 -or $bytes[$index] -eq 10 -or $bytes[$index] -eq 13) {
            throw 'API-key file must contain exactly one non-empty line'
        }
    }
    return [Text.UTF8Encoding]::new($false, $true).GetString($bytes, 0, $length)
}

function Assert-FileHash([string]$Path, [string]$Expected, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label is missing" }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
    if ($actual -cne $Expected) { throw "$Label SHA-256 mismatch" }
}

function Get-OwnedProcess([object]$Runtime, [object]$Release) {
    if ($null -eq $Runtime -or $null -eq $Runtime.pid) { return $null }
    $process = Get-Process -Id ([int]$Runtime.pid) -ErrorAction SilentlyContinue
    if ($null -eq $process) { return $null }
    $cim = Get-CimInstance Win32_Process -Filter "ProcessId = $($Runtime.pid)" -ErrorAction SilentlyContinue
    if ($null -eq $cim -or [string]::IsNullOrWhiteSpace($cim.ExecutablePath)) { return $null }
    $expectedPath = [IO.Path]::GetFullPath([string]$Release.server_executable)
    $actualPath = [IO.Path]::GetFullPath([string]$cim.ExecutablePath)
    $startTicks = $process.StartTime.ToUniversalTime().Ticks
    if (-not [string]::Equals($actualPath, $expectedPath, [StringComparison]::OrdinalIgnoreCase) -or
        $startTicks -ne [Int64]$Runtime.start_time_utc_ticks) {
        return $null
    }
    return $process
}

function Get-RuntimeState {
    $path = Join-Path $StateRoot 'runtime.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    try { return Read-JsonFile $path } catch { return $null }
}

function Invoke-ServerStatus([object]$Release) {
    $key = Read-OneLineSecret ([string]$Release.api_key_file)
    $headers = @{ Authorization = "Bearer $key" }
    $uri = "http://$($Release.host):$($Release.port)/v1/ninfer/status"
    return Invoke-RestMethod -Method Get -Uri $uri -Headers $headers -TimeoutSec 5 -UseBasicParsing
}

function Assert-ServerIdentity([object]$Status, [object]$Release) {
    if ($Status.artifact_type -cne 'ninfer_server_status' -or [int]$Status.schema_version -ne 1 -or
        $Status.status -cne 'ok') {
        throw 'server status envelope mismatch'
    }
    foreach ($group in @('identity', 'runtime', 'scheduler', 'cache', 'mtp')) {
        if ($null -eq $Status.PSObject.Properties[$group]) {
            throw "server status is missing required group: $group"
        }
    }
    $identity = $Status.identity
    $expected = [ordered]@{
        patch_stack_sha = [string]$Release.patch_stack_sha
        deployment_profile = [string]$Release.deployment_profile
        binary_sha256 = [string]$Release.binary_sha256
        model_artifact_sha256 = [string]$Release.model_sha256
        config_sha256 = [string]$Release.config_sha256
    }
    foreach ($entry in $expected.GetEnumerator()) {
        if ([string]$identity.($entry.Key) -cne $entry.Value) {
            throw "server identity mismatch: $($entry.Key)"
        }
    }
}

function Get-StatusObject {
    $state = Get-State
    $release = Get-Release $state ([string]$state.active_release)
    $runtime = Get-RuntimeState
    $owned = Get-OwnedProcess $runtime $release
    $task = Get-ScheduledTask -TaskName ([string]$state.task_name) -ErrorAction SilentlyContinue
    $serverStatus = $null
    $endpointState = 'unavailable'
    if ($null -ne $owned) {
        try {
            $serverStatus = Invoke-ServerStatus $release
            Assert-ServerIdentity $serverStatus $release
            $endpointState = 'ready'
        }
        catch {
            $endpointState = 'starting_or_failed'
        }
    }
    return [ordered]@{
        artifact_type = 'ninfer_windows_lifecycle_status'
        schema_version = 1
        release_id = [string]$state.active_release
        previous_release_id = if ($null -eq $state.previous_release) { $null } else { [string]$state.previous_release }
        task_name = [string]$state.task_name
        task_state = if ($null -eq $task) { 'missing' } else { [string]$task.State }
        process_state = if ($null -eq $owned) { 'stopped' } else { 'running' }
        endpoint_state = $endpointState
        server = $serverStatus
    }
}

function Wait-Ready([int]$TimeoutSeconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $lastError = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $state = Get-State
            $release = Get-Release $state ([string]$state.active_release)
            $status = Invoke-ServerStatus $release
            Assert-ServerIdentity $status $release
            return $status
        }
        catch {
            $lastError = $_.Exception.Message
            Start-Sleep -Seconds 2
        }
    }
    throw "release did not become ready: $lastError"
}

function Stop-ManagedRelease {
    $state = Get-State
    $release = Get-Release $state ([string]$state.active_release)
    $runtime = Get-RuntimeState
    $task = Get-ScheduledTask -TaskName ([string]$state.task_name) -ErrorAction SilentlyContinue
    if ($null -ne $task -and $task.State -eq 'Running') {
        Stop-ScheduledTask -TaskName ([string]$state.task_name)
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $owned = Get-OwnedProcess $runtime $release
        if ($null -eq $owned) { break }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    $owned = Get-OwnedProcess $runtime $release
    if ($null -ne $owned) {
        Stop-Process -Id $owned.Id -Force
        $owned.WaitForExit(10000) | Out-Null
    }
    if ($null -ne (Get-OwnedProcess $runtime $release)) {
        throw 'owned server process did not stop'
    }
    Remove-Item -LiteralPath (Join-Path $StateRoot 'runtime.json') -Force -ErrorAction SilentlyContinue
}

function Start-ManagedRelease {
    $state = Get-State
    $release = Get-Release $state ([string]$state.active_release)
    $owned = Get-OwnedProcess (Get-RuntimeState) $release
    if ($null -ne $owned) {
        Assert-ServerIdentity (Invoke-ServerStatus $release) $release
        return
    }
    $listener = Get-NetTCPConnection -State Listen -LocalPort ([int]$release.port) -ErrorAction SilentlyContinue
    if ($null -ne $listener) { throw 'release listen port is already owned by another process' }
    Start-ScheduledTask -TaskName ([string]$state.task_name)
    Wait-Ready 600 | Out-Null
}

function Invoke-Run {
    $state = Get-State
    $release = Get-Release $state ([string]$state.active_release)
    $lockPath = Join-Path $StateRoot 'run.lock'
    $lock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite,
                           [IO.FileShare]::None)
    try {
        Assert-FileHash ([string]$release.server_executable) ([string]$release.binary_sha256) 'server executable'
        Assert-FileHash ([string]$release.model_artifact) ([string]$release.model_sha256) 'model artifact'
        Assert-FileHash ([string]$release.config_file) ([string]$release.config_sha256) 'server config'
        $config = Read-JsonFile ([string]$release.config_file)
        $cache = Join-Path ([string]$release.release_root) 'cache'
        $logs = Join-Path ([string]$release.release_root) 'logs'
        New-Item -ItemType Directory -Force -Path $cache, $logs | Out-Null
        $serverArguments = @(
            [string]$release.model_artifact,
            '--host', [string]$config.listen.host,
            '--port', [string]$config.listen.port,
            '--api-key-file', [string]$release.api_key_file,
            '--model-id', [string]$config.model_id,
            '--binary-sha256', [string]$release.binary_sha256,
            '--artifact-sha256', [string]$release.model_sha256,
            '--config-sha256', [string]$release.config_sha256,
            '--deployment-profile', [string]$release.deployment_profile,
            '--device', [string]$config.engine.device,
            '--max-context', [string]$config.engine.max_context,
            '--kv-capacity', [string]$config.engine.kv_capacity,
            '--prefill-chunk', [string]$config.engine.prefill_chunk,
            '--kv-dtype', [string]$config.engine.kv_dtype,
            '--max-concurrency', [string]$config.engine.max_concurrency,
            '--max-pending-requests', [string]$config.engine.max_pending_requests,
            '--pending-timeout-ms', [string]$config.engine.pending_timeout_ms,
            '--reasoning-effort', [string]$config.reasoning.effort,
            '--response-store-max-records', [string]$config.response_store.max_records,
            '--response-store-max-mib', [string]$config.response_store.max_mib,
            '--disk-cache',
            '--disk-cache-dir', $cache,
            '--disk-cache-gb', [string]$config.persistent_cache.quota_gib,
            '--preserve-thinking',
            '--request-log-jsonl', (Join-Path $logs 'requests.jsonl'),
            '--log-stats-interval-ms', [string]$config.telemetry.stats_interval_ms,
            '--no-ui'
        )
        $stdout = Join-Path $logs 'stdout.log'
        $stderr = Join-Path $logs 'stderr.log'
        $process = Start-Process -FilePath ([string]$release.server_executable) -ArgumentList $serverArguments `
            -WorkingDirectory ([string]$release.release_root) -RedirectStandardOutput $stdout `
            -RedirectStandardError $stderr -PassThru
        Write-JsonAtomic (Join-Path $StateRoot 'runtime.json') ([ordered]@{
                schema_version = 1
                release_id = [string]$state.active_release
                pid = $process.Id
                start_time_utc_ticks = $process.StartTime.ToUniversalTime().Ticks
            })
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "ninfer-serve exited with code $($process.ExitCode)" }
    }
    finally {
        $runtime = Get-RuntimeState
        if ($null -ne $runtime -and [string]$runtime.release_id -ceq [string]$state.active_release) {
            Remove-Item -LiteralPath (Join-Path $StateRoot 'runtime.json') -Force -ErrorAction SilentlyContinue
        }
        $lock.Dispose()
    }
}

switch ($Action) {
    'Run' {
        Invoke-Run
    }
    'Status' {
        Get-StatusObject | ConvertTo-Json -Depth 16
    }
    'Start' {
        Start-ManagedRelease
        Get-StatusObject | ConvertTo-Json -Depth 16
    }
    'Stop' {
        Stop-ManagedRelease
        Get-StatusObject | ConvertTo-Json -Depth 16
    }
    'Restart' {
        Stop-ManagedRelease
        Start-ManagedRelease
        Get-StatusObject | ConvertTo-Json -Depth 16
    }
    'Rollback' {
        $state = Get-State
        if ($null -eq $state.previous_release -or [string]::IsNullOrWhiteSpace([string]$state.previous_release)) {
            throw 'no previous installed release is available for rollback'
        }
        $current = [string]$state.active_release
        $previous = [string]$state.previous_release
        Get-Release $state $previous | Out-Null
        Stop-ManagedRelease
        $state.active_release = $previous
        $state.previous_release = $current
        Write-JsonAtomic (Join-Path $StateRoot 'state.json') $state
        try {
            Start-ManagedRelease
        }
        catch {
            Stop-ManagedRelease
            $state.active_release = $current
            $state.previous_release = $previous
            Write-JsonAtomic (Join-Path $StateRoot 'state.json') $state
            Start-ManagedRelease
            throw
        }
        Get-StatusObject | ConvertTo-Json -Depth 16
    }
}
