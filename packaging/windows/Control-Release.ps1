[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Run', 'Status', 'Start', 'Stop', 'Restart', 'Rollback', 'Uninstall')]
    [string]$Action,

    [string]$StateRoot = (Join-Path $env:ProgramData 'NInfer\qwen38-3090-omp-v0.2')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Protect-StateRoot.ps1')
if (-not (Test-Path -LiteralPath $StateRoot -PathType Container)) {
    throw 'protected release state root is missing'
}
$StateRoot = Initialize-NInferProtectedStateRoot $StateRoot
Assert-NInferProtectedStateRoot $StateRoot

function Read-JsonFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "missing state file" }
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Get-TrustedNvidiaSmiPath {
    $path = Join-Path ([Environment]::GetFolderPath('System')) 'nvidia-smi.exe'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $path = Join-Path $env:ProgramFiles 'NVIDIA Corporation\NVSMI\nvidia-smi.exe'
    }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw 'trusted nvidia-smi installation is missing'
    }
    return $path
}

function Write-JsonAtomic([string]$Path, [object]$Value) {
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText(
            $temporary,
            ($Value | ConvertTo-Json -Depth 16),
            [Text.UTF8Encoding]::new($false)
        )
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    }
    finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

function Get-State {
    $state = Read-JsonFile (Join-Path $StateRoot 'state.json')
    if ($state.artifact_type -cne 'ninfer_windows_lifecycle_state' -or
        [int]$state.schema_version -ne 4) {
        throw 'release lifecycle state envelope mismatch'
    }
    foreach ($field in @('prepared_release', 'active_release', 'previous_release', 'releases')) {
        if ($null -eq $state.PSObject.Properties[$field]) {
            throw "release lifecycle state is missing pointer field: $field"
        }
    }
    if ([string]::IsNullOrWhiteSpace([string]$state.active_release) -or
        $null -eq $state.releases.PSObject.Properties[[string]$state.active_release]) {
        throw 'release lifecycle active pointer is invalid'
    }
    foreach ($pointer in @('prepared_release', 'previous_release')) {
        $value = [string]$state.$pointer
        if (-not [string]::IsNullOrWhiteSpace($value) -and
            $null -eq $state.releases.PSObject.Properties[$value]) {
            throw "release lifecycle $pointer pointer is invalid"
        }
    }
    return $state
}

function Assert-NoPreparedRelease([object]$State) {
    if (-not [string]::IsNullOrWhiteSpace([string]$State.prepared_release)) {
        throw 'a prepared installer transaction must be repaired before this action'
    }
}

