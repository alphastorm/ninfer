[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Run', 'Status', 'Start', 'Stop', 'Restart', 'Rollback', 'Reconcile', 'Uninstall')]
    [string]$Action,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$StateRoot,

    [string]$ReleaseId,

    [switch]$AllowHeld
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (($PSBoundParameters.ContainsKey('ReleaseId') -and $Action -cne 'Reconcile') -or
    ($Action -ceq 'Reconcile' -and [string]::IsNullOrWhiteSpace($ReleaseId)) -or
    ($PSBoundParameters.ContainsKey('AllowHeld') -and $Action -cne 'Stop')) {
    throw 'ReleaseId is required only for Reconcile; AllowHeld is valid only for Stop'
}
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

# engine.gpu_keep_warm_ms is optional: a release packaged before the runtime learned
# --gpu-keep-warm-ms has no such field, and 0 keeps the GPU's idle power states.
function Get-GpuKeepWarmMs([object]$Config) {
    $property = $Config.engine.PSObject.Properties['gpu_keep_warm_ms']
    if ($null -eq $property) { return 0 }
    $value = $property.Value
    if (-not ($value -is [int] -or $value -is [long]) -or $value -lt 0 -or $value -gt [int]::MaxValue) {
        throw 'release server config engine.gpu_keep_warm_ms must be an integer in [0, 2147483647]'
    }
    return [int]$value
}

function Get-Fp32HeadLogits([object]$Config) {
    $property = $Config.engine.PSObject.Properties['fp32_head_logits']
    if ($null -eq $property) { return $false }
    if ($property.Value -isnot [bool]) {
        throw 'release server config engine.fp32_head_logits must be a boolean'
    }
    return $property.Value
}

function Add-ContextArguments([Collections.Generic.List[string]]$Arguments, [object]$Config) {
    if ([int]$Config.schema_version -eq 4) {
        # Master uses one shared Host byte budget. Never reinterpret the fork's slot/KV
        # budgets here: the schema-4 profile binds their layout-aware conversion at release.
        foreach ($obsolete in @('host_state_slots', 'host_kv_mib', 'max_private_continuations',
                                'max_shared_prefixes', 'max_long_anchors_per_continuation',
                                'max_cache_markers_per_request')) {
            if ($null -ne $Config.context_cache.PSObject.Properties[$obsolete]) {
                throw "schema-4 configuration contains obsolete context policy: $obsolete"
            }
        }
        $hostMib = [string]$Config.context_cache.host_context_mib
        if ($hostMib -cnotmatch '^[0-9]+(?:\.[0-9]+)?$' -or [decimal]$hostMib -le 0) {
            throw 'schema-4 configuration requires a positive exact host_context_mib'
        }
        foreach ($argument in @('--host-context-mib', $hostMib)) { $Arguments.Add($argument) }
        $headroom = $Config.engine.admission_headroom_mib
        if (-not ($headroom -is [int] -or $headroom -is [long]) -or
            $headroom -lt 256 -or $headroom -gt [int]::MaxValue) {
            throw 'schema-4 configuration requires admission_headroom_mib >= 256'
        }
        foreach ($argument in @('--admission-headroom-mib', [string]$headroom)) { $Arguments.Add($argument) }
        if ($Config.session_checkpoint.require_origin_auth -isnot [bool] -or
            -not $Config.session_checkpoint.require_origin_auth) {
            throw 'schema-4 configuration requires strict checkpoint origin authentication'
        }
        $Arguments.Add('--session-checkpoint-require-origin-auth')
    }
    elseif ([int]$Config.schema_version -eq 3) {
        # Rollback launches an older release's own binary and isolated cache. These flags
        # are capabilities of that schema, never aliases accepted by the rebased server.
        foreach ($argument in @(
                '--host-state-slots', [string]$Config.context_cache.host_state_slots,
                '--max-private-continuations', [string]$Config.context_cache.max_private_continuations
            )) { $Arguments.Add($argument) }
        $hostKv = $Config.context_cache.PSObject.Properties['host_kv_mib']
        if ($null -ne $hostKv) {
            foreach ($argument in @('--host-kv-mib', [string]$hostKv.Value)) { $Arguments.Add($argument) }
        }
    }
    else { throw 'unsupported release server configuration schema' }
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
    if ([IO.Path]::GetFileName($Path) -cin @('runtime.json', 'shutdown-holds.json', 'last-stop.json') -or
        [IO.Path]::GetFileName($Path) -clike 'shutdown-reconcile-*.json') { Assert-HoldsLock }
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    [IO.File]::WriteAllText($temporary, ($Value | ConvertTo-Json -Depth 24),
                           [Text.UTF8Encoding]::new($false))
    # Keep an interrupted first publication visible to strict readers. Replacement never
    # unlinks a destination: concurrent readers see the complete old or complete new file.
    if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary, $Path, [NullString]::Value) }
    else { [IO.File]::Move($temporary, $Path) }
}

