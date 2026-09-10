[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Run', 'Status', 'Start', 'Stop', 'Restart', 'Rollback', 'Uninstall')]
    [string]$Action,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$StateRoot
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
    # A native mainline release names its lane in the build profile and records the architecture
    # it was compiled for; the two must agree with each other and with the release identity.
    if ([string]$release.build_profile -cnotmatch '^native-v[0-9][0-9A-Za-z.-]*-rtx([0-9]{4})$') {
        throw 'installed release immutable build profile is invalid'
    }
    $laneNumber = $Matches[1]
    if ([string]$release.cuda_architecture -cnotmatch '^sm_[0-9]{2,3}a?$' -or
        [string]$release.deployment_profile -cne ('qwen38-' + $laneNumber + '-native-v' + ([string]$release.build_profile).Substring(8, ([string]$release.build_profile).Length - 16))) {
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
    $policy = @{}
    if ($null -ne $owner.PSObject.Properties['qualified_power_limit_w'] -and $null -ne $owner.qualified_power_limit_w) {
        $policy['QualifiedPowerLimitW'] = [int]$owner.qualified_power_limit_w
    }
    if ($null -ne $owner.PSObject.Properties['prior_power_limit_range_w']) {
        $policy['PriorPowerLimitMinW'] = [int]$owner.prior_power_limit_range_w[0]
        $policy['PriorPowerLimitMaxW'] = [int]$owner.prior_power_limit_range_w[1]
    }
    $output = ((& ([string]$owner.controller_path) -Action $OwnerAction -StateRoot ([string]$owner.state_root) @policy) | Out-String).Trim()
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
    # The lease is a claim, not a record: its absence is the end state. A graceful stop lets the
    # managed wrapper reach its own restore, so both callers can converge on the same result and
    # whichever finishes second must not fail for finding the work already done.
    Remove-Item -LiteralPath (Join-Path $StateRoot 'gpu-owner-lease.json') -Force `
        -ErrorAction SilentlyContinue
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

# A release installed before the stop-event channel existed is stopped the only way it can be:
# by termination. A release that declares the channel also declares the bounded wait its stop
# gets before the controller terminates it anyway.
function Get-ManagedStopPlan([object]$Release) {
    $mode = 'terminate'
    $timeoutSeconds = 30
    $declared = $Release.PSObject.Properties['managed_stop']
    if ($null -ne $declared -and [string]$declared.Value -ceq 'stop-event') {
        $bound = $Release.PSObject.Properties['graceful_stop_timeout_seconds']
        if ($null -eq $bound) {
            throw 'installed release declares a stop event without a bounded graceful wait'
        }
        $timeoutSeconds = [int]$bound.Value
        if ($timeoutSeconds -lt 1 -or $timeoutSeconds -gt 3600) {
            throw 'installed release graceful-stop wait is out of range'
        }
        $mode = 'stop-event'
    }
    return [ordered]@{ mode = $mode; timeout_seconds = $timeoutSeconds }
}

# The name is only usable when this exact launch published it for this exact release. A stale,
# older-schema, or foreign record yields nothing, and the stop falls back to termination rather
# than signalling an object it cannot attribute.
function Get-RuntimeStopEvent([object]$Runtime, [string]$ReleaseId) {
    if ($null -eq $Runtime) { return $null }
    foreach ($field in @('artifact_type', 'schema_version', 'release_id', 'managed_stop',
                         'stop_event')) {
        if ($null -eq $Runtime.PSObject.Properties[$field]) { return $null }
    }
    if ([string]$Runtime.artifact_type -cne 'ninfer_windows_runtime_state' -or
        [int]$Runtime.schema_version -ne 2 -or
        [string]$Runtime.release_id -cne $ReleaseId -or
        [string]$Runtime.managed_stop -cne 'stop-event') {
        return $null
    }
    $name = [string]$Runtime.stop_event
    if ($name -cnotmatch '^(Global|Local)\\NInfer-Serve-Stop-[0-9a-f]{32}$') { return $null }
    return $name
}

function Request-ManagedStop([string]$Name) {
    $handle = $null
    try {
        $handle = [Threading.EventWaitHandle]::OpenExisting($Name)
    }
    catch [Threading.WaitHandleCannotBeOpenedException] { return $false }
    try { return $handle.Set() } finally { $handle.Dispose() }
}

function Write-ManagedStopReceipt([object]$Receipt) {
    Write-JsonAtomic (Join-Path $StateRoot 'last-stop.json') $Receipt
}

function Get-ManagedStopReceipt {
    $path = Join-Path $StateRoot 'last-stop.json'
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
        last_stop = Get-ManagedStopReceipt
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

function Wait-TaskIdle([string]$TaskName, [DateTime]$DeadlineUtc) {
    while ([DateTime]::UtcNow -lt $DeadlineUtc) {
        $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        if ($null -eq $task -or [string]$task.State -cne 'Running') { return $true }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

# A managed launch owns two pieces of shared state while it runs: the run lock and the
# GPU-owner lease. Terminating it never released either, so the controller could always follow
# straight on; a server that exits gracefully lets its wrapper reach its own cleanup, and a
# controller that restored the lease or started the next release in that window would race it.
function Wait-ManagedWrapperExit([string]$TaskName, [DateTime]$DeadlineUtc) {
    if (-not (Wait-TaskIdle $TaskName $DeadlineUtc)) { return $false }
    $lockPath = Join-Path $StateRoot 'run.lock'
    while ([DateTime]::UtcNow -lt $DeadlineUtc) {
        try {
            $lock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate,
                                    [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
            $lock.Dispose()
            return $true
        }
        catch [IO.IOException] { Start-Sleep -Milliseconds 250 }
    }
    return $false
}

# The termination fallback, in order and best-effort: stop the task, then force whatever is
# still the owned process. Neither failure may skip the one after it - a scheduler API error
# that aborted the stop would leave a serving process alive and no record of why.
function Stop-LaunchNow([object]$Release, [string]$TaskName, [object]$Receipt) {
    $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if ($null -ne $task -and [string]$task.State -ceq 'Running') {
        try { Stop-ScheduledTask -TaskName $TaskName }
        catch { $Receipt.reason = "scheduled-task stop failed: $($_.Exception.Message)" }
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($null -eq (Get-OwnedProcess (Get-RuntimeState) $Release)) { return }
        Start-Sleep -Milliseconds 250
    }
    $live = Get-OwnedProcess (Get-RuntimeState) $Release
    if ($null -ne $live) {
        try {
            Stop-Process -Id $live.Id -Force
            $live.WaitForExit(10000) | Out-Null
            $Receipt.forced = $true
        }
        catch { $Receipt.reason = "force termination failed: $($_.Exception.Message)" }
    }
}

function Stop-ManagedProcess {
    $state = Get-State
    $release = Get-Release $state ([string]$state.active_release)
    $runtime = Get-RuntimeState
    $taskName = [string]$state.task_name
    $plan = Get-ManagedStopPlan $release
    $eventName = Get-RuntimeStopEvent $runtime ([string]$state.active_release)
    $owned = Get-OwnedProcess $runtime $release
    $started = [DateTime]::UtcNow
    $receipt = [ordered]@{
        artifact_type = 'ninfer_windows_managed_stop_receipt'
        schema_version = 1
        release_id = [string]$state.active_release
        mode = [string]$plan.mode
        graceful_wait_seconds = [int]$plan.timeout_seconds
        outcome = 'already_stopped'
        forced = $false
        exit_code = $null
        wrapper_result = $null
        reason = $null
        pid = if ($null -eq $owned) { $null } else { [int]$owned.Id }
        started_utc = $started.ToString('o')
        completed_utc = $null
        elapsed_seconds = 0.0
    }

    if ($null -ne $owned -and $plan.mode -ceq 'stop-event' -and $null -ne $eventName) {
        $deadline = $started.AddSeconds([int]$plan.timeout_seconds)
        # The wrapper publishes runtime.json before the server creates its event, so a stop that
        # lands during startup retries until the object exists or the process is gone.
        $signalled = $false
        while (-not $signalled -and -not $owned.HasExited -and [DateTime]::UtcNow -lt $deadline) {
            $signalled = Request-ManagedStop $eventName
            if (-not $signalled) { Start-Sleep -Milliseconds 250 }
        }
        if ($signalled) {
            $receipt.outcome = 'graceful'
            $remaining = [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalMilliseconds)
            $owned.WaitForExit([int][Math]::Max(0, $remaining)) | Out-Null
            if (-not $owned.HasExited) {
                $receipt.reason = "the server did not exit within $([int]$plan.timeout_seconds)s of the stop request"
            }
            else {
                # A process this controller did not start does not always expose an exit code:
                # Get-Process hands back a handle without the rights .NET needs, and reading it
                # yields $null rather than 0. What is observable is that the request was
                # delivered and the process exited; classify on that, record the code when the
                # host gives one, and never treat its absence as a nonzero exit. The flush
                # itself is proven by the session that comes back, not by an exit code.
                $exitCode = $null
                try { $exitCode = $owned.ExitCode } catch { $exitCode = $null }
                if ($null -ne $exitCode) {
                    $receipt.exit_code = [int]$exitCode
                    if ([int]$exitCode -ne 0) {
                        $receipt.outcome = 'graceful_nonzero_exit'
                        $receipt.reason = 'the server exited nonzero after the stop request'
                    }
                }
            }
        }
        else {
            $receipt.outcome = 'signal_failed'
            $receipt.reason = 'the published stop event could not be opened'
        }
    }
    elseif ($null -ne $owned) {
        $receipt.outcome = 'terminated'
        if ($plan.mode -ceq 'stop-event') {
            $receipt.reason = 'this launch published no usable stop event'
        }
    }

    # The fallback reads state now, not the snapshot this call opened with: a stop that raced a
    # start would otherwise find no owned process in a stale record, skip both the task stop and
    # the force-kill, and return while the newly launched server keeps serving. It also gives a
    # wrapper that is merely unwinding its grace first, so a graceful stop does not kill the
    # cleanup it just asked for. The receipt is written on every path.
    try {
        if ($null -ne (Get-OwnedProcess (Get-RuntimeState) $release)) {
            if ($receipt.outcome -ceq 'graceful') { $receipt.outcome = 'graceful_wait_expired' }
            if ($receipt.outcome -ceq 'already_stopped') {
                $receipt.outcome = 'terminated'
                $receipt.reason = 'a launch appeared while this stop was in progress'
            }
            Stop-LaunchNow $release $taskName $receipt
        }

        # A stop returns only once no wrapper still holds the run lock or the GPU-owner lease,
        # whether this call signalled the server, terminated it, or found it already gone while
        # its wrapper was still unwinding.
        if (-not (Wait-ManagedWrapperExit $taskName ([DateTime]::UtcNow.AddSeconds(120)))) {
            $receipt.reason = 'the managed wrapper did not release the run lock after its server stopped'
            Stop-LaunchNow $release $taskName $receipt
            if (-not (Wait-ManagedWrapperExit $taskName ([DateTime]::UtcNow.AddSeconds(60)))) {
                $receipt.outcome = 'wrapper_cleanup_stalled'
                throw 'the managed wrapper did not release the run lock after its server stopped'
            }
        }
        if ($null -ne (Get-OwnedProcess (Get-RuntimeState) $release)) {
            $receipt.outcome = 'still_running'
            $receipt.reason = 'the owned server process survived termination'
            throw 'owned server process did not stop'
        }

        # The wrapper does have a real child handle, so its own result is the observable record
        # of how the server exited - including a shutdown that could not save every live
        # session, which exits nonzero. Task Scheduler result codes also carry the task's own
        # lifecycle, so a terminated task never reports zero and is not judged by it.
        $info = Get-ScheduledTaskInfo -TaskName $taskName -ErrorAction SilentlyContinue
        $receipt.wrapper_result = if ($null -eq $info) { $null } else { [int]$info.LastTaskResult }
        if ($receipt.outcome -ceq 'graceful' -and $null -ne $receipt.wrapper_result -and
            [int]$receipt.wrapper_result -ne 0) {
            $receipt.outcome = 'graceful_incomplete_shutdown'
            $receipt.reason = "the server reported an unclean shutdown (wrapper result $([int]$receipt.wrapper_result)); a live session may not have been saved"
        }
        Remove-Item -LiteralPath (Join-Path $StateRoot 'runtime.json') -Force -ErrorAction SilentlyContinue
    }
    finally {
        $receipt.completed_utc = [DateTime]::UtcNow.ToString('o')
        $receipt.elapsed_seconds = [Math]::Round(([DateTime]::UtcNow - $started).TotalSeconds, 3)
        Write-ManagedStopReceipt $receipt
    }
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
    # A server that exited on its own - gracefully, or by crashing - leaves its wrapper restoring
    # the GPU owner and releasing the run lock. Acquiring the lease across that is the same race
    # a stop closes, and the next launch would find its own lock held.
    if (-not (Wait-ManagedWrapperExit ([string]$state.task_name) ([DateTime]::UtcNow.AddSeconds(120)))) {
        throw 'a managed wrapper still holds the run lock; the previous launch has not finished'
    }

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
                '--device-state-slots', [string]$config.context_cache.device_state_slots,
                '--host-state-slots', [string]$config.context_cache.host_state_slots,
                '--host-kv-mib', [string]$config.context_cache.host_kv_mib,
                '--max-private-continuations', [string]$config.context_cache.max_private_continuations,
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
        if (-not [bool]$config.reasoning.thinking) { $serverArguments.Add('--no-thinking') }
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

        # One kernel-object name per launch, never reused: the server refuses a name that already
        # exists, and only a release that declares the channel is asked to honour it.
        $stopPlan = Get-ManagedStopPlan $release
        $stopEventName = $null
        if ([string]$stopPlan.mode -ceq 'stop-event') {
            $stopEventName = 'Global\NInfer-Serve-Stop-' + [Guid]::NewGuid().ToString('N')
            $serverArguments.Add('--stop-event')
            $serverArguments.Add($stopEventName)
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
                artifact_type = 'ninfer_windows_runtime_state'
                schema_version = 2
                release_id = [string]$state.active_release
                pid = $process.Id
                start_time_utc_ticks = $process.StartTime.ToUniversalTime().Ticks
                managed_stop = [string]$stopPlan.mode
                stop_event = $stopEventName
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

# Two controller invocations that mutate the lifecycle must not interleave: a stop that restored
# the GPU owner while a start was acquiring the lease would leave the next server running with
# the owner's power cap. The wrapper's run lock is a different claim - held for a launch's whole
# life - so `Run` deliberately does not take this one.
function Invoke-WithActionLock([ScriptBlock]$Body) {
    $path = Join-Path $StateRoot 'action.lock'
    $deadline = [DateTime]::UtcNow.AddSeconds(300)
    $lock = $null
    while ($null -eq $lock) {
        try {
            $lock = [IO.File]::Open($path, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite,
                                    [IO.FileShare]::None)
        }
        catch [IO.IOException] {
            if ([DateTime]::UtcNow -ge $deadline) {
                throw 'another managed lifecycle action is still in progress'
            }
            Start-Sleep -Milliseconds 500
        }
    }
    try { & $Body } finally { $lock.Dispose() }
}

switch ($Action) {
    'Run' {
        Invoke-Run
    }
    'Status' {
        Get-StatusObject | ConvertTo-Json -Depth 20
    }
    'Start' {
        Invoke-WithActionLock { Start-ManagedRelease }
        Get-StatusObject | ConvertTo-Json -Depth 20
    }
    'Stop' {
        Invoke-WithActionLock { Stop-ManagedRelease }
        Get-StatusObject | ConvertTo-Json -Depth 20
    }
    'Restart' {
        Invoke-WithActionLock {
            Assert-NoPreparedRelease (Get-State)
            Stop-ManagedRelease $false
            try {
                Start-ManagedRelease
            }
            catch {
                Restore-GpuOwnerLease
                throw
            }
        }
        Get-StatusObject | ConvertTo-Json -Depth 20
    }
    'Rollback' {
        Invoke-WithActionLock {
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
        }
        Get-StatusObject | ConvertTo-Json -Depth 20
    }
    'Uninstall' {
        Invoke-WithActionLock { Uninstall-ManagedRelease }
    }
}