function Get-Release([object]$State, [string]$ReleaseId) {
    $property = $State.releases.PSObject.Properties[$ReleaseId]
    if ($null -eq $property) { throw "release is not installed: $ReleaseId" }
    $release = $property.Value
    foreach ($field in @(
            'ninfer_sha256', 'binary_sha256', 'benchmark_sha256', 'package_sha256',
            'inner_checksums_sha256', 'config_sha256', 'model_artifact_sha256'
        )) {
        if ([string]$release.$field -cnotmatch '^[0-9a-f]{64}$') {
            throw "installed release identity is invalid: $field"
        }
    }
    foreach ($field in @('upstream_base_sha', 'lineage_base_sha', 'patch_stack_sha')) {
        if ([string]$release.$field -cnotmatch '^[0-9a-f]{40}$') {
            throw "installed release source identity is invalid: $field"
        }
    }
    if ([string]$release.build_profile -cnotin @('omp-v0.2.0-rtx3090', 'omp-v0.2.1-rtx3090', 'omp-v0.2.2-rtx3090', 'omp-v0.2.3-rtx3090', 'omp-v0.2.4-rtx3090', 'omp-v0.2.5-rtx3090') -or
        [string]$release.cuda_architecture -cne 'sm_86') {
        throw 'installed release immutable build profile is invalid'
    }
    return $release
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
function Test-PathWithinRoot([string]$Path, [string]$Root) {
    $separators = [char[]]@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd($separators) + [IO.Path]::DirectorySeparatorChar
    return $fullPath.StartsWith($fullRoot, [StringComparison]::OrdinalIgnoreCase)
}
function Test-SamePath([string]$Left, [string]$Right) {
    return [string]::Equals(
        [IO.Path]::GetFullPath($Left),
        [IO.Path]::GetFullPath($Right),
        [StringComparison]::OrdinalIgnoreCase
    )
}


function Assert-InstalledModelIdentity([object]$Release) {
    foreach ($field in @(
            'model_reference', 'model_bytes', 'model_creation_utc_ticks',
            'model_last_write_utc_ticks', 'cache_root', 'receipts_root'
        )) {
        if ($null -eq $Release.PSObject.Properties[$field]) {
            throw 'model artifact verified metadata is missing; reinstall the release'
        }
    }
    $path = [string]$Release.model_artifact
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'model artifact is missing' }
    if ([string]$Release.model_reference -cne 'external-pinned-read-only') {
        throw 'model artifact reference kind is unsupported; reinstall the release'
    }
    if ([string]$Release.model_reference -ceq 'external-pinned-read-only' -and
        (Test-PathWithinRoot $path $StateRoot)) {
        throw 'pinned model artifact is inside operation-owned lifecycle state'
    }
    $item = Get-Item -LiteralPath $path
    if ([Int64]$item.Length -ne [Int64]$Release.model_bytes -or
        [Int64]$item.CreationTimeUtc.Ticks -ne [Int64]$Release.model_creation_utc_ticks -or
        [Int64]$item.LastWriteTimeUtc.Ticks -ne [Int64]$Release.model_last_write_utc_ticks) {
        throw 'model artifact changed after install; reinstall the release'
    }
}

function Assert-CandidateLayout([object]$Release, [string]$ReleaseId) {
    if ([string]$Release.model_reference -cne 'external-pinned-read-only') {
        throw 'candidate model reference is not external and read-only'
    }
    $root = [string]$Release.release_root
    $expectedRoot = Join-Path (Join-Path $StateRoot 'releases') $ReleaseId
    if (-not (Test-SamePath $root $expectedRoot)) { throw 'candidate root identity mismatch' }
    $expected = @('bin', 'config', 'logs', 'receipts')
    $actual = @(Get-ChildItem -LiteralPath $root -Directory -Force | Sort-Object Name | ForEach-Object Name)
    $expectedSorted = @($expected | Sort-Object)
    if ($actual.Count -ne $expectedSorted.Count) { throw 'candidate root layout mismatch' }
    for ($index = 0; $index -lt $expectedSorted.Count; $index++) {
        if ([string]$actual[$index] -cne [string]$expectedSorted[$index]) {
            throw 'candidate root layout mismatch'
        }
    }
    if (@(Get-ChildItem -LiteralPath $root -File -Force).Count -ne 0) {
        throw 'candidate root contains an unclassified file'
    }
    if (-not (Test-SamePath ([string]$Release.server_executable) (Join-Path (Join-Path $root 'bin') 'ninfer-serve.exe'))) {
        throw 'candidate server executable identity mismatch'
    }
    if (-not (Test-SamePath ([string]$Release.config_file) (Join-Path (Join-Path $root 'config') 'server-config.json'))) {
        throw 'candidate server config identity mismatch'
    }
    if (-not (Test-SamePath ([string]$Release.receipts_root) (Join-Path $root 'receipts'))) {
        throw 'candidate receipts root identity mismatch'
    }
    $expectedCacheRoot = Join-Path (Join-Path $StateRoot 'cache') $ReleaseId
    if (-not (Test-SamePath ([string]$Release.cache_root) $expectedCacheRoot)) {
        throw 'candidate cache root identity mismatch'
    }
    $expectedSecret = Join-Path (Join-Path (Join-Path $StateRoot 'secrets') $ReleaseId) 'api-key.txt'
    if (-not (Test-SamePath ([string]$Release.api_key_file) $expectedSecret)) {
        throw 'candidate secret identity mismatch'
    }
}