function Invoke-WithHoldsLock([ScriptBlock]$Body) {
    if (Get-Variable NInferHoldsHeld -Scope Script -ValueOnly -ErrorAction SilentlyContinue) {
        throw 'launch-attribution lock is not reentrant'
    }
    $handle = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    while ($null -eq $handle) {
        try {
            $handle = [IO.File]::Open((Join-Path $StateRoot 'holds.lock'),
                [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        }
        catch [IO.IOException] {
            if ([DateTime]::UtcNow -ge $deadline) { throw 'launch-attribution lock acquisition timed out' }
            Start-Sleep -Milliseconds 100
        }
    }
    $script:NInferHoldsHeld = $true
    try { & $Body }
    finally { $script:NInferHoldsHeld = $false; $handle.Dispose() }
}

function Assert-HoldsLock {
    if (-not (Get-Variable NInferHoldsHeld -Scope Script -ValueOnly -ErrorAction SilentlyContinue)) {
        throw 'launch-attribution mutation requires holds.lock'
    }
}

function Read-AttributionJson([string]$Name) {
    Assert-HoldsLock
    $path = Join-Path $StateRoot $Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        if ((Test-Path -LiteralPath $path) -or
            @(Get-ChildItem -LiteralPath $StateRoot -Filter "$Name.*.tmp" -Force).Count -ne 0) {
            throw "missing or invalid attribution destination with interrupted publication: $Name"
        }
        return $null
    }
    $record = Read-JsonFile $path
    if ($null -eq $record -or $record -isnot [pscustomobject]) {
        throw "invalid attribution object: $Name"
    }
    return $record
}

function Get-AttributionField([object]$Record, [string]$Name) {
    if ($null -eq $Record) { return $null }
    $property = $Record.PSObject.Properties[$Name]
    if ($null -ne $property) { return ,$property.Value }
    return $null
}

function Get-LaunchRoot([string]$Id) {
    if ($Id -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') { throw 'invalid launch release id' }
    return [IO.Path]::GetFullPath((Join-Path (Join-Path $StateRoot 'releases') $Id))
}

function Get-LaunchKey([object]$Runtime) {
    $key = [string](Get-AttributionField $Runtime 'launch_id')
    if ($key -cnotmatch '^[0-9a-f]{32}$') {
        if ([string](Get-AttributionField $Runtime 'managed_stop') -ceq 'stop-event' -or
            -not [string]::IsNullOrEmpty($key)) { throw 'invalid stop-event launch id' }
        return $null
    }
    return $key
}

function Get-RuntimeStrict {
    $runtime = Read-AttributionJson 'runtime.json'
    if ($null -eq $runtime) { return $null }
    if ([string](Get-AttributionField $runtime 'artifact_type') -cne 'ninfer_windows_runtime_state' -or
        (Get-AttributionField $runtime 'schema_version') -cnotin @(1, 2) -or
        $null -eq $runtime.PSObject.Properties['pid'] -or
        $null -eq $runtime.PSObject.Properties['start_time_utc_ticks']) {
        throw 'invalid runtime attribution envelope'
    }
    Get-LaunchRoot ([string]$runtime.release_id) | Out-Null
    Get-LaunchKey $runtime | Out-Null
    return $runtime
}

function Assert-SameLaunch([object]$Expected, [object]$Current) {
    if ($null -eq $Expected -or $null -eq $Current) {
        if ($null -ne $Expected -or $null -ne $Current) { throw 'runtime launch changed during proof' }
        return
    }
    if ([string]$Expected.release_id -cne [string]$Current.release_id -or
        [string](Get-LaunchKey $Expected) -cne [string](Get-LaunchKey $Current)) {
        throw 'runtime launch changed during proof'
    }
    if ($null -eq (Get-LaunchKey $Expected) -and
        ($Expected.pid -ne $Current.pid -or
         $Expected.start_time_utc_ticks -ne $Current.start_time_utc_ticks)) {
        throw 'legacy runtime process attribution changed during proof'
    }
}

function Get-ShutdownHolds {
    $envelope = Read-AttributionJson 'shutdown-holds.json'
    if ($null -eq $envelope) {
        return [pscustomobject]@{artifact_type='ninfer_windows_shutdown_holds';schema_version=1;holds=@()}
    }
    if ([string](Get-AttributionField $envelope 'artifact_type') -cne 'ninfer_windows_shutdown_holds' -or
        (Get-AttributionField $envelope 'schema_version') -cne 1 -or
        (Get-AttributionField $envelope 'holds') -isnot [array]) { throw 'invalid shutdown holds envelope' }
    $keys = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($entry in $envelope.holds) {
        $key = [string](Get-AttributionField $entry 'launch_id')
        $id = [string](Get-AttributionField $entry 'release_id')
        $root = Get-LaunchRoot $id
        if ($key -cnotmatch '^[0-9a-f]{32}$' -or -not $keys.Add($key)) {
            throw 'invalid or duplicate shutdown hold launch key'
        }
        if (-not [string]::Equals([IO.Path]::GetFullPath([string]$entry.release_root),
                                  $root, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'shutdown hold canonical root mismatch'
        }
        if (
            [string](Get-LaunchKey $entry.runtime) -cne $key -or
            [string]$entry.runtime.release_id -cne $id -or
            [string]$entry.runtime.managed_stop -cne 'stop-event' -or
            [string]$entry.stop_receipt.artifact_type -cne 'ninfer_windows_managed_stop_receipt' -or
            $entry.stop_receipt.schema_version -cne 1 -or
            [string]$entry.stop_receipt.launch_id -cne $key -or
            [string]$entry.stop_receipt.release_id -cne $id -or
            [string]$entry.stop_receipt.mode -cne 'stop-event') {
            throw 'invalid or conflicting shutdown hold runtime/receipt binding'
        }
    }
    return $envelope
}

function Get-StickyStopReceipt {
    $receipt = Read-AttributionJson 'last-stop.json'
    if ($null -ne $receipt -and
        ([string](Get-AttributionField $receipt 'artifact_type') -cne 'ninfer_windows_managed_stop_receipt' -or
         (Get-AttributionField $receipt 'schema_version') -cne 1)) { throw 'invalid managed-stop receipt' }
    return $receipt
}

function Remove-RuntimeForLaunch([object]$Expected) {
    Assert-HoldsLock
    $current = Get-RuntimeStrict
    Assert-SameLaunch $Expected $current
    if ($null -ne $current) { Remove-Item -LiteralPath (Join-Path $StateRoot 'runtime.json') -Force }
}

function Enter-LaunchFence([string]$Name, [int]$TimeoutSeconds = 60) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ($true) {
        try {
            return [IO.File]::Open((Join-Path $StateRoot ($Name + '.lock')),
                [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        }
        catch [IO.IOException] {
            if ([DateTime]::UtcNow -ge $deadline) {
                throw 'Stop the serving launch, then retry repair; managed launch exclusion timed out'
            }
            Start-Sleep -Milliseconds 100
        }
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

# The server loads the DLLs beside its executable ahead of any system location. Install accepted
# them only inside the package's closed inner manifest, which it retains in receipts bound to the
# release record by hash; every launch holds bin/*.dll to that manifest again, so a DLL
# replaced, removed, or planted after install never loads.
function Assert-AppLocalDlls([object]$Release) {
    $manifestPath = Join-Path ([string]$Release.receipts_root) 'SHA256SUMS.txt'
    Assert-FileHash $manifestPath ([string]$Release.inner_checksums_sha256) 'retained package manifest'
    $expected = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in [IO.File]::ReadAllLines($manifestPath, [Text.Encoding]::ASCII)) {
        if ($line -cnotmatch '^([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._/-]*)$') {
            throw 'retained package manifest contains a malformed entry'
        }
        $digest = [string]$Matches[1]
        if ([string]$Matches[2] -notmatch '^bin/([^/]+\.dll)$') { continue }
        $name = [string]$Matches[1]
        if ($expected.ContainsKey($name)) { throw "retained package manifest lists an app-local DLL twice: $name" }
        $expected[$name] = $digest
    }
    $present = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path ([string]$Release.release_root) 'bin') -File -Force)) {
        if ([IO.Path]::GetExtension($file.Name) -ine '.dll') { continue }
        if (-not $expected.ContainsKey($file.Name)) {
            throw "app-local DLL is not in the retained package manifest: $($file.Name)"
        }
        Assert-FileHash $file.FullName $expected[$file.Name] "app-local DLL $($file.Name)"
        [void]$present.Add($file.Name)
    }
    foreach ($name in $expected.Keys) {
        if (-not $present.Contains($name)) { throw "app-local DLL is missing: $name" }
    }
}

function Assert-SelectedGpuIdentity([object]$Release) {
    foreach ($field in @('gpu_uuid', 'gpu_name')) {
        if ($null -eq $Release.PSObject.Properties[$field]) {
            throw 'selected GPU identity is missing; reinstall the release'
        }
    }
    if (Test-Path Env:CUDA_VISIBLE_DEVICES) {
        throw 'CUDA_VISIBLE_DEVICES must be absent for bound GPU ordinal identity'
    }
    if ([string]$Release.gpu_uuid -cnotmatch '^GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$') {
        throw 'selected GPU UUID is invalid'
    }
    $rows = @(& (Get-TrustedNvidiaSmiPath) -i ([string]$Release.gpu_uuid) --query-gpu=index,uuid,name,pci.bus_id --format=csv,noheader,nounits 2>&1)
    if ($LASTEXITCODE -ne 0 -or $rows.Count -ne 1) { throw 'selected GPU identity query failed' }
    $parts = @(([string]$rows[0] -split ',') | ForEach-Object { $_.Trim() })
    $rowIndex = -1
    if ($parts.Count -ne 4 -or -not [int]::TryParse($parts[0], [ref]$rowIndex) -or $rowIndex -lt 0 -or
        $parts[3] -notmatch '^[0-9a-f]{8}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$') {
        throw 'selected GPU identity query returned an invalid row'
    }
    $pci = $Release.PSObject.Properties['gpu_pci_bus_id']
    if ($parts[1] -cne [string]$Release.gpu_uuid -or $parts[2] -cne [string]$Release.gpu_name -or
        ($null -ne $pci -and $parts[3].ToUpperInvariant() -cne [string]$pci.Value)) {
        throw 'selected GPU identity changed after install'
    }
    return $rowIndex
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
    if (-not (Get-Command -Name ([string]$owner.controller_path)).Parameters.ContainsKey('GpuUuid')) {
        throw 'GPU-owner controller predates UUID binding; reinstall with the bundled controller'
    }
    return $owner
}

function Invoke-GpuOwner([object]$State, [ValidateSet('status', 'stop', 'start')][string]$OwnerAction) {
    $owner = Get-GpuOwner $State
    if ($null -eq $owner) { return $null }
    $release = Get-Release $State ([string]$State.active_release)
    $policy = @{ GpuUuid = [string]$release.gpu_uuid }
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
    $pci = $release.PSObject.Properties['gpu_pci_bus_id']
    if ($null -eq $status.PSObject.Properties['gpu_uuid'] -or
        [string]$status.gpu_uuid -cne [string]$release.gpu_uuid -or
        $null -eq $status.PSObject.Properties['gpu_pci_bus_id'] -or
        ($null -ne $pci -and [string]$status.gpu_pci_bus_id -cne [string]$pci.Value)) {
        throw 'GPU-owner status does not attest the bound UUID and PCI identity'
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
        }
        $lease.release_id = [string]$state.active_release
        Write-JsonAtomic $leasePath $lease
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
    # The lease is handed back only once no server it was taken for still runs: restoring the
    # owner under a live server lifts the safe cap that server runs at, and the claim is lost.
    $runtime = Invoke-WithHoldsLock { Get-RuntimeStrict }
    if ($null -ne $runtime) {
        $runtimeRelease = Resolve-RuntimeRelease $runtime $state
        if ($null -ne (Get-OwnedProcess $runtime $runtimeRelease)) {
            throw 'the GPU-owner lease stays held while the release server still runs'
        }
        Assert-NoReleaseProcess $runtimeRelease
    }
    Assert-NoReleaseProcess (Get-Release $state ([string]$state.active_release))
    $leaseId = [string](Get-AttributionField $lease 'release_id')
    if (-not [string]::IsNullOrEmpty($leaseId)) {
        Assert-NoReleaseProcess ([pscustomobject]@{
            release_root = Get-LaunchRoot $leaseId
            server_executable = Join-Path (Join-Path (Get-LaunchRoot $leaseId) 'bin') 'ninfer-serve.exe'
        })
    }
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
    if ($null -eq $cim -or [string]::IsNullOrWhiteSpace([string](Get-AttributionField $cim 'ExecutablePath'))) { return $null }
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

# Absence of a PID record is not proof of exit: a wrapper can die between spawn and publication.
# An image census cannot safely adopt/kill that child, but it can withhold ownership and cleanup.
function Assert-NoReleaseProcess([object]$Release) {
    $expected = [IO.Path]::GetFullPath([string]$Release.server_executable)
    $root = [string](Get-AttributionField $Release 'release_root')
    if ([string]::IsNullOrWhiteSpace($root)) { $root = Split-Path -Parent (Split-Path -Parent $expected) }
    foreach ($entry in @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop)) {
        if ([string]::IsNullOrWhiteSpace([string](Get-AttributionField $entry 'ExecutablePath'))) {
            $name = $entry.PSObject.Properties['Name']
            if ($null -eq $name -or [string]::IsNullOrWhiteSpace([string]$name.Value) -or
                [string]$name.Value -ieq [IO.Path]::GetFileName($expected)) {
                throw 'cannot exclude an unattributed release server with an unreadable image path'
            }
            continue
        }
        if (Test-PathWithinRoot ([string]$entry.ExecutablePath) $root) {
            throw "unattributed release server still runs: $($entry.ProcessId); retain runtime and GPU-owner lease until exit is proven"
        }
    }
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

# Returns 'signalled', 'absent' (the object does not exist yet, retry), or 'failed:<reason>'.
# Nothing here throws: a failure to deliver the request is a reason to terminate, never a
# reason to return with the server still running.
function Request-ManagedStop([string]$Name) {
    $handle = $null
    try {
        $handle = [Threading.EventWaitHandle]::OpenExisting($Name)
    }
    catch [Threading.WaitHandleCannotBeOpenedException] { return 'absent' }
    catch { return "failed: $($_.Exception.GetType().Name): $($_.Exception.Message)" }
    try {
        if ($handle.Set()) { return 'signalled' }
        return 'failed: SetEvent returned false'
    }
    catch { return "failed: $($_.Exception.GetType().Name): $($_.Exception.Message)" }
    finally { $handle.Dispose() }
}

function Write-ManagedStopReceipt([object]$Receipt) {
    return Invoke-WithHoldsLock {
        $old = Get-StickyStopReceipt
        $current = Get-RuntimeStrict
        $key = [string](Get-AttributionField $Receipt 'launch_id')
        if ($null -ne $current -and -not [string]::IsNullOrEmpty($key) -and
            [string](Get-LaunchKey $current) -ceq $key -and
            [string](Get-AttributionField $old 'launch_id') -ceq $key -and
            (Test-NonCleanStopReceipt $old)) {
            if ([string]$old.release_id -cne [string]$Receipt.release_id) {
                throw 'conflicting sticky receipt release binding'
            }
            return $old
        }
        Write-JsonAtomic (Join-Path $StateRoot 'last-stop.json') $Receipt
        return $Receipt
    }
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
    $config = Read-JsonFile ([string]$Release.config_file)
    $expectedSchema = if ([int]$config.schema_version -eq 4) { 2 } else { 1 }
    if ($Status.artifact_type -cne 'ninfer_server_status' -or [int]$Status.schema_version -ne $expectedSchema -or
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
        upstream_base_sha = [string]$Release.upstream_base_sha
        patch_stack_sha = [string]$Release.patch_stack_sha
        build_profile = [string]$Release.build_profile
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
    if ($identity.source_dirty -isnot [bool] -or $identity.source_dirty -or
        [string]$identity.model_id -cne [string]$config.model_id -or
        [string]$Status.runtime.public_model_id -cne [string]$config.model_id) {
        throw 'server runtime identity mismatch'
    }
    foreach ($field in @('max_concurrency', 'running', 'prefilling', 'decode_ready', 'waiting',
                         'materializing', 'capture_pending', 'committed_decode_tokens',
                         'computed_prefill_tokens')) {
        if ($null -eq $Status.scheduler.PSObject.Properties[$field] -or
            $null -eq $Status.scheduler.$field) {
            throw "server status is missing scheduler.$field"
        }
    }
    if ($null -eq $Status.cache.reused_prompt_tokens -or
        [string]::IsNullOrWhiteSpace([string]$Status.cache.last_selection.path)) {
        throw 'server status is missing real cache reuse telemetry'
    }
}

function Test-ServerStartGpu([string]$RequestLog, [string]$ExpectedUuid, [Int64]$StartOffset,
    [Int64]$CreationTicks, [int]$ServerPid, [int]$ExpectedDevice, [string]$ExpectedName,
    [int]$ExpectedSchema) {
    if (-not (Test-Path -LiteralPath $RequestLog -PathType Leaf)) { return $false }
    if ($CreationTicks -ne 0 -and (Get-Item -LiteralPath $RequestLog).CreationTimeUtc.Ticks -ne $CreationTicks) {
        throw 'request log was replaced after launch'
    }
    $stream = [IO.File]::Open($RequestLog, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        if ($StartOffset -lt 0 -or $stream.Length -lt $StartOffset) { throw 'request log was truncated after launch' }
        [void]$stream.Seek($StartOffset, [IO.SeekOrigin]::Begin)
        $bytes = [byte[]]::new([int][Math]::Min(1MB, $stream.Length - $StartOffset))
        $count = 0
        while ($count -lt $bytes.Length) {
            $read = $stream.Read($bytes, $count, $bytes.Length - $count)
            if ($read -eq 0) { break }
            $count += $read
        }
        $text = [Text.Encoding]::UTF8.GetString($bytes, 0, $count)
    }
    finally { $stream.Dispose() }
    # Ignore an incomplete trailing JSONL record. Only bytes appended after this launch's
    # captured boundary can attest it; historical server_start records are never examined.
    $end = $text.LastIndexOf([char]10)
    if ($end -ge 0) {
        foreach ($line in ($text.Substring(0, $end) -split "`n")) {
            if ([string]::IsNullOrWhiteSpace($line)) { continue }
            $record = $line | ConvertFrom-Json
            $event = $record.PSObject.Properties['event']
            if ($null -eq $event -or [string]$event.Value -cne 'server_start') { continue }
            if ([string]$record.artifact_type -cne 'ninfer_serve_request_log' -or
                ($ExpectedSchema -ne 0 -and [int]$record.schema_version -ne $ExpectedSchema)) {
                throw 'server_start request-log schema mismatch'
            }
            $instance = $record.PSObject.Properties['server_instance_id']
            if ($null -eq $instance -or [string]$instance.Value -cnotmatch ("^serve-$ServerPid-[0-9]+$")) {
                throw 'server_start does not belong to the launched process'
            }
            $environment = $record.PSObject.Properties['environment']
            if ($null -eq $environment -or $null -eq $environment.Value) { throw 'missing server_start GPU identity' }
            $uuid = $environment.Value.PSObject.Properties['gpu_uuid']
            if ($null -eq $uuid -or [string]$uuid.Value -cne $ExpectedUuid) {
                throw 'server_start GPU UUID differs from the bound GPU'
            }
            if ($null -eq $environment.Value.PSObject.Properties['device'] -or
                [int]$environment.Value.device -ne $ExpectedDevice -or
                [string]$environment.Value.gpu_name -cne $ExpectedName) {
                throw 'server_start device or GPU name differs from the bound GPU'
            }
            return $true
        }
    }
    if ($count -eq 1MB) { throw 'server_start GPU evidence exceeded the 1 MiB search bound' }
    return $false
}

function Assert-RuntimeGpuReady([object]$Release) {
    $runtime = Get-RuntimeState
    if ($null -eq (Get-OwnedProcess $runtime $Release) -or
        $null -eq $runtime.PSObject.Properties['gpu_uuid_verified'] -or
        -not [bool]$runtime.gpu_uuid_verified -or
        $null -eq $runtime.PSObject.Properties['gpu_uuid'] -or
        [string]$runtime.gpu_uuid -cne [string]$Release.gpu_uuid) {
        throw 'this launch has not attested its bound GPU UUID'
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
            Assert-RuntimeGpuReady $release
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
            Assert-RuntimeGpuReady $release
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

# Whether a controller action is mutating this lifecycle right now. The managed wrapper reads
# this to decide whether the GPU-owner lease is its to restore.
function Test-ManagedActionInProgress {
    $path = Join-Path $StateRoot 'action.lock'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    try {
        $probe = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite,
                                 [IO.FileShare]::None)
        $probe.Dispose()
        return $false
    }
    catch [IO.IOException] { return $true }
}

# The termination fallback, in order and best-effort: stop the task, then force whatever is
# still the owned process. Neither failure may skip the one after it - a scheduler API error
# that aborted the stop would leave a serving process alive and no record of why.
function Stop-LaunchNow([object]$Release, [string]$TaskName, [object]$Receipt, [object]$ExpectedRuntime) {
    $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if ($null -ne $task -and [string]$task.State -ceq 'Running') {
        try { Stop-ScheduledTask -TaskName $TaskName }
        catch { $Receipt.reason = "scheduled-task stop failed: $($_.Exception.Message)" }
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($null -eq (Get-OwnedProcess (Read-StopRuntime $ExpectedRuntime) $Release)) { return }
        Start-Sleep -Milliseconds 250
    }
    $live = Get-OwnedProcess (Read-StopRuntime $ExpectedRuntime) $Release
    if ($null -ne $live) {
        try {
            Stop-Process -Id $live.Id -Force
            $live.WaitForExit(10000) | Out-Null
            $Receipt.forced = $true
        }
        catch { $Receipt.reason = "force termination failed: $($_.Exception.Message)" }
    }
}

function Resolve-RuntimeRelease([object]$Runtime, [object]$State) {
    $id = [string]$Runtime.release_id
    $root = Get-LaunchRoot $id
    $property = if ($null -eq $State) { $null } else { $State.releases.PSObject.Properties[$id] }
    if ($null -ne $property) {
        $release = Get-Release $State $id
        if (-not (Test-SamePath ([string]$release.release_root) $root)) {
            throw 'runtime release root is not canonical'
        }
        return $release
    }
    return [pscustomobject]@{
        release_root=$root;server_executable=(Join-Path (Join-Path $root 'bin') 'ninfer-serve.exe')
        managed_stop=[string](Get-AttributionField $Runtime 'managed_stop');graceful_stop_timeout_seconds=30
    }
}

function New-StopReceipt([object]$Runtime, [string]$Id, [object]$Plan) {
    return [pscustomobject][ordered]@{
        artifact_type='ninfer_windows_managed_stop_receipt';schema_version=1
        launch_id=if ($null -eq $Runtime) { $null } else { Get-LaunchKey $Runtime }
        release_id=$Id;mode=[string]$Plan.mode;graceful_wait_seconds=[int]$Plan.timeout_seconds
        outcome='already_stopped';forced=$false;exit_code=$null;wrapper_result=$null
        sessions_saved=$null;sessions_skipped=$null;sessions_refused=$null;reason=$null
        pid=if ($null -eq $Runtime) { $null } else { $Runtime.pid }
        started_utc=[DateTime]::UtcNow.ToString('o');completed_utc=$null;elapsed_seconds=0.0
    }
}

function Test-NonCleanStopReceipt([object]$Receipt) {
    return $null -ne $Receipt -and [string]$Receipt.mode -ceq 'stop-event' -and
        ([bool]$Receipt.forced -or [string]$Receipt.outcome -cnotin @('graceful', 'already_stopped'))
}

function Set-StopReportVerdict([object]$Runtime, [object]$Release, [object]$Receipt) {
    if ([string]$Receipt.mode -cne 'stop-event') { return }
    $report = $null
    $expected = Join-Path (Join-Path ([string]$Release.release_root) 'logs') 'shutdown.json'
    $path = [string](Get-AttributionField $Runtime 'shutdown_report')
    if (-not [string]::IsNullOrWhiteSpace($path) -and (Test-SamePath $path $expected)) {
        try { $report = Read-JsonFile $path } catch { $report = $null }
    }
    $valid = $null -ne $report -and
        [string](Get-AttributionField $report 'artifact_type') -ceq 'ninfer_serve_shutdown_report' -and
        (Get-AttributionField $report 'schema_version') -ceq 1 -and
        [string](Get-AttributionField $report 'launch_id') -ceq [string](Get-LaunchKey $Runtime) -and
        (Get-AttributionField $report 'complete') -is [bool]
    foreach ($field in @('saved', 'skipped', 'refused')) {
        $value = Get-AttributionField $report $field
        if (-not ($value -is [int] -or $value -is [long]) -or $value -lt 0 -or $value -gt [int]::MaxValue) {
            $valid = $false
        }
    }
    if ($valid) {
        $Receipt.sessions_saved = [int]$report.saved
        $Receipt.sessions_skipped = [int]$report.skipped
        $Receipt.sessions_refused = [int]$report.refused
    }
    # An observed force/signal/nonzero failure remains causal even if the report looks complete.
    if (Test-NonCleanStopReceipt $Receipt) { return }
    if (-not $valid) {
        $Receipt.outcome = 'graceful_unreported'
        $Receipt.reason = 'no valid complete shutdown report bound to this launch and canonical path'
    }
    elseif (-not $report.complete -or $report.refused -ne 0) {
        $Receipt.outcome = 'graceful_incomplete_shutdown'
        $Receipt.reason = 'incomplete final save; retained checkpoints do not recover refused sessions; inspect and Reconcile'
    }
}

function Complete-LaunchRetirement([object]$Runtime, [object]$Release, [object]$Receipt) {
    Set-StopReportVerdict $Runtime $Release $Receipt
    $Receipt.completed_utc = [DateTime]::UtcNow.ToString('o')
    $Receipt.elapsed_seconds = [Math]::Round(([DateTime]::UtcNow - [DateTime]$Receipt.started_utc).TotalSeconds, 3)
    return Invoke-WithHoldsLock {
        $latest = Get-RuntimeStrict
        Assert-SameLaunch $Runtime $latest
        $holds = Get-ShutdownHolds
        $prior = Get-StickyStopReceipt
        $key = [string](Get-LaunchKey $Runtime)
        $existing = @($holds.holds | Where-Object { [string]$_.launch_id -ceq $key })
        $chosen = $Receipt
        if ($existing.Count -ne 0) {
            if ([string]$existing[0].release_id -cne [string]$Runtime.release_id) { throw 'conflicting held release' }
            $chosen = $existing[0].stop_receipt
        }
        elseif ($null -ne $prior -and -not [string]::IsNullOrEmpty($key) -and
                [string](Get-AttributionField $prior 'launch_id') -ceq $key -and
                (Test-NonCleanStopReceipt $prior)) {
            if ([string]$prior.release_id -cne [string]$Runtime.release_id) { throw 'conflicting sticky release' }
            $chosen = $prior
        }
        Write-JsonAtomic (Join-Path $StateRoot 'last-stop.json') $chosen
        $held = [string]$Receipt.mode -ceq 'stop-event' -and
            ((Test-NonCleanStopReceipt $Receipt) -or (Test-NonCleanStopReceipt $chosen) -or $existing.Count -ne 0)
        if ($held -and $existing.Count -eq 0) {
            $entry = [pscustomobject]@{
                launch_id=$key;release_id=[string]$latest.release_id
                release_root=[string]$Release.release_root;runtime=$latest;stop_receipt=$chosen
            }
            $holds.holds = @($holds.holds) + @($entry)
            Write-JsonAtomic (Join-Path $StateRoot 'shutdown-holds.json') $holds
        }
        Remove-RuntimeForLaunch $latest
        return [pscustomobject]@{
            stopped=$true;launch_id=if ([string]::IsNullOrEmpty($key)) { $null } else { $key }
            release_id=[string]$Runtime.release_id;release_root=[string]$Release.release_root
            held_launch_ids=@(if ($held) { $key });receipt=$chosen
        }
    }
}

function Read-StopRuntime([object]$Expected) {
    $current = Invoke-WithHoldsLock { Get-RuntimeStrict }
    Assert-SameLaunch $Expected $current
    return $current
}

# Caller retains run.lock: no wrapper can replace the slot during this proof and commit.
function Invoke-LaunchAdmission([object]$State) {
    $snapshot = Invoke-WithHoldsLock { Get-ShutdownHolds | Out-Null; Get-RuntimeStrict }
    if ($null -eq $snapshot) { return }
    $release = Resolve-RuntimeRelease $snapshot $State
    if ($null -ne (Get-OwnedProcess $snapshot $release)) { throw 'a prior attributable launch still runs' }
    Assert-NoReleaseProcess $release
    $plan = Get-ManagedStopPlan $release
    $receipt = New-StopReceipt $snapshot ([string]$snapshot.release_id) $plan
    $result = Complete-LaunchRetirement $snapshot $release $receipt
    if ($result.held_launch_ids.Count -ne 0) { Write-Warning 'prior final-save evidence remains held until explicit Reconcile' }
}

function Publish-Runtime([object]$Record, [bool]$NewLaunch = $false) {
    Invoke-WithHoldsLock {
        $current = Get-RuntimeStrict
        $holds = Get-ShutdownHolds
        if ($NewLaunch) {
            if ($null -ne $current) { throw 'runtime admission requires an empty slot' }
            if (@($holds.holds | Where-Object { $_.launch_id -ceq [string]$Record.launch_id }).Count -ne 0) {
                throw 'new launch key is already held'
            }
        }
        else { Assert-SameLaunch ([pscustomobject]$Record) $current }
        Write-JsonAtomic (Join-Path $StateRoot 'runtime.json') $Record
    }
}

function Stop-ManagedProcess {
    $state = Get-State
    $runtime = Invoke-WithHoldsLock { Get-ShutdownHolds | Out-Null; Get-RuntimeStrict }
    $release = if ($null -eq $runtime) { Get-Release $state ([string]$state.active_release) }
               else { Resolve-RuntimeRelease $runtime $state }
    $id = if ($null -eq $runtime) { [string]$state.active_release } else { [string]$runtime.release_id }
    $plan = Get-ManagedStopPlan $release
    $receipt = New-StopReceipt $runtime $id $plan
    $fence = $null
    $persistenceFailure = $null
    try {
        $owned = Get-OwnedProcess $runtime $release
        if ($null -ne $owned -and $id -cne [string]$state.active_release) {
            throw 'foreign live runtime refuses Stop; active release is not authority to terminate it'
        }
        $eventName = Get-RuntimeStopEvent $runtime $id
        if ($null -ne $owned -and $plan.mode -ceq 'stop-event' -and $null -ne $eventName) {
            $deadline = [DateTime]::UtcNow.AddSeconds([int]$plan.timeout_seconds)
            $delivery = 'absent'
            while ($delivery -ceq 'absent' -and -not $owned.HasExited -and [DateTime]::UtcNow -lt $deadline) {
                $delivery = Request-ManagedStop $eventName
                if ($delivery -ceq 'absent') { Start-Sleep -Milliseconds 250 }
            }
            if ($delivery -ceq 'signalled') {
                $receipt.outcome = 'graceful'
                $remaining = [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalMilliseconds)
                $owned.WaitForExit([int][Math]::Max(0, $remaining)) | Out-Null
                if (-not $owned.HasExited) { $receipt.outcome = 'graceful_wait_expired' }
                else {
                    try { $exitCode = $owned.ExitCode } catch { $exitCode = $null }
                    if ($null -ne $exitCode) {
                        $receipt.exit_code = [int]$exitCode
                        if ([int]$exitCode -ne 0) { $receipt.outcome = 'graceful_nonzero_exit' }
                    }
                }
            }
            else { $receipt.outcome = 'signal_failed'; $receipt.reason = "stop event delivery: $delivery" }
        }
        elseif ($null -ne $owned) { $receipt.outcome = 'terminated' }

        if ($null -ne (Get-OwnedProcess (Read-StopRuntime $runtime) $release)) {
            if ($receipt.outcome -ceq 'already_stopped') { $receipt.outcome = 'terminated' }
            $receipt.forced = $true
            try { Write-ManagedStopReceipt $receipt | Out-Null }
            catch { $persistenceFailure = $_ }
            # A receipt failure never suppresses best-effort termination, but does withhold handback.
            Stop-LaunchNow $release ([string]$state.task_name) $receipt $runtime
        }
        if (-not (Wait-ManagedWrapperExit ([string]$state.task_name) ([DateTime]::UtcNow.AddSeconds(120)))) {
            $receipt.outcome = 'wrapper_cleanup_stalled'
            $receipt.reason = 'the managed wrapper did not release the run lock after its server stopped'
            throw $receipt.reason
        }
        $fence = Enter-LaunchFence 'run'
        $fresh = Read-StopRuntime $runtime
        if ($null -ne (Get-OwnedProcess $fresh $release)) {
            $receipt.outcome = 'still_running'
            throw 'owned server process did not stop'
        }
        Assert-NoReleaseProcess $release
        $info = Get-ScheduledTaskInfo -TaskName ([string]$state.task_name) -ErrorAction SilentlyContinue
        $receipt.wrapper_result = if ($null -eq $info) { $null } else { [int]$info.LastTaskResult }
        if ($null -ne $persistenceFailure) { throw $persistenceFailure }
        if ($null -eq $runtime) {
            $receipt = Write-ManagedStopReceipt $receipt
            return [pscustomobject]@{stopped=$true;launch_id=$null;release_id=$id
                release_root=[string]$release.release_root;held_launch_ids=@();receipt=$receipt}
        }
        return Complete-LaunchRetirement $runtime $release $receipt
    }
    catch {
        $failure = $_
        # Unexpected process/wait errors are not permission to leave a still-attributed child
        # serving. Never apply this best effort to a foreign or replacement launch.
        if ($null -ne $runtime -and $id -ceq [string]$state.active_release) {
            try {
                if ($null -ne (Get-OwnedProcess (Read-StopRuntime $runtime) $release)) {
                    $receipt.forced = $true
                    try { Write-ManagedStopReceipt $receipt | Out-Null } catch { }
                    Stop-LaunchNow $release ([string]$state.task_name) $receipt $runtime
                }
            }
            catch { $receipt.reason = "stop failure: $($failure.Exception.Message); termination: $($_.Exception.Message)" }
        }
        if ($receipt.outcome -cin @('already_stopped', 'graceful')) {
            $receipt.outcome = 'stop_unproven'; $receipt.reason = $failure.Exception.Message
        }
        $receipt.completed_utc = [DateTime]::UtcNow.ToString('o')
        try { Write-ManagedStopReceipt $receipt | Out-Null }
        catch {
            throw [InvalidOperationException]::new("stop failed and receipt publication failed: $($_.Exception.Message)", $failure.Exception)
        }
        throw $failure
    }
    finally { if ($null -ne $fence) { $fence.Dispose() } }
}

function Stop-ManagedRelease([bool]$RestoreOwner = $true) {
    $result = Stop-ManagedProcess
    if (-not $result.stopped) { throw 'managed stop did not prove quiescence' }
    if ($RestoreOwner) { Restore-GpuOwnerLease }
    if ($result.held_launch_ids.Count -ne 0) { Write-Warning 'stopped launch has incomplete final-save evidence; inspect and Reconcile' }
    return $result
}

function Start-ManagedRelease {
    $state = Get-State
    Assert-NoPreparedRelease $state
    $release = Get-Release $state ([string]$state.active_release)
    # Refuse an invalid binding or release before ownership can change the card's power cap.
    Assert-CandidateLayout $release ([string]$state.active_release)
    Assert-FileHash (Join-Path (Join-Path ([string]$release.release_root) 'bin') 'ninfer.exe') ([string]$release.ninfer_sha256) 'CLI executable'
    Assert-FileHash ([string]$release.server_executable) ([string]$release.binary_sha256) 'server executable'
    Assert-FileHash (Join-Path (Join-Path ([string]$release.release_root) 'bin') 'ninfer_bench.exe') ([string]$release.benchmark_sha256) 'benchmark executable'
    Assert-AppLocalDlls $release
    Assert-InstalledModelIdentity $release
    Assert-FileHash ([string]$release.config_file) ([string]$release.config_sha256) 'server config'
    Assert-SelectedGpuIdentity $release | Out-Null
    $owned = Get-OwnedProcess (Get-RuntimeState) $release
    if ($null -ne $owned) {
        Assert-ServerIdentity (Invoke-ServerStatus $release) $release
        Assert-RuntimeGpuReady $release
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
    $admissionFence = Enter-LaunchFence 'run'
    try {
        Invoke-LaunchAdmission $state
        Assert-NoReleaseProcess $release
    }
    finally { $admissionFence.Dispose() }

    Acquire-GpuOwnerLease
    try {
        Start-ScheduledTask -TaskName ([string]$state.task_name)
        Wait-Ready 600 | Out-Null
    }
    catch {
        $startFailure = $_
        $cleanupFailures = [Collections.Generic.List[string]]::new()
        try {
            $stopped = Stop-ManagedProcess
            if (-not $stopped.stopped) { throw 'candidate stop proof is missing' }
            Restore-GpuOwnerLease
        }
        catch { $cleanupFailures.Add("candidate stop/owner restore: $($_.Exception.Message)") }
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
    $process = $null
    $stdoutStream = $null
    $stderrStream = $null
    $stdoutCopy = $null
    $stderrCopy = $null
    $ownerLeaseHeld = $false
    try {
        $lockPath = Join-Path $StateRoot 'run.lock'
        $lock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite,
                               [IO.FileShare]::None)
        $state = Get-State
        Assert-NoPreparedRelease $state
        $release = Get-Release $state ([string]$state.active_release)
        Invoke-LaunchAdmission $state
        if ($null -ne (Get-NetTCPConnection -State Listen -LocalPort ([int]$release.port) -ErrorAction SilentlyContinue)) {
            throw 'release listen port is already owned by another process'
        }
        Assert-CandidateLayout $release ([string]$state.active_release)
        Assert-FileHash (Join-Path (Join-Path ([string]$release.release_root) 'bin') 'ninfer.exe') ([string]$release.ninfer_sha256) 'CLI executable'
        Assert-FileHash ([string]$release.server_executable) ([string]$release.binary_sha256) 'server executable'
        Assert-FileHash (Join-Path (Join-Path ([string]$release.release_root) 'bin') 'ninfer_bench.exe') ([string]$release.benchmark_sha256) 'benchmark executable'
        Assert-AppLocalDlls $release
        Assert-InstalledModelIdentity $release
        $resolvedGpuIndex = Assert-SelectedGpuIdentity $release
        Assert-FileHash ([string]$release.config_file) ([string]$release.config_sha256) 'server config'
        $config = Read-JsonFile ([string]$release.config_file)
        $cache = [string]$release.cache_root
        if (-not [bool]$config.session_checkpoint.enabled) {
            throw 'managed release requires durable session checkpoints'
        }
        $checkpointDirectory = if ([int]$config.schema_version -eq 4) { 'session-checkpoints-v3' }
                               else { 'session-checkpoints' }
        $checkpointRoot = Join-Path $cache $checkpointDirectory
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
                '--device', [string]$resolvedGpuIndex,
                '--max-context', [string]$config.engine.max_context,
                '--kv-capacity', [string]$config.engine.kv_capacity,
                '--prefill-chunk', [string]$config.engine.prefill_chunk,
                '--kv-dtype', [string]$config.engine.kv_dtype,
                '--max-concurrency', [string]$config.engine.max_concurrency,
                '--max-pending-requests', [string]$config.engine.max_pending_requests,
                '--pending-timeout-ms', [string]$config.engine.pending_timeout_ms,
                '--device-state-slots', [string]$config.context_cache.device_state_slots,
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
        Add-ContextArguments $serverArguments $config
        $fp32HeadLogits = Get-Fp32HeadLogits $config
        if ($fp32HeadLogits) {
            $serverArguments.Add('--fp32-head-logits')
        }
        if (-not [bool]$config.engine.cuda_graph) { $serverArguments.Add('--no-cuda-graph') }
        if (-not [bool]$config.engine.prefix_reuse) { $serverArguments.Add('--no-prefix-reuse') }
        if (-not [bool]$config.reasoning.thinking) { $serverArguments.Add('--no-thinking') }
        if ([bool]$config.reasoning.preserve_thinking) { $serverArguments.Add('--preserve-thinking') }
        if ([bool]$config.engine.vision) { $serverArguments.Add('--vision') }
        # The runtime gained --gpu-keep-warm-ms after the shipped lineage, and an older server refuses
        # it as an unknown argument: only a release whose own packaged config declares a positive
        # value passes it, so a rollback to an earlier release never receives it.
        $gpuKeepWarmMs = Get-GpuKeepWarmMs $config
        if ($gpuKeepWarmMs -gt 0) {
            foreach ($argument in @('--gpu-keep-warm-ms', [string]$gpuKeepWarmMs)) {
                $serverArguments.Add($argument)
            }
        }

        $speculativeBackend = [string]$config.speculative.backend
        $draftTokens = [int]$config.speculative.draft_tokens
        if ($speculativeBackend -ceq 'none') {
            if ($draftTokens -ne 0) { throw 'MTP0 configuration must use zero draft tokens' }
        }
        elseif ($speculativeBackend -ceq 'mtp') {
            if ($draftTokens -lt 1 -or $draftTokens -gt 5) {
                throw 'MTP draft tokens must be in [1,5]'
            }
            foreach ($argument in @('--spec', $speculativeBackend, '--draft-tokens', [string]$draftTokens,
                                    '--lm-head-draft')) {
                $serverArguments.Add($argument)
            }
        }
        elseif ($speculativeBackend -ceq 'dflash2') {
            if ($draftTokens -lt 1 -or $draftTokens -gt 15) {
                throw 'DFlash2 draft tokens must be in [1,15]'
            }
            foreach ($argument in @('--spec', $speculativeBackend, '--draft-tokens', [string]$draftTokens,
                                    '--lm-head-draft')) {
                $serverArguments.Add($argument)
            }
        }
        else {
            throw "unsupported release speculative backend: $speculativeBackend"
        }

        # Both flags belong to the graceful-stop channel and only a release that declares it
        # knows them: a release installed before the channel existed refuses either as an unknown
        # argument and never starts. One kernel-object name per launch, never reused - the server
        # refuses a name that already exists - and one report path, cleared first so a stale
        # report can never be read as this launch's outcome.
        $stopPlan = Get-ManagedStopPlan $release
        $stopEventName = $null
        $shutdownReport = $null
        $launchId = [Guid]::NewGuid().ToString('N')
        if ([string]$stopPlan.mode -ceq 'stop-event') {
            # One identity per launch, carried by the event name and echoed in the report the
            # server writes, so the report the controller reads is provably this launch's.
            $stopEventName = 'Global\NInfer-Serve-Stop-' + $launchId
            $shutdownReport = Join-Path $logs 'shutdown.json'
            Remove-Item -LiteralPath $shutdownReport -Force -ErrorAction SilentlyContinue
            foreach ($argument in @('--stop-event', $stopEventName,
                                    '--shutdown-report', $shutdownReport)) {
                $serverArguments.Add($argument)
            }
        }

        $argumentLine = [string]::Join(' ', @($serverArguments | ForEach-Object {
                    Quote-NativeArgument $_
                }))
        $stdout = Join-Path $logs 'stdout.log'
        $stderr = Join-Path $logs 'stderr.log'
        $requestLog = Join-Path $logs 'requests.jsonl'
        # Redirection truncates, so the log that explains a stop would be destroyed by the next
        # launch - which is exactly the launch an operator compares it against. Keep one.
        foreach ($log in @($stdout, $stderr)) {
            if (Test-Path -LiteralPath $log -PathType Leaf) {
                Move-Item -LiteralPath $log -Destination "$log.previous" -Force `
                    -ErrorAction Stop
            }
        }
        Assert-FileHash ([string]$release.server_executable) ([string]$release.binary_sha256) 'server executable before launch'
        Assert-FileHash ([string]$release.config_file) ([string]$release.config_sha256) 'server config before launch'
        Assert-NoReleaseProcess $release
        Acquire-GpuOwnerLease
        $ownerLeaseHeld = $true
        $startInfo = [Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = [string]$release.server_executable
        $startInfo.Arguments = $argumentLine
        $startInfo.WorkingDirectory = [string]$release.release_root
        $startInfo.UseShellExecute = $false
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.EnvironmentVariables['CUDA_DEVICE_ORDER'] = 'PCI_BUS_ID'
        $stdoutStream = [IO.File]::Open($stdout, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
        $stderrStream = [IO.File]::Open($stderr, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
        $requestLogOffset = 0L
        $requestLogCreationTicks = 0L
        if (Test-Path -LiteralPath $requestLog -PathType Leaf) {
            $logItem = Get-Item -LiteralPath $requestLog
            $requestLogOffset = [Int64]$logItem.Length
            $requestLogCreationTicks = [Int64]$logItem.CreationTimeUtc.Ticks
        }
        # Publish launch attribution before spawning. A null PID means the wrapper may have died
        # during spawn, never that the release is stopped; the image census closes that interval.
        $runtimeRecord = [ordered]@{
                artifact_type = 'ninfer_windows_runtime_state'
                schema_version = 2
                release_id = [string]$state.active_release
                pid = $null
                start_time_utc_ticks = $null
                managed_stop = [string]$stopPlan.mode
                stop_event = $stopEventName
                shutdown_report = $shutdownReport
                launch_id = $launchId
                gpu_uuid = [string]$release.gpu_uuid
                gpu_device = $resolvedGpuIndex
                gpu_name = [string]$release.gpu_name
                gpu_uuid_verified = $false
                request_log_start_offset = $requestLogOffset
                request_log_creation_utc_ticks = $requestLogCreationTicks
            }
        Publish-Runtime $runtimeRecord $true
        try { $process = [Diagnostics.Process]::Start($startInfo) }
        catch {
            # Process.Start returned no child; only this still-owning wrapper can prove this.
            Invoke-WithHoldsLock { Remove-RuntimeForLaunch ([pscustomobject]$runtimeRecord) }
            throw
        }
        $runtimeRecord.pid = $process.Id
        $runtimeRecord.start_time_utc_ticks = $process.StartTime.ToUniversalTime().Ticks
        Publish-Runtime $runtimeRecord
        $stdoutCopy = $process.StandardOutput.BaseStream.CopyToAsync($stdoutStream)
        $stderrCopy = $process.StandardError.BaseStream.CopyToAsync($stderrStream)
        $gpuDeadline = [DateTime]::UtcNow.AddSeconds(600)
        $requestLogSchema = if ([int]$config.schema_version -eq 4) { 30 } else { 0 }
        while (-not (Test-ServerStartGpu $requestLog ([string]$release.gpu_uuid) $requestLogOffset $requestLogCreationTicks $process.Id $resolvedGpuIndex ([string]$release.gpu_name) $requestLogSchema)) {
            if ($process.HasExited) { throw 'server exited before GPU UUID attestation' }
            if ([DateTime]::UtcNow -ge $gpuDeadline) { throw 'server_start GPU UUID attestation timed out after 600 seconds' }
            Start-Sleep -Milliseconds 200
        }
        $runtimeRecord.gpu_uuid_verified = $true
        Publish-Runtime $runtimeRecord
        $process.WaitForExit()
        # Windows hands a parent that redirected the child's streams a handle whose ExitCode
        # reads as absent - clean exit or not - so an unreadable code carries no information and
        # must not be read as failure. The server's own shutdown report is the outcome channel.
        $serverExit = $null
        try { $serverExit = $process.ExitCode } catch { $serverExit = $null }
        if ($null -ne $serverExit -and [int]$serverExit -ne 0) {
            # The scheduled task retries a failed wrapper. That is right for a server that died
            # on its own; it is the opposite of right for one the controller asked to stop and
            # that reported an unclean shutdown on the way out - a retry would relaunch what an
            # operator just stopped. The report says which; a launch that ended by request exits
            # clean here and leaves the verdict to the stop receipt.
            $stoppedByRequest = $false
            if ($null -ne $shutdownReport -and (Test-Path -LiteralPath $shutdownReport -PathType Leaf)) {
                try {
                    $report = Read-JsonFile $shutdownReport
                    $stoppedByRequest = [string]$report.launch_id -ceq $launchId
                }
                catch { $stoppedByRequest = $false }
            }
            if (-not $stoppedByRequest) {
                throw "ninfer-serve exited with code $([int]$serverExit)"
            }
        }
    }
    finally {
        # Identity mismatch, missing attestation, and any launch failure stop the exact child.
        try {
            try {
                if ($null -ne $process -and -not $process.HasExited) {
                    $process.Kill()
                    $process.WaitForExit()
                }
                if ($null -ne $stdoutCopy) { $stdoutCopy.GetAwaiter().GetResult() }
                if ($null -ne $stderrCopy) { $stderrCopy.GetAwaiter().GetResult() }
            }
            finally {
                if ($null -ne $stdoutStream) { $stdoutStream.Dispose() }
                if ($null -ne $stderrStream) { $stderrStream.Dispose() }
            }
        }
        finally {
            # Stop owns retirement of runtime.json, including the launch-bound shutdown verdict.
            # Failed Kill must not discard attribution or return the GPU owner to its prior cap.
            if ($null -ne $lock) { $lock.Dispose() }
            # A controller action owns restoration while holding action.lock. Otherwise the
            # wrapper restores its own lease only after proving its child exited.
            if ($ownerLeaseHeld -and ($null -eq $process -or $process.HasExited) -and
                -not (Test-ManagedActionInProgress)) { Restore-GpuOwnerLease }
        }
    }
}

function Test-LiveManagedWrapper([string]$TaskName) {
    $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if ($null -eq $task -or [string]$task.State -cne 'Running') { return $false }
    $probe = $null
    try {
        $probe = [IO.File]::Open((Join-Path $StateRoot 'run.lock'),
            [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        return $false
    }
    catch [IO.IOException] { return $true }
    finally { if ($null -ne $probe) { $probe.Dispose() } }
}

function Invoke-Reconcile([string]$Id) {
    $root = Get-LaunchRoot $Id
    $selection = Invoke-WithHoldsLock {
        $envelope = Get-ShutdownHolds
        [pscustomobject]@{
            entries=@($envelope.holds | Where-Object { [string]$_.release_id -ceq $Id })
            runtime=Get-RuntimeStrict
            all_keys=@($envelope.holds | ForEach-Object { [string]$_.launch_id })
        }
    }
    if ($selection.entries.Count -eq 0) { throw 'no held launches exist for the selected release' }
    $ids = @($selection.entries | ForEach-Object { [string]$_.launch_id })
    $state = if (Test-Path -LiteralPath (Join-Path $StateRoot 'state.json') -PathType Leaf) { Get-State } else { $null }
    $runtime = $selection.runtime
    $key = if ($null -eq $runtime) { $null } else { Get-LaunchKey $runtime }
    $selectedRuntime = $null -ne $key -and $key -cin $ids
    $fence = $null
    $census = 'idle'
    try {
        $release = [pscustomobject]@{release_root=$root;server_executable=(Join-Path (Join-Path $root 'bin') 'ninfer-serve.exe')}
        if ($selectedRuntime) {
            if ($null -ne (Get-OwnedProcess $runtime $release)) { throw 'selected held launch still runs' }
            $taskName = if ($null -eq $state) { '' } else { [string]$state.task_name }
            if (-not (Wait-ManagedWrapperExit $taskName ([DateTime]::UtcNow.AddSeconds(60)))) {
                throw 'held launch wrapper has not drained'
            }
            $fence = Enter-LaunchFence 'run'
            Assert-NoReleaseProcess $release
        }
        elseif ($null -ne $state -and [string]$state.active_release -ceq $Id -and
                $null -ne $runtime -and [string]$runtime.release_id -ceq $Id -and
                $null -ne $key -and $key -cnotin $selection.all_keys -and
                $null -ne (Get-OwnedProcess $runtime (Resolve-RuntimeRelease $runtime $state)) -and
                (Test-LiveManagedWrapper ([string]$state.task_name))) {
            $census = 'active-new-launch'
        }
        else { Assert-NoReleaseProcess $release }
        $receipts = Join-Path $StateRoot 'receipts'
        New-Item -ItemType Directory -Force -Path $receipts | Out-Null
        return Invoke-WithHoldsLock {
            $current = Get-RuntimeStrict
            Assert-SameLaunch $runtime $current
            $envelope = Get-ShutdownHolds
            $cleared = @($envelope.holds | Where-Object { [string]$_.launch_id -cin $ids })
            if ($cleared.Count -eq 0) { throw 'selected holds were already acknowledged' }
            $receipt = [pscustomobject][ordered]@{
                artifact_type='ninfer_windows_shutdown_reconcile_receipt';schema_version=1
                receipt_id=[Guid]::NewGuid().ToString('N');release_id=$Id;utc=[DateTime]::UtcNow.ToString('o')
                lifecycle_state_present=($null -ne $state);census_result=$census
                cleared_holds=$cleared;remaining_hold_count=(@($envelope.holds).Count - $cleared.Count)
            }
            Write-JsonAtomic (Join-Path $receipts ('shutdown-reconcile-' + $receipt.receipt_id + '.json')) $receipt
            if ($selectedRuntime) { Remove-RuntimeForLaunch $current }
            $envelope.holds = @($envelope.holds | Where-Object { [string]$_.launch_id -cnotin $ids })
            Write-JsonAtomic (Join-Path $StateRoot 'shutdown-holds.json') $envelope
            return $receipt
        }
    }
    finally { if ($null -ne $fence) { $fence.Dispose() } }
}

function Assert-NoShutdownHolds {
    Invoke-WithHoldsLock {
        if (@((Get-ShutdownHolds).holds).Count -ne 0) {
            throw 'shutdown evidence is held; inspect and Reconcile every release before Uninstall'
        }
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

        # Stop and task removal are one step under the action lock: a Start between them would
        # launch a server that outlives its release identity. Scoped, not wrapped around the
        # whole function - the lock file lives in the tree this function goes on to delete, so
        # its handle must be closed before that delete, the same reason install.lock is released
        # in the finally below. With the task gone nothing can launch afterwards.
        Invoke-WithActionLock {
            Assert-NoShutdownHolds
            Stop-ManagedRelease | Out-Null
            $removalFence = Enter-LaunchFence 'run'
            try {
            Assert-NoShutdownHolds
            Invoke-WithHoldsLock {
                if ($null -ne (Get-RuntimeStrict)) { throw 'runtime appeared before task removal' }
            }
            # Whole-root removal also retires a pause that predates the release lease. Restore
            # its recorded prior limit now, or leave its record and the state root for retry.
            Invoke-GpuOwner $state 'start' | Out-Null
            Unregister-ScheduledTask -TaskName ([string]$state.task_name) -Confirm:$false -ErrorAction SilentlyContinue
            if ($null -ne (Get-OwnedProcess (Get-RuntimeState) (Get-Release $state ([string]$state.active_release)))) {
                throw 'a server process survived the uninstall stop'
            }
            }
            finally { $removalFence.Dispose() }
        }
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
        $result = Invoke-WithActionLock { Stop-ManagedRelease }
        Get-StatusObject | ConvertTo-Json -Depth 20
        if ($result.held_launch_ids.Count -ne 0 -and -not $AllowHeld) {
            throw "launch $($result.launch_id) of release $($result.release_id) stopped with held final-save evidence; inspect and Reconcile"
        }
    }
    'Reconcile' {
        Invoke-Reconcile $ReleaseId | ConvertTo-Json -Depth 24
    }
    'Restart' {
        Invoke-WithActionLock {
            Assert-NoPreparedRelease (Get-State)
            Stop-ManagedRelease $false | Out-Null
            Start-ManagedRelease
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
            Stop-ManagedRelease | Out-Null
            $state.active_release = $previous
            $state.previous_release = $current
            Write-JsonAtomic (Join-Path $StateRoot 'state.json') $state
            try {
                Start-ManagedRelease
            }
            catch {
                $targetFailure = $_
                try {
                    Stop-ManagedRelease | Out-Null
                    $state.active_release = $current
                    $state.previous_release = $previous
                    Write-JsonAtomic (Join-Path $StateRoot 'state.json') $state
                    Start-ManagedRelease
                }
                catch {
                    throw [InvalidOperationException]::new(
                        "rollback target start failed: $($targetFailure.Exception.Message); recovery failed: $($_.Exception.Message)",
                        $targetFailure.Exception)
                }
                throw $targetFailure
            }
        }
        Get-StatusObject | ConvertTo-Json -Depth 20
    }
    'Uninstall' {
        Uninstall-ManagedRelease
    }
}