function Assert-SelectedGpuIdentity([object]$Release) {
    foreach ($field in @('gpu_index', 'gpu_uuid', 'gpu_name')) {
        if ($null -eq $Release.PSObject.Properties[$field]) {
            throw 'selected GPU identity is missing; reinstall the release'
        }
    }
    if (Test-Path Env:CUDA_VISIBLE_DEVICES) {
        throw 'CUDA_VISIBLE_DEVICES must be absent for bound GPU ordinal identity'
    }
    $rows = @(& (Get-TrustedNvidiaSmiPath) --query-gpu=index,uuid,name --format=csv,noheader,nounits 2>&1)
    if ($LASTEXITCODE -ne 0) { throw 'selected GPU identity query failed' }
    $matched = $false
    foreach ($row in $rows) {
        $parts = @(([string]$row -split ',') | ForEach-Object { $_.Trim() })
        $rowIndex = -1
        if ($parts.Count -ne 3 -or -not [int]::TryParse($parts[0], [ref]$rowIndex)) {
            throw 'selected GPU identity query returned an invalid row'
        }
        if ($rowIndex -ne [int]$Release.gpu_index) { continue }
        if ($parts[1] -cne [string]$Release.gpu_uuid -or $parts[2] -cne [string]$Release.gpu_name) {
            throw 'selected GPU identity changed after install'
        }
        $matched = $true
    }
    if (-not $matched) { throw 'selected GPU ordinal is unavailable' }
}

function Get-GpuOwner([object]$State) {
    $property = $State.PSObject.Properties['gpu_owner']
    if ($null -eq $property -or $null -eq $property.Value) { return $null }
    $owner = $property.Value
    $expectedStateRoot = Join-Path $StateRoot 'gpu-owner-state'
    if ($null -eq $owner.PSObject.Properties['state_root'] -or
        -not [string]::Equals(
            [IO.Path]::GetFullPath([string]$owner.state_root),
            [IO.Path]::GetFullPath($expectedStateRoot),
            [StringComparison]::OrdinalIgnoreCase
        )) {
        throw 'GPU-owner state root is outside the managed release root'
    }
    Assert-FileHash ([string]$owner.controller_path) ([string]$owner.controller_sha256) `
        'GPU-owner controller'
    return $owner
}

function Invoke-GpuOwner([object]$State, [ValidateSet('status', 'stop', 'start')][string]$OwnerAction) {
    $owner = Get-GpuOwner $State
    if ($null -eq $owner) { return $null }
    $output = ((& ([string]$owner.controller_path) -Action $OwnerAction -StateRoot ([string]$owner.state_root)) | Out-String).Trim()
    if ($OwnerAction -cne 'status') { return $null }
    if ([string]::IsNullOrWhiteSpace($output)) { throw 'GPU-owner status returned no JSON' }
    $status = $output | ConvertFrom-Json
    if ($null -eq $status.PSObject.Properties['paused'] -or
        $status.paused -isnot [bool]) {
        throw 'GPU-owner status must expose a paused boolean'
    }
    return $status
}

function Get-GpuOwnerLease {
    $path = Join-Path $StateRoot 'gpu-owner-lease.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    $lease = Read-JsonFile $path
    if ($lease.artifact_type -cne 'ninfer_gpu_owner_lease' -or [int]$lease.schema_version -ne 1) {
        throw 'GPU-owner lease envelope mismatch'
    }
    return $lease
}

function Acquire-GpuOwnerLease {
    $state = Get-State
    $owner = Get-GpuOwner $state
    if ($null -eq $owner) { return }
    $leasePath = Join-Path $StateRoot 'gpu-owner-lease.json'
    $lease = Get-GpuOwnerLease
    if ($null -ne $lease) {
        if ([string]$lease.controller_sha256 -cne [string]$owner.controller_sha256) {
            throw 'GPU-owner lease controller identity mismatch'
        }
        $current = Invoke-GpuOwner $state 'status'
        if ([string]$lease.phase -cne 'released' -or -not [bool]$current.paused) {
            Invoke-GpuOwner $state 'stop' | Out-Null
            $current = Invoke-GpuOwner $state 'status'
            if (-not [bool]$current.paused) {
                throw 'interrupted GPU-owner lease acquisition remains unsatisfied'
            }
            $lease.phase = 'released'
            Write-JsonAtomic $leasePath $lease
        }
        return
    }

    $before = Invoke-GpuOwner $state 'status'
    $lease = [ordered]@{
        artifact_type = 'ninfer_gpu_owner_lease'
        schema_version = 1
        release_id = [string]$state.active_release
        controller_sha256 = [string]$owner.controller_sha256
        prior_paused = [bool]$before.paused
        phase = 'captured'
        acquired_utc = [DateTime]::UtcNow.ToString('o')
    }
    Write-JsonAtomic $leasePath $lease
    try {
        if (-not [bool]$before.paused) { Invoke-GpuOwner $state 'stop' | Out-Null }
        $after = Invoke-GpuOwner $state 'status'
        if (-not [bool]$after.paused) { throw 'GPU owner did not release the GPU' }
        $lease.phase = 'released'
        Write-JsonAtomic $leasePath $lease
    }
    catch {
        $acquireFailure = $_
        try {
            if ([bool]$lease.prior_paused) {
                Invoke-GpuOwner $state 'stop' | Out-Null
            }
            else {
                Invoke-GpuOwner $state 'start' | Out-Null
            }
            Remove-Item -LiteralPath $leasePath -Force
        }
        catch {
            throw [InvalidOperationException]::new(
                "GPU-owner release failed and prior state restoration failed: $($_.Exception.Message)",
                $acquireFailure.Exception
            )
        }
        throw $acquireFailure
    }
}

function Restore-GpuOwnerLease {
    $lease = Get-GpuOwnerLease
    if ($null -eq $lease) { return }
    $state = Get-State
    $owner = Get-GpuOwner $state
    if ($null -eq $owner -or
        [string]$lease.controller_sha256 -cne [string]$owner.controller_sha256) {
        throw 'cannot restore GPU owner with a different controller identity'
    }
    if ([bool]$lease.prior_paused) {
        Invoke-GpuOwner $state 'stop' | Out-Null
    }
    else {
        Invoke-GpuOwner $state 'start' | Out-Null
    }
    $after = Invoke-GpuOwner $state 'status'
    if ([bool]$after.paused -ne [bool]$lease.prior_paused) {
        throw 'GPU-owner prior state was not restored exactly'
    }
    Remove-Item -LiteralPath (Join-Path $StateRoot 'gpu-owner-lease.json') -Force
}

function Get-GpuOwnerStatus([object]$State) {
    $ownerProperty = $State.PSObject.Properties['gpu_owner']
    if ($null -eq $ownerProperty -or $null -eq $ownerProperty.Value) {
        return [ordered]@{ managed = $false; lease_active = $false }
    }
    $lease = $null
    try { $lease = Get-GpuOwnerLease } catch { $lease = $null }
    try {
        $status = Invoke-GpuOwner $State 'status'
        return [ordered]@{
            managed = $true
            lease_active = $null -ne $lease
            prior_paused = if ($null -eq $lease) { $null } else { [bool]$lease.prior_paused }
            current_paused = [bool]$status.paused
            controller_sha256 = [string]$ownerProperty.Value.controller_sha256
            status = 'ok'
        }
    }
    catch {
        return [ordered]@{
            managed = $true
            lease_active = $null -ne $lease
            controller_sha256 = [string]$ownerProperty.Value.controller_sha256
            status = 'error'
            error = $_.Exception.Message
        }
    }
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
        model_artifact_sha256 = [string]$Release.model_artifact_sha256
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
        schema_version = 4
        release_id = [string]$state.active_release
        prepared_release_id = if ([string]::IsNullOrWhiteSpace([string]$state.prepared_release)) { $null } else { [string]$state.prepared_release }
        previous_release_id = if ($null -eq $state.previous_release) { $null } else { [string]$state.previous_release }
        task_name = [string]$state.task_name
        task_start_mode = 'explicit-on-demand'
        task_state = if ($null -eq $task) { 'missing' } else { [string]$task.State }
        process_state = if ($null -eq $owned) { 'stopped' } else { 'running' }
        endpoint_state = $endpointState
        gpu_owner = Get-GpuOwnerStatus $state
        server = $serverStatus
    }
}

function Assert-ManagedStartLiveness([object]$State, [object]$Release) {
    $task = Get-ScheduledTask -TaskName ([string]$State.task_name) -ErrorAction SilentlyContinue
    if ($null -eq $task) {
        throw 'managed scheduled task disappeared before release became ready'
    }
    if ([string]$task.State -cne 'Running') {
        throw "managed scheduled task exited before release became ready: $($task.State)"
    }

    $runtime = Get-RuntimeState
    if ($null -eq $runtime) {
        throw 'managed scheduled task did not publish process ownership before startup grace elapsed'
    }
    if ($null -eq (Get-OwnedProcess $runtime $Release)) {
        throw 'owned server process exited before release became ready'
    }
}

function Wait-Ready([int]$TimeoutSeconds) {
    $started = [DateTime]::UtcNow
    # Task Scheduler may not dispatch the wrapper immediately on an attended workstation. Give it
    # time to publish the owned-process record; the full readiness deadline still bounds model load.
    $startupGraceDeadline = $started.AddSeconds(30)
    $deadline = $started.AddSeconds($TimeoutSeconds)
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
            if ([DateTime]::UtcNow -ge $startupGraceDeadline) {
                $state = Get-State
                $release = Get-Release $state ([string]$state.active_release)
                Assert-ManagedStartLiveness $state $release
            }
            Start-Sleep -Seconds 2
        }
    }
    throw "release did not become ready: $lastError"
}

function Stop-ManagedProcess {
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

function Stop-ManagedRelease([bool]$RestoreOwner = $true) {
    Stop-ManagedProcess
    if ($RestoreOwner) { Restore-GpuOwnerLease }
}

function Start-ManagedRelease {
    $state = Get-State
    Assert-NoPreparedRelease $state
    $release = Get-Release $state ([string]$state.active_release)
    $owned = Get-OwnedProcess (Get-RuntimeState) $release
    if ($null -ne $owned) {
        Assert-ServerIdentity (Invoke-ServerStatus $release) $release
        return
    }
    $listener = Get-NetTCPConnection -State Listen -LocalPort ([int]$release.port) -ErrorAction SilentlyContinue
    if ($null -ne $listener) { throw 'release listen port is already owned by another process' }

    Acquire-GpuOwnerLease
    try {
        Start-ScheduledTask -TaskName ([string]$state.task_name)
        Wait-Ready 600 | Out-Null
    }
    catch {
        $startFailure = $_
        $cleanupFailures = [Collections.Generic.List[string]]::new()
        try { Stop-ManagedProcess } catch { $cleanupFailures.Add("candidate stop: $($_.Exception.Message)") }
        try { Restore-GpuOwnerLease } catch { $cleanupFailures.Add("GPU-owner restore: $($_.Exception.Message)") }
        if ($cleanupFailures.Count -ne 0) {
            throw [InvalidOperationException]::new(
                "release start failed and cleanup was incomplete: $([string]::Join('; ', $cleanupFailures))",
                $startFailure.Exception
            )
        }
        throw $startFailure
    }
}

function Quote-NativeArgument([string]$Argument) {
    if ($Argument.Length -eq 0) { return '""' }
    if ($Argument -notmatch '[\s"]') { return $Argument }
    $builder = [Text.StringBuilder]::new()
    $builder.Append('"') | Out-Null
    $slashes = 0
    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq '\') {
            $slashes++
            continue
        }
        if ($character -eq '"') {
            $builder.Append([string]::new([char]92, ($slashes * 2 + 1))) | Out-Null
            $builder.Append('"') | Out-Null
        }
        else {
            if ($slashes -ne 0) { $builder.Append([string]::new([char]92, $slashes)) | Out-Null }
            $builder.Append($character) | Out-Null
        }
        $slashes = 0
    }
    if ($slashes -ne 0) { $builder.Append([string]::new([char]92, ($slashes * 2))) | Out-Null }
    $builder.Append('"') | Out-Null
    return $builder.ToString()
}

function Invoke-Run {
    $state = $null
    $lock = $null
    $ownerLeaseHeld = $false
    try {
        $lockPath = Join-Path $StateRoot 'run.lock'
        $lock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite,
                               [IO.FileShare]::None)
        Acquire-GpuOwnerLease
        $ownerLeaseHeld = $true
        $state = Get-State
        Assert-NoPreparedRelease $state
        $release = Get-Release $state ([string]$state.active_release)
        Assert-CandidateLayout $release ([string]$state.active_release)
        Assert-FileHash (Join-Path (Join-Path ([string]$release.release_root) 'bin') 'ninfer.exe') ([string]$release.ninfer_sha256) 'CLI executable'
        Assert-FileHash ([string]$release.server_executable) ([string]$release.binary_sha256) 'server executable'
        Assert-FileHash (Join-Path (Join-Path ([string]$release.release_root) 'bin') 'ninfer_bench.exe') ([string]$release.benchmark_sha256) 'benchmark executable'
        Assert-InstalledModelIdentity $release
        Assert-SelectedGpuIdentity $release
        Assert-FileHash ([string]$release.config_file) ([string]$release.config_sha256) 'server config'
        $config = Read-JsonFile ([string]$release.config_file)
        $cache = [string]$release.cache_root
        if (-not [bool]$config.session_checkpoint.enabled) {
            throw 'managed release requires durable session checkpoints'
        }
        $checkpointRoot = Join-Path $cache 'session-checkpoints'
        $logs = Join-Path ([string]$release.release_root) 'logs'
        New-Item -ItemType Directory -Force -Path $cache, $checkpointRoot, $logs | Out-Null
        $serverArguments = [Collections.Generic.List[string]]::new()
        foreach ($argument in @(
                [string]$release.model_artifact,
                '--host', [string]$config.listen.host,
                '--port', [string]$config.listen.port,
                '--api-key-file', [string]$release.api_key_file,
                '--model-id', [string]$config.model_id,
                '--binary-sha256', [string]$release.binary_sha256,
                '--artifact-sha256', [string]$release.model_artifact_sha256,
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
                '--session-checkpoint-dir', $checkpointRoot,
                '--session-checkpoint-quota-mib',
                    [string]$config.session_checkpoint.quota_mib,
                '--session-checkpoint-staging-mib',
                    [string]$config.session_checkpoint.staging_mib,
                '--request-log-jsonl', (Join-Path $logs 'requests.jsonl'),
                '--log-stats-interval-ms', [string]$config.telemetry.stats_interval_ms
            )) {
            $serverArguments.Add($argument)
        }
        if (-not [bool]$config.engine.cuda_graph) { $serverArguments.Add('--no-cuda-graph') }
        if (-not [bool]$config.engine.prefix_reuse) { $serverArguments.Add('--no-prefix-reuse') }
        if ([bool]$config.reasoning.preserve_thinking) { $serverArguments.Add('--preserve-thinking') }
        if ([bool]$config.engine.vision) { $serverArguments.Add('--vision') }

        $speculativeBackend = [string]$config.speculative.backend
        $draftTokens = [int]$config.speculative.draft_tokens
        if ($speculativeBackend -ceq 'none') {
            if ($draftTokens -ne 0) { throw 'MTP0 configuration must use zero draft tokens' }
        }
        elseif ($speculativeBackend -ceq 'mtp') {
            if ($draftTokens -lt 1 -or $draftTokens -gt 5) {
                throw 'MTP draft tokens must be in [1,5]'
            }
            foreach ($argument in @('--spec', 'mtp', '--draft-tokens', [string]$draftTokens,
                                    '--lm-head-draft')) {
                $serverArguments.Add($argument)
            }
        }
        else {
            throw "unsupported release speculative backend: $speculativeBackend"
        }

        $argumentLine = [string]::Join(' ', @($serverArguments | ForEach-Object {
                    Quote-NativeArgument $_
                }))
        $stdout = Join-Path $logs 'stdout.log'
        $stderr = Join-Path $logs 'stderr.log'
        Assert-FileHash ([string]$release.server_executable) ([string]$release.binary_sha256) 'server executable before launch'
        Assert-FileHash ([string]$release.config_file) ([string]$release.config_sha256) 'server config before launch'
        $process = Start-Process -FilePath ([string]$release.server_executable) -ArgumentList $argumentLine `
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
        if ($null -ne $state) {
            $runtime = Get-RuntimeState
            if ($null -ne $runtime -and [string]$runtime.release_id -ceq [string]$state.active_release) {
                Remove-Item -LiteralPath (Join-Path $StateRoot 'runtime.json') -Force -ErrorAction SilentlyContinue
            }
        }
        if ($null -ne $lock) { $lock.Dispose() }
        if ($ownerLeaseHeld) { Restore-GpuOwnerLease }
    }
}

function Uninstall-ManagedRelease {
    $fullStateRoot = [IO.Path]::GetFullPath($StateRoot).TrimEnd(
        [char[]]@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    )
    if ([string]::IsNullOrWhiteSpace($fullStateRoot) -or
        [string]::Equals($fullStateRoot, [IO.Path]::GetPathRoot($fullStateRoot),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'refusing to uninstall an unsafe lifecycle state root'
    }

    $lock = $null
    $receiptJson = $null
    try {
        $lock = [IO.File]::Open(
            (Join-Path $StateRoot 'install.lock'),
            [IO.FileMode]::OpenOrCreate,
            [IO.FileAccess]::ReadWrite,
            [IO.FileShare]::None
        )
        $state = Get-State
        Assert-NoPreparedRelease $state
        $releaseIds = @($state.releases.PSObject.Properties | ForEach-Object { [string]$_.Name })
        foreach ($releaseId in $releaseIds) {
            $release = Get-Release $state $releaseId
            Assert-CandidateLayout $release $releaseId
            if (Test-PathWithinRoot ([string]$release.model_artifact) $StateRoot) {
                throw 'uninstall refused an operation-owned model reference'
            }
        }

        Stop-ManagedRelease
        Unregister-ScheduledTask -TaskName ([string]$state.task_name) -Confirm:$false -ErrorAction SilentlyContinue
        $receiptJson = ([ordered]@{
                artifact_type = 'ninfer_windows_release_uninstall_receipt'
                schema_version = 1
                status = 'passed'
                uninstalled_releases = $releaseIds
                release_count = $releaseIds.Count
                external_models_deleted = 0
                secret_values_recorded = 0
            } | ConvertTo-Json -Depth 8 -Compress)
    }
    finally {
        if ($null -ne $lock) { $lock.Dispose() }
    }

    $currentDirectory = [IO.Path]::GetFullPath((Get-Location).Path)
    if ((Test-SamePath $currentDirectory $fullStateRoot) -or
        (Test-PathWithinRoot $currentDirectory $fullStateRoot)) {
        Set-Location (Split-Path -Parent $fullStateRoot)
    }
    Remove-Item -LiteralPath $fullStateRoot -Recurse -Force
    Write-Output $receiptJson
}

switch ($Action) {
    'Run' {
        Invoke-Run
    }
    'Status' {
        Get-StatusObject | ConvertTo-Json -Depth 20
    }
    'Start' {
        Start-ManagedRelease
        Get-StatusObject | ConvertTo-Json -Depth 20
    }
    'Stop' {
        Stop-ManagedRelease
        Get-StatusObject | ConvertTo-Json -Depth 20
    }
    'Restart' {
        Assert-NoPreparedRelease (Get-State)
        Stop-ManagedRelease $false
        try {
            Start-ManagedRelease
        }
        catch {
            Restore-GpuOwnerLease
            throw
        }
        Get-StatusObject | ConvertTo-Json -Depth 20
    }
    'Rollback' {
        $state = Get-State
        Assert-NoPreparedRelease $state
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
        Get-StatusObject | ConvertTo-Json -Depth 20
    }
    'Uninstall' {
        Uninstall-ManagedRelease
    }
}
