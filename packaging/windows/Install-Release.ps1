[CmdletBinding(DefaultParameterSetName = 'Install')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Install')]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$PackagePath,

    [Parameter(Mandatory = $true, ParameterSetName = 'Install')]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$PackageSha256,

    [Parameter(Mandatory = $true, ParameterSetName = 'Install')]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ModelArtifactPath,

    [Parameter(Mandatory = $true, ParameterSetName = 'Install')]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ApiKeyFile,

    [Parameter(ParameterSetName = 'Install')]
    [string]$GpuOwnerControllerPath,

    [Parameter(ParameterSetName = 'Install')]
    [ValidatePattern('^GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$')]
    [string]$GpuUuid,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$StateRoot,

    [Parameter(ParameterSetName = 'Install')]
    [switch]$NoStart,

    [Parameter(ParameterSetName = 'Install')]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9-]{0,31}$')]
    [string]$ScratchTaskSuffix,

    [Parameter(Mandatory = $true, ParameterSetName = 'Repair')]
    [switch]$RepairInterruptedInstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Protect-StateRoot.ps1')

$principal = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent()
)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Install-Release.ps1 must run from an elevated PowerShell session'
}

function Read-JsonFile([string]$Path) {
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

function Resolve-LifecycleTaskName([object]$Spec, [string]$Root, [string]$Suffix) {
    $name = [string]$Spec.lifecycle.task_name
    if ($name -cnotmatch '^NInfer-Qwen38-[0-9]{4}-Native$') { throw 'unexpected lifecycle task identity' }
    if ([string]::IsNullOrWhiteSpace($Suffix)) { return $name }
    if ($Suffix -cnotmatch '^[A-Za-z0-9][A-Za-z0-9-]{0,31}$') { throw 'invalid scratch task suffix' }
    $productRoot = Join-Path (Join-Path $env:ProgramData 'NInfer') ([string]$Spec.lifecycle.state_root_name)
    if ([string]::Equals([IO.Path]::GetFullPath($Root), [IO.Path]::GetFullPath($productRoot),
                        [StringComparison]::OrdinalIgnoreCase)) {
        throw 'scratch task isolation requires a non-product state root'
    }
    return $name + '-Scratch-' + $Suffix
}

function Write-JsonAtomic([string]$Path, [object]$Value) {
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText(
            $temporary,
            ($Value | ConvertTo-Json -Depth 20),
            [Text.UTF8Encoding]::new($false)
        )
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    }
    finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}


function Restore-ReleaseTask([string]$TaskName, [bool]$Existed, [string]$Xml) {
    if ($Existed) {
        Register-ScheduledTask -TaskName $TaskName -Xml $Xml -Force | Out-Null
        return
    }
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
}

function Write-InstallEvent([string]$Phase, [string]$Message, [object]$Data = $null) {
    $event = [ordered]@{
        artifact_type = 'ninfer_windows_install_progress'
        schema_version = 1
        utc = [DateTime]::UtcNow.ToString('o')
        phase = $Phase
        message = $Message
    }
    if ($null -ne $Data) { $event['data'] = $Data }
    [Console]::Out.WriteLine(($event | ConvertTo-Json -Depth 8 -Compress))
    [Console]::Out.Flush()
}

function Get-BoundedDiagnostic([object]$Value, [int]$MaximumLength = 1024) {
    $text = [string]$Value
    if ($text.Length -le $MaximumLength) { return $text }
    return $text.Substring(0, $MaximumLength) + '...[truncated]'
}

function Test-PathWithinRoot([string]$Path, [string]$Root) {
    $separators = [char[]]@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd($separators) + [IO.Path]::DirectorySeparatorChar
    return $fullPath.StartsWith($fullRoot, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-ExternalModelReference([string]$Path, [string]$LifecycleRoot) {
    if (Test-PathWithinRoot $Path $LifecycleRoot) {
        throw 'pinned model artifact must remain outside operation-owned lifecycle state'
    }
}

function Get-StreamingSha256([string]$Path, [string]$Label) {
    $itemBefore = Get-Item -LiteralPath $Path
    $total = [Int64]$itemBefore.Length
    $completed = [Int64]0
    $lastReportedBytes = [Int64]0
    $lastReportUtc = [DateTime]::UtcNow
    $buffer = New-Object byte[] (8 * 1024 * 1024)
    $hasher = [Security.Cryptography.IncrementalHash]::CreateHash(
        [Security.Cryptography.HashAlgorithmName]::SHA256
    )
    $stream = $null
    Write-InstallEvent 'model_identity_started' "Verifying $Label without copying it" ([ordered]@{
            path = $Path
            bytes_total = $total
        })
    try {
        $stream = [IO.FileStream]::new(
            $Path,
            [IO.FileMode]::Open,
            [IO.FileAccess]::Read,
            [IO.FileShare]::Read,
            $buffer.Length,
            [IO.FileOptions]::SequentialScan
        )
        while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $hasher.AppendData($buffer, 0, $read)
            $completed += $read
            $now = [DateTime]::UtcNow
            if (($completed - $lastReportedBytes) -ge 1073741824 -or
                ($now - $lastReportUtc).TotalSeconds -ge 30) {
                Write-InstallEvent 'model_identity_progress' "Verifying $Label" ([ordered]@{
                        bytes_completed = $completed
                        bytes_total = $total
                        percent = if ($total -eq 0) { 100.0 } else {
                            [Math]::Round(($completed * 100.0) / $total, 2)
                        }
                    })
                $lastReportedBytes = $completed
                $lastReportUtc = $now
            }
        }
        $digest = [BitConverter]::ToString($hasher.GetHashAndReset()).Replace('-', '').ToLowerInvariant()
    }
    finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $hasher.Dispose()
    }
    $itemAfter = Get-Item -LiteralPath $Path
    if ([Int64]$itemAfter.Length -ne [Int64]$itemBefore.Length -or
        [Int64]$itemAfter.CreationTimeUtc.Ticks -ne [Int64]$itemBefore.CreationTimeUtc.Ticks -or
        [Int64]$itemAfter.LastWriteTimeUtc.Ticks -ne [Int64]$itemBefore.LastWriteTimeUtc.Ticks) {
        throw "$Label changed while its identity was being verified"
    }
    Write-InstallEvent 'model_identity_completed' "Verified $Label without copying it" ([ordered]@{
            bytes_completed = $completed
            bytes_total = $total
            sha256 = $digest
        })
    return [ordered]@{
        sha256 = $digest
        bytes = [Int64]$itemAfter.Length
        creation_utc_ticks = [Int64]$itemAfter.CreationTimeUtc.Ticks
        last_write_utc_ticks = [Int64]$itemAfter.LastWriteTimeUtc.Ticks
    }
}

function Save-PersistedFileSnapshot(
    [string]$Path,
    [string]$SnapshotDirectory,
    [string]$Name
) {
    $backupPath = Join-Path $SnapshotDirectory "$Name.bin"
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        [IO.File]::WriteAllBytes($backupPath, [IO.File]::ReadAllBytes($Path))
        return [ordered]@{ exists = $true; backup_path = $backupPath }
    }
    return [ordered]@{ exists = $false; backup_path = $null }
}

function Restore-PersistedFileSnapshot([string]$Path, [object]$Snapshot) {
    if (-not [bool]$Snapshot.exists) {
        Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
        return
    }
    $backupPath = [string]$Snapshot.backup_path
    if (-not (Test-Path -LiteralPath $backupPath -PathType Leaf)) {
        throw "transaction snapshot is missing: $backupPath"
    }
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).restore"
    try {
        [IO.File]::WriteAllBytes($temporary, [IO.File]::ReadAllBytes($backupPath))
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    }
    finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

function Write-InstallTransaction([object]$Transaction) {
    $Transaction.updated_utc = [DateTime]::UtcNow.ToString('o')
    Write-JsonAtomic ([string]$Transaction.journal_path) $Transaction
}

function Get-PendingInstallTransactions([string]$TransactionsRoot) {
    if (-not (Test-Path -LiteralPath $TransactionsRoot -PathType Container)) { return }
    foreach ($directory in @(Get-ChildItem -LiteralPath $TransactionsRoot -Directory)) {
        $journalPath = Join-Path $directory.FullName 'journal.json'
        if (-not (Test-Path -LiteralPath $journalPath -PathType Leaf)) { continue }
        $transaction = Read-JsonFile $journalPath
        if ($transaction.artifact_type -cne 'ninfer_windows_install_transaction' -or
            [int]$transaction.schema_version -ne 1 -or
            [string]$transaction.journal_path -cne $journalPath) {
            throw "installer transaction journal envelope mismatch: $journalPath"
        }
        if ([string]$transaction.status -cin @('in_progress', 'repair_required')) {
            Write-Output $transaction
        }
    }
}

function Get-TransactionFlag([object]$Transaction, [string]$Name) {
    $property = $Transaction.flags.PSObject.Properties[$Name]
    return $null -ne $property -and [bool]$property.Value
}

function Get-TransactionCleanupTargets([object]$Transaction) {
    $descriptors = @(
        [ordered]@{ field = 'release_root'; kind = 'candidate_release'; root = (Join-Path $StateRoot 'releases') },
        [ordered]@{ field = 'secret_directory'; kind = 'candidate_secret'; root = (Join-Path $StateRoot 'secrets') },
        [ordered]@{ field = 'cache_root'; kind = 'candidate_cache'; root = (Join-Path $StateRoot 'cache') },
        [ordered]@{ field = 'stage_path'; kind = 'package_stage'; root = $StateRoot }
    )
    foreach ($descriptor in $descriptors) {
        $property = $Transaction.PSObject.Properties[[string]$descriptor.field]
        if ($null -eq $property -or [string]::IsNullOrWhiteSpace([string]$property.Value)) { continue }
        $path = [IO.Path]::GetFullPath([string]$property.Value)
        if (-not (Test-PathWithinRoot $path ([string]$descriptor.root))) {
            throw "transaction cleanup path escapes its owned root: $path"
        }
        if ([string]$descriptor.kind -ceq 'package_stage' -and
            [IO.Path]::GetFileName($path) -cnotmatch '^staging-[0-9a-f]{32}$') {
            throw "transaction stage path has an invalid identity: $path"
        }
        if (Test-Path -LiteralPath $path) {
            [pscustomobject]@{ kind = [string]$descriptor.kind; path = $path }
        }
    }
}

function Remove-InstallTreeStreaming([object]$Target) {
    $path = [string]$Target.path
    $kind = [string]$Target.kind
    Write-InstallEvent 'cleanup_target_started' "Deleting interrupted install $kind" ([ordered]@{
            kind = $kind
            path = $path
        })
    $filesDeleted = 0
    $directoriesDeleted = 0
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        throw "transaction cleanup target is no longer the directory the transaction created: $path"
    }
    if (Test-Path -LiteralPath $path -PathType Container) {
        $files = @(Get-ChildItem -LiteralPath $path -File -Recurse -Force)
        foreach ($file in $files) {
            if ($filesDeleted -lt 32 -or ($filesDeleted % 128) -eq 0) {
                Write-InstallEvent 'cleanup_file_progress' "Deleting interrupted install file" ([ordered]@{
                        kind = $kind
                        file = $file.FullName
                        files_deleted = $filesDeleted
                    })
            }
            Remove-Item -LiteralPath $file.FullName -Force
            $filesDeleted++
        }
        $directories = @(Get-ChildItem -LiteralPath $path -Directory -Recurse -Force |
            Sort-Object { $_.FullName.Length } -Descending)
        foreach ($directory in $directories) {
            Remove-Item -LiteralPath $directory.FullName -Force
            $directoriesDeleted++
        }
        Remove-Item -LiteralPath $path -Force
        $directoriesDeleted++
    }
    Write-InstallEvent 'cleanup_target_completed' "Deleted interrupted install $kind" ([ordered]@{
            kind = $kind
            path = $path
            files_deleted = $filesDeleted
            directories_deleted = $directoriesDeleted
        })
    return [ordered]@{
        kind = $kind
        path = $path
        status = 'deleted'
        files_deleted = $filesDeleted
        directories_deleted = $directoriesDeleted
    }
}

function Assert-OneLineSecret([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $length = $bytes.Length
    while ($length -gt 0 -and ($bytes[$length - 1] -eq 10 -or $bytes[$length - 1] -eq 13)) {
        $length--
    }
    if ($length -eq 0) { throw 'API-key file is empty' }
    if ($length -lt 32) { throw 'strict checkpoint origin authentication requires at least 32 API-key bytes' }
    for ($index = 0; $index -lt $length; $index++) {
        if ($bytes[$index] -eq 0 -or $bytes[$index] -eq 10 -or $bytes[$index] -eq 13) {
            throw 'API-key file must contain exactly one non-empty line'
        }
    }
    [Text.UTF8Encoding]::new($false, $true).GetString($bytes, 0, $length) | Out-Null
}

function Get-RequiredReleaseField([object]$Release, [string]$ReleaseId, [string]$Field) {
    $property = $Release.PSObject.Properties[$Field]
    if ($null -eq $property -or $null -eq $property.Value -or
        ($property.Value -is [string] -and [string]::IsNullOrWhiteSpace([string]$property.Value))) {
        throw "installed release '$ReleaseId' is missing required field: $Field"
    }
    return $property.Value
}

function Copy-InstalledRelease([string]$ReleaseId, [object]$Release) {
    $values = [ordered]@{}
    foreach ($field in @(
            'release_root', 'server_executable', 'model_artifact', 'config_file',
            'api_key_file', 'host', 'port', 'deployment_profile', 'upstream_base_sha',
            'lineage_base_sha', 'patch_stack_sha', 'build_profile', 'cuda_architecture',
            'ninfer_sha256', 'binary_sha256', 'benchmark_sha256', 'package_sha256',
            'inner_checksums_sha256', 'config_sha256', 'model_artifact_sha256',
            'model_bytes', 'model_creation_utc_ticks', 'model_last_write_utc_ticks',
            'model_reference', 'cache_root', 'receipts_root', 'gpu_index', 'gpu_uuid',
            'gpu_name', 'installed_utc'
        )) {
        $values[$field] = Get-RequiredReleaseField $Release $ReleaseId $field
    }
    foreach ($field in @(
            'ninfer_sha256', 'binary_sha256', 'benchmark_sha256', 'package_sha256',
            'inner_checksums_sha256', 'config_sha256', 'model_artifact_sha256'
        )) {
        if ([string]$values[$field] -cnotmatch '^[0-9a-f]{64}$') {
            throw "installed release '$ReleaseId' has an invalid SHA-256 field: $field"
        }
    }
    foreach ($field in @('upstream_base_sha', 'lineage_base_sha', 'patch_stack_sha')) {
        if ([string]$values[$field] -cnotmatch '^[0-9a-f]{40}$') {
            throw "installed release '$ReleaseId' has an invalid Git identity: $field"
        }
    }

    $modelPath = [IO.Path]::GetFullPath([string]$values.model_artifact)
    if (-not (Test-Path -LiteralPath $modelPath -PathType Leaf)) {
        throw "installed release '$ReleaseId' model artifact is missing"
    }
    $modelItem = Get-Item -LiteralPath $modelPath
    if ([Int64]$modelItem.Length -ne [Int64]$values.model_bytes -or
        [Int64]$modelItem.CreationTimeUtc.Ticks -ne [Int64]$values.model_creation_utc_ticks -or
        [Int64]$modelItem.LastWriteTimeUtc.Ticks -ne [Int64]$values.model_last_write_utc_ticks) {
        throw "installed release '$ReleaseId' model artifact changed after install"
    }
    $releaseRoot = [IO.Path]::GetFullPath([string]$values.release_root)
    if ([string]$values.model_reference -cne 'external-pinned-read-only' -or
        (Test-PathWithinRoot $modelPath $StateRoot)) {
        throw "installed release '$ReleaseId' stores an external model reference inside its candidate root"
    }

    $copy = [ordered]@{
        release_root = $releaseRoot
        server_executable = [string]$values.server_executable
        model_artifact = $modelPath
        model_reference = [string]$values.model_reference
        config_file = [string]$values.config_file
        cache_root = [string]$values.cache_root
        receipts_root = [string]$values.receipts_root
        api_key_file = [string]$values.api_key_file
        host = [string]$values.host
        port = [int]$values.port
        deployment_profile = [string]$values.deployment_profile
        upstream_base_sha = [string]$values.upstream_base_sha
        lineage_base_sha = [string]$values.lineage_base_sha
        patch_stack_sha = [string]$values.patch_stack_sha
        build_profile = [string]$values.build_profile
        cuda_architecture = [string]$values.cuda_architecture
        ninfer_sha256 = [string]$values.ninfer_sha256
        binary_sha256 = [string]$values.binary_sha256
        benchmark_sha256 = [string]$values.benchmark_sha256
        package_sha256 = [string]$values.package_sha256
        inner_checksums_sha256 = [string]$values.inner_checksums_sha256
        model_artifact_sha256 = [string]$values.model_artifact_sha256
        model_bytes = [Int64]$values.model_bytes
        model_creation_utc_ticks = [Int64]$values.model_creation_utc_ticks
        model_last_write_utc_ticks = [Int64]$values.model_last_write_utc_ticks
        config_sha256 = [string]$values.config_sha256
        gpu_index = [int]$values.gpu_index
        gpu_uuid = [string]$values.gpu_uuid
        gpu_name = [string]$values.gpu_name
        installed_utc = [string]$values.installed_utc
    }
    # The capability is the installed release's own and must survive every reconstruction of
    # its record: a copy that dropped it would turn the incumbent's next stop into a termination,
    # which is exactly the loss the channel exists to prevent. A record that never had it is a
    # release that predates the channel, and stays so.
    $declared = $Release.PSObject.Properties['managed_stop']
    if ($null -ne $declared -and -not [string]::IsNullOrWhiteSpace([string]$declared.Value)) {
        $copy['managed_stop'] = [string]$declared.Value
        $bound = $Release.PSObject.Properties['graceful_stop_timeout_seconds']
        if ($null -eq $bound) {
            throw "installed release '$ReleaseId' declares a stop channel without a bounded graceful wait"
        }
        $copy['graceful_stop_timeout_seconds'] = [int]$bound.Value
    }
    $pci = $Release.PSObject.Properties['gpu_pci_bus_id']
    if ($null -ne $pci) { $copy['gpu_pci_bus_id'] = [string]$pci.Value }
    return $copy
}

function Get-InstalledReleases([object]$State) {
    if ($State.artifact_type -cne 'ninfer_windows_lifecycle_state' -or
        [int]$State.schema_version -ne 4) {
        throw 'installed release lifecycle state envelope mismatch'
    }
    $releasesProperty = $State.PSObject.Properties['releases']
    if ($null -eq $releasesProperty -or $null -eq $releasesProperty.Value) {
        throw 'installed release lifecycle state has no release records'
    }

    if ($null -eq $State.PSObject.Properties['prepared_release'] -or
        -not [string]::IsNullOrWhiteSpace([string]$State.prepared_release)) {
        throw 'installed lifecycle state has an unresolved prepared release'
    }
    $installed = [ordered]@{}
    foreach ($property in $releasesProperty.Value.PSObject.Properties) {
        if ([string]::IsNullOrWhiteSpace([string]$property.Name)) {
            throw 'installed release lifecycle state has an invalid release identity'
        }
        $installed[$property.Name] = Copy-InstalledRelease $property.Name $property.Value
    }
    $activeRelease = [string](Get-RequiredReleaseField $State 'lifecycle state' 'active_release')
    if (-not $installed.Contains($activeRelease)) {
        throw 'installed lifecycle active release record is missing'
    }
    $previous = [string]$State.previous_release
    if (-not [string]::IsNullOrWhiteSpace($previous) -and -not $installed.Contains($previous)) {
        throw 'installed lifecycle previous release record is missing'
    }
    return $installed
}

function Assert-HostPrerequisites([object]$Spec) {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or -not [Environment]::Is64BitOperatingSystem -or [IntPtr]::Size -ne 8) {
        throw 'this release requires 64-bit Windows on x86-64'
    }
    if (Test-Path Env:CUDA_VISIBLE_DEVICES) { throw 'CUDA_VISIBLE_DEVICES must be absent for bound GPU ordinal identity' }
    $rows = @(& (Get-TrustedNvidiaSmiPath) --query-gpu=index,uuid,name,pci.bus_id,compute_cap,driver_version --format=csv,noheader,nounits 2>&1)
    if ($LASTEXITCODE -ne 0 -or $rows.Count -eq 0) { throw 'NVIDIA GPU prerequisite query failed' }
    $qualified = @()
    foreach ($row in $rows) {
        $parts = @(([string]$row -split ',') | ForEach-Object { $_.Trim() })
        $rowIndex = -1
        if ($parts.Count -ne 6 -or -not [int]::TryParse($parts[0], [ref]$rowIndex) -or $rowIndex -lt 0 -or
            $parts[1] -cnotmatch '^GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$' -or
            $parts[3] -notmatch '^[0-9a-f]{8}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$') {
            throw 'NVIDIA GPU prerequisite query returned an invalid row'
        }
        if ($GpuUuid -and $parts[1] -ine $GpuUuid) { continue }
        if ($parts[2] -ceq [string]$Spec.gpu.name -and $parts[4] -ceq [string]$Spec.gpu.compute_capability) {
            $qualified += [pscustomobject]@{ index = $rowIndex; uuid = $parts[1]; name = $parts[2]; pci_bus_id = $parts[3].ToUpperInvariant(); driver = $parts[5] }
        }
    }
    if ($qualified.Count -eq 0) { throw 'no qualified GPU is installed (or -GpuUuid is not qualified for this lane)' }
    if ($qualified.Count -ne 1) { throw 'more than one qualified GPU is installed; select one with -GpuUuid' }
    if ([Version]$qualified[0].driver -lt [Version]("$([int]$Spec.gpu.minimum_driver_major).0")) { throw 'NVIDIA driver is too old' }
    return $qualified[0]
}

function Get-TarCommand {
    $command = Get-Command tar.exe -ErrorAction SilentlyContinue
    if ($null -eq $command) { $command = Get-Command tar -ErrorAction SilentlyContinue }
    if ($null -eq $command) { throw 'the operating-system tar command is required' }
    return $command.Source
}

function Expand-ReleasePackage([string]$Package, [string]$Destination) {
    $tar = Get-TarCommand
    $entries = @(& $tar '-tzf' $Package)
    if ($LASTEXITCODE -ne 0 -or $entries.Count -eq 0) {
        throw 'release package member inventory failed'
    }
    $rootName = $null
    $members = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($rawEntry in $entries) {
        $entry = ([string]$rawEntry).Replace('\', '/').TrimEnd('/')
        if ([string]::IsNullOrWhiteSpace($entry) -or $entry.StartsWith('/') -or
            $entry.Contains(':') -or -not $members.Add($entry)) {
            throw 'release package contains an invalid or duplicate member path'
        }
        $segments = @($entry.Split('/'))
        if ($segments.Count -eq 0 -or @($segments | Where-Object { $_ -in @('', '.', '..') }).Count -ne 0) {
            throw 'release package member path escapes its release root'
        }
        if ($null -eq $rootName) { $rootName = $segments[0] }
        elseif ([string]$segments[0] -cne [string]$rootName) {
            throw 'release package must contain exactly one release root'
        }
    }
    & $tar '-xzf' $Package '-C' $Destination
    if ($LASTEXITCODE -ne 0) { throw 'release package extraction failed' }
}

function Assert-PayloadChecksums([string]$PayloadRoot) {
    $manifestPath = Join-Path $PayloadRoot 'SHA256SUMS.txt'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw 'package checksum manifest is missing'
    }
    $directorySeparators = [char[]]@(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar
    )
    $root = [IO.Path]::GetFullPath($PayloadRoot).TrimEnd($directorySeparators) +
        [IO.Path]::DirectorySeparatorChar
    $listed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in @([IO.File]::ReadAllLines($manifestPath, [Text.Encoding]::ASCII))) {
        if ($line -cnotmatch '^([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._/-]*)$') {
            throw 'package checksum manifest contains a malformed entry'
        }
        $expected = [string]$Matches[1]
        $manifestRelative = [string]$Matches[2]
        $relative = $manifestRelative.Replace('/', [IO.Path]::DirectorySeparatorChar)
        if (-not $listed.Add($relative) -or
            [string]::Equals($relative, 'SHA256SUMS.txt', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'package checksum manifest contains an invalid or duplicate path'
        }
        $candidate = [IO.Path]::GetFullPath((Join-Path $PayloadRoot $relative))
        if (-not $candidate.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'package checksum path escapes its payload root'
        }
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            throw "package file is missing: $relative"
        }
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $candidate).Hash.ToLowerInvariant()
        if ($actual -cne $expected) {
            throw "package file SHA-256 mismatch: $relative"
        }
    }
    foreach ($file in Get-ChildItem -LiteralPath $PayloadRoot -File -Recurse -Force) {
        $relative = $file.FullName.Substring($root.Length)
        if ([string]::Equals($relative, 'SHA256SUMS.txt', [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        if (-not $listed.Contains($relative)) { throw "package contains an unverified file: $relative" }
    }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $manifestPath).Hash.ToLowerInvariant()
}

# The server loads the DLLs beside its executable ahead of any system location, so an installed
# release is intact only while bin/*.dll still matches the package manifest its install retained
# in receipts and bound to the release record by hash: none replaced, removed, or planted.
function Assert-AppLocalDlls([object]$Release) {
    $manifestPath = Join-Path ([string]$Release.receipts_root) 'SHA256SUMS.txt'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $manifestPath).Hash.ToLowerInvariant() -cne
            [string]$Release.inner_checksums_sha256) {
        throw 'retained package manifest no longer matches its recorded identity'
    }
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
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLowerInvariant() -cne $expected[$file.Name]) {
            throw "app-local DLL $($file.Name) SHA-256 mismatch"
        }
        [void]$present.Add($file.Name)
    }
    foreach ($name in $expected.Keys) {
        if (-not $present.Contains($name)) { throw "app-local DLL is missing: $name" }
    }
}

function Register-ReleaseTask([string]$TaskName, [string]$ControllerPath) {
    $quotedController = '"' + $ControllerPath.Replace('"', '""') + '"'
    $quotedState = '"' + $StateRoot.Replace('"', '""') + '"'
    $arguments = "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $quotedController -Action Run -StateRoot $quotedState"
    $taskAction = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arguments
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -RestartCount 3 -RestartInterval ([TimeSpan]::FromMinutes(1)) -StartWhenAvailable
    $taskPrincipal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $TaskName -Action $taskAction -Description ('Owns only the isolated NInfer Qwen3.8 ' + [string]$script:releaseGpuName + ' native release process; explicit start only.') -Settings $settings -Principal $taskPrincipal -Force | Out-Null
}

function Assert-AllowedListenHost([object]$Spec, [object]$Config) {
    $expectedClasses = @('loopback', 'tailscale-ipv4')
    $specClasses = @($Spec.network.allowed_listen_host_classes)
    $configClasses = @($Config.listen.allowed_host_classes)
    foreach ($classes in @($specClasses, $configClasses)) {
        if ($classes.Count -ne $expectedClasses.Count) {
            throw 'release listen-host policy is not loopback/Tailscale-only'
        }
        for ($index = 0; $index -lt $expectedClasses.Count; $index++) {
            if ([string]$classes[$index] -cne $expectedClasses[$index]) {
                throw 'release listen-host policy is not loopback/Tailscale-only'
            }
        }
    }
    if ([string]$Spec.network.authentication -cne 'required-api-key-file' -or
        [string]$Config.authentication.mode -cne 'required-api-key-file') {
        throw 'release authentication policy must require an API-key file'
    }

    $address = $null
    if (-not [Net.IPAddress]::TryParse([string]$Config.listen.host, [ref]$address) -or
        $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
        throw 'release listen host must be an IPv4 loopback or Tailscale address'
    }
    $bytes = $address.GetAddressBytes()
    $loopback = $bytes[0] -eq 127
    $tailscale = $bytes[0] -eq 100 -and $bytes[1] -ge 64 -and $bytes[1] -le 127
    if (-not $loopback -and -not $tailscale) {
        throw 'release listen host must be an IPv4 loopback or Tailscale address'
    }
}

function Assert-NativeLaneRuntimeContract([object]$Spec, [object]$Config) {
    if ([int]$Spec.schema_version -ne 5) {
        throw 'release specification schema must be 5'
    }
    if ($null -eq $Spec.PSObject.Properties['runtime_shape'] -or
        $null -eq $Spec.runtime_shape.PSObject.Properties['fit']) {
        throw 'release specification must declare runtime_shape.fit'
    }
    $shape = $Spec.runtime_shape
    $fit = $shape.fit
    $validSpeculation = [int]$shape.max_concurrency -gt 0 -and
        [int]$Config.engine.max_concurrency -eq [int]$shape.max_concurrency -and
        @('mtp', 'dflash2') -ccontains [string]$shape.speculative_backend -and
        [string]$Config.speculative.backend -ceq [string]$shape.speculative_backend -and
        [int]$shape.draft_tokens.min -ge 1 -and
        [int]$Config.speculative.draft_tokens -ge [int]$shape.draft_tokens.min -and
        ($null -eq $shape.draft_tokens.max -or
         [int]$Config.speculative.draft_tokens -le [int]$shape.draft_tokens.max) -and
        ([string]$shape.speculative_backend -cne 'dflash2' -or
         ($null -ne $shape.draft_tokens.max -and
          [int]$shape.draft_tokens.min -eq [int]$shape.draft_tokens.max))
    $proposal = $shape.PSObject.Properties['proposal']
    $configProposal = $Config.speculative.PSObject.Properties['proposal']
    $validProposal = $null -ne $proposal -and
        -not [string]::IsNullOrWhiteSpace([string]$proposal.Value) -and
        $(if ([string]$proposal.Value -ceq 'artifact-default') {
            $null -eq $configProposal
        } else {
            $null -ne $configProposal -and [string]$configProposal.Value -ceq [string]$proposal.Value
        })
    $validFit = ($fit.device_state_slots -is [int] -or $fit.device_state_slots -is [long]) -and
        $fit.device_state_slots -ge 0 -and
        ($Config.context_cache.device_state_slots -is [int] -or
         $Config.context_cache.device_state_slots -is [long]) -and
        $Config.context_cache.device_state_slots -eq $fit.device_state_slots -and
        ($fit.admission_headroom_mib -is [int] -or $fit.admission_headroom_mib -is [long]) -and
        $fit.admission_headroom_mib -ge 256 -and
        ($Config.engine.admission_headroom_mib -is [int] -or
         $Config.engine.admission_headroom_mib -is [long]) -and
        $Config.engine.admission_headroom_mib -eq $fit.admission_headroom_mib
    $nativeSeam = [string]$shape.speculative_backend -ceq 'dflash2' -and
        @('86', '89') -ccontains [string]$Spec.gpu.cmake_cuda_architecture
    $expectedFormat = if ($nativeSeam) { 'Q4_G64_FP16' } else { 'artifact' }
    $expectedResidency = if ($nativeSeam) { 'host-mapped' } else { 'device' }
    $validFit = $validFit -and
        [string]$fit.draft_mlp_device_format -ceq $expectedFormat -and
        [string]$fit.token_embedding_residency -ceq $expectedResidency
    if ([int]$shape.max_concurrency -gt 1 -and
        @('86', '89') -ccontains [string]$Spec.gpu.cmake_cuda_architecture) {
        # Extra images are additional to active lanes. KV and Host capacity stay shared.
        $headroomFloor = if ([string]$Spec.gpu.cmake_cuda_architecture -ceq '86') { 768 } else { 640 }
        $pendingTimeout = $Config.engine.PSObject.Properties['pending_timeout_ms']
        $validFit = $validFit -and $fit.device_state_slots -ge 2 -and
            $fit.admission_headroom_mib -ge $headroomFloor -and
            $null -ne $pendingTimeout -and
            ($pendingTimeout.Value -is [int] -or $pendingTimeout.Value -is [long]) -and
            $pendingTimeout.Value -ge 180000 -and $pendingTimeout.Value -le [int]::MaxValue
    }
    $translation = $Spec.host_context_translation
    $hostContextBytes = [decimal]$translation.predecessor_host_kv_mib * 1048576 +
        [decimal]$translation.predecessor_host_state_slots * [decimal]$translation.native_state_image_bytes
    # A PowerShell string-to-[decimal] cast rounds through Double; parse exact MiB text instead.
    $hostContextMib = [decimal]::Parse([string]$Config.context_cache.host_context_mib,
                                      [Globalization.CultureInfo]::InvariantCulture)
    if ($Config.artifact_type -cne 'ninfer_windows_server_config' -or
        [int]$Config.schema_version -ne 4 -or
        [string]$Config.release_id -cne [string]$Spec.release_id -or
        [string]$Config.deployment_profile -cne [string]$Spec.deployment_profile -or
        -not $validSpeculation -or -not $validProposal -or -not $validFit -or
        [decimal]$translation.host_context_bytes -ne $hostContextBytes -or
        [string]$Config.context_cache.host_context_mib -cnotmatch '^[0-9]+(?:\.[0-9]+)?$' -or
        $hostContextMib -le 0 -or
        $hostContextMib * 1048576 -ne $hostContextBytes -or
        -not [bool]$Config.session_checkpoint.enabled -or
        $Config.session_checkpoint.require_origin_auth -isnot [bool] -or
        -not $Config.session_checkpoint.require_origin_auth -or
        [int]$Config.session_checkpoint.staging_mib -le 0 -or
        $null -ne $Config.context_cache.PSObject.Properties['host_state_slots'] -or
        $null -ne $Config.context_cache.PSObject.Properties['host_kv_mib'] -or
        $null -ne $Config.context_cache.PSObject.Properties['max_private_continuations'] -or
        $null -ne $Config.PSObject.Properties['persistent_cache'] -or
        $null -ne $Config.reasoning.PSObject.Properties['effort']) {
        throw 'server configuration is not an authenticated native lane profile'
    }
    # Every new package declares keep-warm explicitly, including zero.
    $gpuKeepWarm = $Config.engine.PSObject.Properties['gpu_keep_warm_ms']
    if ($null -eq $gpuKeepWarm -or -not ($gpuKeepWarm.Value -is [int] -or $gpuKeepWarm.Value -is [long]) -or
        $gpuKeepWarm.Value -lt 0 -or $gpuKeepWarm.Value -gt [int]::MaxValue) {
        throw 'server configuration must declare engine.gpu_keep_warm_ms as an integer in [0, 2147483647]'
    }
}

function Assert-InstallerArchitectureContract([object]$Spec, [object]$Config) {
    Assert-NativeLaneRuntimeContract $Spec $Config
    $expectedRoots = @('bin', 'config', 'logs', 'receipts')
    $actualRoots = @($Spec.lifecycle.candidate_root_directories)
    if ($actualRoots.Count -ne $expectedRoots.Count) {
        throw 'release specification candidate root contract mismatch'
    }
    for ($index = 0; $index -lt $expectedRoots.Count; $index++) {
        if ([string]$actualRoots[$index] -cne $expectedRoots[$index]) {
            throw 'release specification candidate root contract mismatch'
        }
    }
    if ([string]$Spec.lifecycle.model_artifact_ownership -cne 'external-pinned-read-only' -or
        [string]$Spec.lifecycle.cache_ownership -cne 'state-root-per-release' -or
        [string]$Spec.lifecycle.interrupted_install_reentry -cne 'repair-required') {
        throw 'release specification installer ownership contract mismatch'
    }
    $expectedPointers = @('prepared_release', 'active_release', 'previous_release')
    if ([string]::Join(',', @($Spec.lifecycle.state_pointers)) -cne
        [string]::Join(',', $expectedPointers)) {
        throw 'release specification lifecycle pointer contract mismatch'
    }
    # This installer ships with a server that stops on its named event; the lane must declare the
    # channel and the bounded wait the controller grants a stop before it terminates the process.
    if ([string]$Spec.lifecycle.managed_stop -cne 'stop-event' -or
        $null -eq $Spec.lifecycle.PSObject.Properties['graceful_stop_timeout_seconds'] -or
        [int]$Spec.lifecycle.graceful_stop_timeout_seconds -lt 1 -or
        [int]$Spec.lifecycle.graceful_stop_timeout_seconds -gt 3600) {
        throw 'release specification managed-stop contract mismatch'
    }
    Assert-AllowedListenHost $Spec $Config
    if ([Int64]$Spec.model.bytes -le 0 -or
        [string]$Spec.model.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        [string]$Spec.model.revision -cnotmatch '^[0-9a-f]{40}$') {
        throw 'release specification pinned model identity mismatch'
    }
    if ([string]$Spec.lane -cnotmatch '^rtx[0-9]{4}$' -or
        [string]$Spec.build_profile -cnotmatch '^native-v[0-9][0-9A-Za-z.-]*-rtx[0-9]{4}$' -or
        [string]$Spec.gpu.cuda_architecture -cne ('sm_' + [string]$Spec.gpu.cmake_cuda_architecture) -or
        [string]$Spec.source.lineage_base_sha -cnotmatch '^[0-9a-f]{40}$' -or
        [string]$Spec.lifecycle.state_root_name -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
        throw 'release specification immutable build identity mismatch'
    }
}

function Set-ObjectProperty([object]$Object, [string]$Name, [object]$Value) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        Add-Member -InputObject $Object -MemberType NoteProperty -Name $Name -Value $Value
    }
    else {
        $property.Value = $Value
    }
}

function Set-TransactionPhase([object]$Transaction, [string]$Phase) {
    Set-ObjectProperty $Transaction 'phase' $Phase
    Write-InstallTransaction $Transaction
}

function Set-TransactionFlag([object]$Transaction, [string]$Name) {
    $property = $Transaction.flags.PSObject.Properties[$Name]
    if ($null -eq $property) {
        Add-Member -InputObject $Transaction.flags -MemberType NoteProperty -Name $Name -Value $true
    }
    else {
        $property.Value = $true
    }
    Write-InstallTransaction $Transaction
}

# install.lock serializes creation in the protected root. Refuse pre-existing trees, then publish
# the cleanup intent before mkdir: repair must also cover death immediately after creation.
function New-TransactionDirectory([object]$Transaction, [string]$Field, [string]$Path) {
    if (Test-Path -LiteralPath $Path) { throw "transaction directory already exists: $Path" }
    Set-ObjectProperty $Transaction $Field $Path
    Write-InstallTransaction $Transaction
    New-Item -ItemType Directory -Path $Path | Out-Null
}

function New-CandidateDirectories(
    [object]$Transaction,
    [string]$ReleaseRoot,
    [string]$SecretDirectory,
    [string]$CacheRoot
) {
    if (Test-Path -LiteralPath $ReleaseRoot) { throw 'release instance is already installed' }
    if (Test-Path -LiteralPath $SecretDirectory) { throw 'release secret instance is already installed' }
    if (Test-Path -LiteralPath $CacheRoot) { throw 'release cache instance is already installed' }
    New-TransactionDirectory $Transaction 'release_root' $ReleaseRoot
    New-TransactionDirectory $Transaction 'secret_directory' $SecretDirectory
    New-TransactionDirectory $Transaction 'cache_root' $CacheRoot
}

function Convert-CommandOutputToJson([object[]]$Output, [string]$Label) {
    $lines = @($Output | ForEach-Object { [string]$_ })
    if ($lines.Count -eq 0) { throw "$Label returned no JSON" }
    return ([string]::Join([Environment]::NewLine, $lines) | ConvertFrom-Json)
}

# The arguments an owner record's policy hands its GPU-owner controller, mapped as the release
# controller maps them, so the installer judges the owner exactly as the lifecycle acts on it.
function Get-GpuOwnerPolicyArguments([object]$Owner, [string]$GpuUuid) {
    $arguments = @{ GpuUuid = $GpuUuid }
    $limit = $Owner.PSObject.Properties['qualified_power_limit_w']
    if ($null -ne $limit -and $null -ne $limit.Value) {
        $arguments['QualifiedPowerLimitW'] = [int]$limit.Value
    }
    $range = $Owner.PSObject.Properties['prior_power_limit_range_w']
    if ($null -ne $range -and $null -ne $range.Value) {
        $arguments['PriorPowerLimitMinW'] = [int]$range.Value[0]
        $arguments['PriorPowerLimitMaxW'] = [int]$range.Value[1]
    }
    return $arguments
}

# A GPU-owner controller's status under one policy must attest the selected card. The owner
# refuses a live lease under any policy but the one that wrote it, so the policy asked is always
# the one holding the owner at that moment.
function Assert-GpuOwnerAttestation([string]$ControllerPath, [hashtable]$Policy, [object]$SelectedGpu) {
    $output = @(& $ControllerPath -Action status -StateRoot $ownerStateRoot @Policy)
    $status = Convert-CommandOutputToJson $output 'GPU-owner controller status'
    if ($null -eq $status.PSObject.Properties['paused'] -or $status.paused -isnot [bool]) {
        throw 'GPU-owner controller status must expose a paused boolean'
    }
    if ($null -eq $status.PSObject.Properties['gpu_uuid'] -or
        [string]$status.gpu_uuid -cne [string]$SelectedGpu.uuid -or
        $null -eq $status.PSObject.Properties['gpu_pci_bus_id'] -or
        [string]$status.gpu_pci_bus_id -cne [string]$SelectedGpu.pci_bus_id) {
        throw 'GPU-owner controller status does not attest the selected UUID and PCI identity'
    }
}

# Before anything changes, the GPU owner is judged under the policy that holds it. A running
# incumbent holds it under its own lifecycle's policy, through the hash-bound controller that
# lifecycle recorded: a successor policy capping the card differently refuses that lease, and
# only the incumbent's managed stop, which needs exactly that controller, retires it. Without a
# running incumbent the successor's policy is the one that acts next.
function Assert-GpuOwnerUpgradeReady(
    [object]$OldState,
    [bool]$IncumbentRunning,
    [string]$SuccessorController,
    [hashtable]$SuccessorPolicy,
    [object]$SelectedGpu
) {
    if (-not $IncumbentRunning) {
        # A dead wrapper can leave the inner claim even when outer status accepts an equal cap.
        # Replacing its controller would strand that claim under the successor's identity.
        # Keep the original lifecycle intact so its managed Stop can reconcile it first.
        if (Test-Path -LiteralPath (Join-Path $StateRoot 'gpu-owner-lease.json')) {
            throw 'non-running incumbent retains a GPU-owner lease; reconcile with its original managed Stop before upgrading'
        }
        Assert-GpuOwnerAttestation $SuccessorController $SuccessorPolicy $SelectedGpu
        return
    }
    $property = $OldState.PSObject.Properties['gpu_owner']
    $owner = if ($null -eq $property) { $null } else { $property.Value }
    if ($null -eq $owner) { throw 'running release has no GPU-owner record; stop it before upgrading' }
    foreach ($field in @('controller_path', 'controller_sha256', 'state_root')) {
        if ($null -eq $owner.PSObject.Properties[$field] -or [string]::IsNullOrWhiteSpace([string]$owner.$field)) {
            throw "running release GPU-owner record is missing field: $field"
        }
    }
    if (-not [string]::Equals([IO.Path]::GetFullPath([string]$owner.state_root),
                              [IO.Path]::GetFullPath($ownerStateRoot), [StringComparison]::OrdinalIgnoreCase)) {
        throw 'running release GPU-owner state root is outside the managed release root'
    }
    $controller = [string]$owner.controller_path
    if (-not (Test-Path -LiteralPath $controller -PathType Leaf) -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $controller).Hash.ToLowerInvariant() -cne [string]$owner.controller_sha256) {
        throw 'running release GPU-owner controller no longer matches its recorded identity'
    }
    if (-not (Get-Command -Name $controller).Parameters.ContainsKey('GpuUuid')) {
        throw 'legacy unscoped GPU-owner wrapper is running; establish safe quiescence before upgrade'
    }
    Assert-GpuOwnerAttestation $controller (Get-GpuOwnerPolicyArguments $owner ([string]$SelectedGpu.uuid)) $SelectedGpu
}

# Stop the running incumbent with its own controller, whose managed stop retires its GPU-owner
# lease under its own policy; only then does the successor's policy judge the owner. A lease the
# stop hands back to an earlier owner pause stays the incumbent's, and the successor refuses it
# here, before either controller is replaced.
function Stop-IncumbentForUpgrade(
    [object]$Transaction,
    [string]$SuccessorController,
    [hashtable]$SuccessorPolicy,
    [object]$SelectedGpu
) {
    Set-TransactionFlag $Transaction 'incumbent_touched'
    & $controllerPath -Action Stop -StateRoot $StateRoot | Out-Null
    Assert-GpuOwnerAttestation $SuccessorController $SuccessorPolicy $SelectedGpu
}

# The candidate is quiescent once nothing it launched can still run. While the lifecycle state
# names it active it may be running, and its own controller stops it, restoring the GPU owner and
# retiring the lease it holds; a stop that fails leaves that state in place, so a repair re-attempts
# the same stop for the same instance. Whatever the state names, no process may still execute
# from the candidate's release root. The rollback itself never removes a GPU-owner lease.
function Assert-TransactionCandidateQuiescent([object]$Transaction, [string]$StatePath, [string]$ControllerPath) {
    if (-not (Get-TransactionFlag $Transaction 'state_activated')) { return }
    if ((Test-Path -LiteralPath $StatePath -PathType Leaf) -and
        [string](Read-JsonFile $StatePath).active_release -ceq [string]$Transaction.instance_id) {
        & $ControllerPath -Action Stop -StateRoot $StateRoot | Out-Null
    }
    $releaseRoot = [string]$Transaction.release_root
    if (-not [string]::IsNullOrWhiteSpace($releaseRoot)) {
        $running = @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop | Where-Object {
                -not [string]::IsNullOrWhiteSpace([string]$_.ExecutablePath) -and
                (Test-PathWithinRoot ([string]$_.ExecutablePath) $releaseRoot)
            } | ForEach-Object { [string]$_.ProcessId })
        if ($running.Count -ne 0) {
            throw "candidate processes still run from its release root: $([string]::Join(', ', $running))"
        }
    }
}

function Invoke-InstallTransactionRollback(
    [object]$Transaction,
    [string]$StatePath,
    [string]$ControllerPath,
    [string]$OwnerControllerPath,
    [ValidateSet('rollback', 'repair')][string]$Mode
) {
    $failures = [Collections.Generic.List[string]]::new()
    $cleanupActions = [Collections.Generic.List[object]]::new()
    Write-InstallEvent "${Mode}_started" "Installer $Mode started" ([ordered]@{
            operation_id = [string]$Transaction.operation_id
            phase = [string]$Transaction.phase
        })

    # Nothing is restored, deleted, or restarted until the candidate is proven quiescent: one that
    # may still run keeps the lifecycle state, controller, trees, and lease that stop it.
    try { Assert-TransactionCandidateQuiescent $Transaction $StatePath $ControllerPath }
    catch { $failures.Add("candidate quiescence: $(Get-BoundedDiagnostic $_.Exception.Message)") }
    if ($failures.Count -eq 0) {
        if (Get-TransactionFlag $Transaction 'state_activated') {
            try { Restore-PersistedFileSnapshot $StatePath $Transaction.snapshots.state }
            catch { $failures.Add("state restore: $(Get-BoundedDiagnostic $_.Exception.Message)") }
        }
        if (Get-TransactionFlag $Transaction 'controller_touched') {
            try { Restore-PersistedFileSnapshot $ControllerPath $Transaction.snapshots.controller }
            catch { $failures.Add("controller restore: $(Get-BoundedDiagnostic $_.Exception.Message)") }
        }
        if (Get-TransactionFlag $Transaction 'task_touched') {
            try {
                $taskXml = if ([bool]$Transaction.task_existed) {
                    Get-Content -LiteralPath ([string]$Transaction.task_xml_path) -Raw -Encoding UTF8
                }
                else {
                    $null
                }
                Restore-ReleaseTask ([string]$Transaction.task_name) ([bool]$Transaction.task_existed) $taskXml
            }
            catch { $failures.Add("task restore: $(Get-BoundedDiagnostic $_.Exception.Message)") }
        }
        if (Get-TransactionFlag $Transaction 'owner_controller_touched') {
            try { Restore-PersistedFileSnapshot $OwnerControllerPath $Transaction.snapshots.owner_controller }
            catch { $failures.Add("GPU-owner controller restore: $(Get-BoundedDiagnostic $_.Exception.Message)") }
        }
        if (Get-TransactionFlag $Transaction 'state_helpers_touched') {
            try { Restore-PersistedFileSnapshot $stateHelperPath $Transaction.snapshots.state_helper }
            catch { $failures.Add("state helper restore: $(Get-BoundedDiagnostic $_.Exception.Message)") }
            try { Restore-PersistedFileSnapshot $ownerStateHelperPath $Transaction.snapshots.owner_state_helper }
            catch { $failures.Add("GPU-owner state helper restore: $(Get-BoundedDiagnostic $_.Exception.Message)") }
        }
    }

    # The candidate's trees are deleted, and the incumbent restarted, only over the restored
    # lifecycle: against a state that still names the candidate, the restart would launch it.
    if ($failures.Count -eq 0) {
        $cleanupTargets = @(Get-TransactionCleanupTargets $Transaction)
        Write-InstallEvent 'cleanup_inventory' 'Enumerated interrupted install cleanup targets' ([ordered]@{
                operation_id = [string]$Transaction.operation_id
                target_count = $cleanupTargets.Count
                targets = @($cleanupTargets | ForEach-Object { [string]$_.kind })
            })
        foreach ($target in $cleanupTargets) {
            try {
                $cleanupActions.Add((Remove-InstallTreeStreaming $target))
            }
            catch {
                $failures.Add("$([string]$target.kind) cleanup: $(Get-BoundedDiagnostic $_.Exception.Message)")
            }
        }

        if (Get-TransactionFlag $Transaction 'incumbent_touched') {
            try { & $ControllerPath -Action Start -StateRoot $StateRoot | Out-Null }
            catch { $failures.Add("incumbent restart: $(Get-BoundedDiagnostic $_.Exception.Message)") }
        }
    }

    $cleanupArray = @($cleanupActions | ForEach-Object { $_ })
    $diagnosticArray = @($failures | Select-Object -First 16 | ForEach-Object { [string]$_ })
    Set-ObjectProperty $Transaction 'cleanup_actions' $cleanupArray
    Set-ObjectProperty $Transaction 'diagnostics' $diagnosticArray
    if ($failures.Count -eq 0) {
        Set-ObjectProperty $Transaction 'status' $(if ($Mode -ceq 'repair') { 'repaired' } else { 'rolled_back' })
        Set-ObjectProperty $Transaction 'phase' $(if ($Mode -ceq 'repair') { 'repair_completed' } else { 'rollback_completed' })
        Write-InstallTransaction $Transaction
        Write-InstallEvent "${Mode}_completed" "Installer $Mode completed" ([ordered]@{
                operation_id = [string]$Transaction.operation_id
                cleanup_action_count = $cleanupArray.Count
            })
        return $cleanupArray
    }

    Set-ObjectProperty $Transaction 'status' 'repair_required'
    Set-ObjectProperty $Transaction 'phase' "${Mode}_incomplete"
    Write-InstallTransaction $Transaction
    Write-InstallEvent ($Mode + '_incomplete') "Installer $Mode requires repair" ([ordered]@{
            operation_id = [string]$Transaction.operation_id
            diagnostics = $diagnosticArray
        })
    throw "installer $Mode was incomplete: $([string]::Join('; ', $diagnosticArray))"
}

$StateRoot = Initialize-NInferProtectedStateRoot $StateRoot
Protect-NInferRetainedSecretAcls $StateRoot
Assert-NInferProtectedStateTree $StateRoot
$statePath = Join-Path $StateRoot 'state.json'
$controllerPath = Join-Path $StateRoot 'Control-Release.ps1'
$ownerControllerPath = Join-Path (Join-Path $StateRoot 'gpu-owner') 'Control-GpuOwner.ps1'
$ownerStateRoot = Join-Path $StateRoot 'gpu-owner-state'
$stateHelperPath = Join-Path $StateRoot 'Protect-StateRoot.ps1'
$ownerStateHelperPath = Join-Path (Split-Path -Parent $ownerControllerPath) 'Protect-StateRoot.ps1'
$receiptsStateRoot = Join-Path $StateRoot 'receipts'
$transactionsRoot = Join-Path $receiptsStateRoot 'install-transactions'
New-Item -ItemType Directory -Force -Path $transactionsRoot | Out-Null
$installLockPath = Join-Path $StateRoot 'install.lock'
$installLock = $null

try {
    try {
        $installLock = [IO.File]::Open(
            $installLockPath,
            [IO.FileMode]::OpenOrCreate,
            [IO.FileAccess]::ReadWrite,
            [IO.FileShare]::None
        )
    }
    catch {
        throw 'another installer or installer-repair operation is already running; do not retry qualification'
    }

    $pendingTransactions = @(Get-PendingInstallTransactions $transactionsRoot)
    if ($RepairInterruptedInstall) {
        $repairStartedUtc = [DateTime]::UtcNow.ToString('o')
        $transactionResults = [Collections.Generic.List[object]]::new()
        foreach ($pending in $pendingTransactions) {
            $cleanupActions = @(Invoke-InstallTransactionRollback $pending $statePath $controllerPath $ownerControllerPath 'repair')
            $transactionResults.Add([ordered]@{
                    operation_id = [string]$pending.operation_id
                    status = 'repaired'
                    cleanup_actions = @($cleanupActions | ForEach-Object { $_ })
                })
        }
        $repairTransactions = @($transactionResults | ForEach-Object { $_ })
        $cleanupActionCount = 0
        foreach ($transactionResult in $repairTransactions) {
            $cleanupActionCount += @($transactionResult.cleanup_actions).Count
        }
        $repairReceipt = [ordered]@{
            artifact_type = 'ninfer_windows_installer_repair_receipt'
            schema_version = 1
            status = 'passed'
            started_utc = $repairStartedUtc
            completed_utc = [DateTime]::UtcNow.ToString('o')
            transaction_count = $repairTransactions.Count
            cleanup_action_count = $cleanupActionCount
            transactions = $repairTransactions
            reentry_policy = 'repair-required-before-install'
        }
        $repairReceiptPath = Join-Path $receiptsStateRoot (
            'installer-repair-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '.json'
        )
        Write-JsonAtomic $repairReceiptPath $repairReceipt
        Write-Output ($repairReceipt | ConvertTo-Json -Depth 20 -Compress)
        return
    }

    if ($pendingTransactions.Count -ne 0) {
        Write-InstallEvent 'install_blocked' 'Interrupted installer state requires repair before re-entry' ([ordered]@{
                pending_transaction_count = $pendingTransactions.Count
                operation_ids = @($pendingTransactions | ForEach-Object { [string]$_.operation_id })
            })
        throw 'interrupted installer transaction requires installer repair before another install; run only -RepairInterruptedInstall'
    }

    $package = (Resolve-Path -LiteralPath $PackagePath).Path
    if ([IO.Path]::GetFileName($package) -cnotmatch '^ninfer-rtx[0-9]{4}-native-v[0-9][0-9A-Za-z.-]*-windows-x86_64-cuda[0-9.]+-rtx[0-9]{4}\.tar\.gz$') {
        throw 'unexpected release package filename'
    }
    Write-InstallEvent 'package_identity_started' 'Verifying release package identity' ([ordered]@{ path = $package })
    $actualPackageSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $package).Hash.ToLowerInvariant()
    if ($actualPackageSha -cne $PackageSha256) { throw 'release package SHA-256 mismatch' }
    Write-InstallEvent 'package_identity_completed' 'Verified release package identity' ([ordered]@{ sha256 = $actualPackageSha })
    $apiKeySource = (Resolve-Path -LiteralPath $ApiKeyFile).Path
    Assert-OneLineSecret $apiKeySource

    $oldState = if (Test-Path -LiteralPath $statePath -PathType Leaf) { Read-JsonFile $statePath } else { $null }
    $installedReleases = if ($null -eq $oldState) { [ordered]@{} } else { Get-InstalledReleases $oldState }
    $ownerControllerSource = $null
    $ownerRecord = $null
    # Upgrade the managed controller with the installer, never inherit an unscoped adapter.
    if ([string]::IsNullOrWhiteSpace($GpuOwnerControllerPath)) {
        $bundledOwnerController = Join-Path $PSScriptRoot 'Control-GpuOwner.ps1'
        if (Test-Path -LiteralPath $bundledOwnerController -PathType Leaf) {
            $GpuOwnerControllerPath = $bundledOwnerController
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($GpuOwnerControllerPath)) {
        if (-not (Test-Path -LiteralPath $GpuOwnerControllerPath -PathType Leaf)) {
            throw 'GPU-owner controller does not exist'
        }
        $ownerControllerSource = (Resolve-Path -LiteralPath $GpuOwnerControllerPath).Path
        if (-not (Get-Command -Name $ownerControllerSource).Parameters.ContainsKey('GpuUuid')) {
            throw 'GPU-owner controller must support -GpuUuid; use the bundled controller'
        }
        $ownerRecord = [ordered]@{
            controller_path = $ownerControllerPath
            controller_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $ownerControllerSource).Hash.ToLowerInvariant()
            state_root = $ownerStateRoot
        }
    }
    if ($null -eq $ownerRecord) {
        throw 'a UUID-aware GPU-owner controller is required'
    }

    $operationId = [Guid]::NewGuid().ToString('N')
    $transactionRoot = Join-Path $transactionsRoot $operationId
    $snapshotRoot = Join-Path $transactionRoot 'snapshots'
    New-Item -ItemType Directory -Force -Path $snapshotRoot | Out-Null
    $stage = Join-Path $StateRoot ('staging-' + $operationId)
    $journalPath = Join-Path $transactionRoot 'journal.json'
    $transaction = [pscustomobject][ordered]@{
        artifact_type = 'ninfer_windows_install_transaction'
        schema_version = 1
        status = 'in_progress'
        operation_id = $operationId
        phase = 'initialized'
        started_utc = [DateTime]::UtcNow.ToString('o')
        updated_utc = [DateTime]::UtcNow.ToString('o')
        journal_path = $journalPath
        state_root = $StateRoot
        stage_path = $null
        instance_id = $null
        release_root = $null
        secret_directory = $null
        cache_root = $null
        task_name = $null
        task_existed = $false
        task_was_running = $false
        task_xml_path = $null
        reentry_policy = 'repair-required-before-install'
        snapshots = [pscustomobject][ordered]@{
            state = Save-PersistedFileSnapshot $statePath $snapshotRoot 'state'
            controller = Save-PersistedFileSnapshot $controllerPath $snapshotRoot 'controller'
            owner_controller = Save-PersistedFileSnapshot $ownerControllerPath $snapshotRoot 'owner-controller'
            state_helper = Save-PersistedFileSnapshot $stateHelperPath $snapshotRoot 'state-helper'
            owner_state_helper = Save-PersistedFileSnapshot $ownerStateHelperPath $snapshotRoot 'owner-state-helper'
        }
        flags = [pscustomobject][ordered]@{
            incumbent_touched = $false
            owner_controller_touched = $false
            state_helpers_touched = $false
            controller_touched = $false
            state_activated = $false
            task_touched = $false
        }
    }
    Write-InstallTransaction $transaction
    New-TransactionDirectory $transaction 'stage_path' $stage
    Write-InstallEvent 'install_started' 'Started durable installer transaction' ([ordered]@{
            operation_id = $operationId
            journal_path = $journalPath
        })
    try {
        Write-InstallEvent 'package_expand_started' 'Expanding verified release package' ([ordered]@{ stage_path = $stage })
        Expand-ReleasePackage $package $stage
        Write-InstallEvent 'package_expand_completed' 'Expanded verified release package' ([ordered]@{ stage_path = $stage })
        $payloadDirectories = @(Get-ChildItem -LiteralPath $stage -Directory)
        if ($payloadDirectories.Count -ne 1) { throw 'package must contain exactly one release root' }
        $payload = $payloadDirectories[0].FullName
        $innerChecksumsSha = Assert-PayloadChecksums $payload

        $buildIdentity = Read-JsonFile (Join-Path $payload 'build-identity.json')
        $spec = Read-JsonFile (Join-Path $payload 'release-spec.json')
        $config = Read-JsonFile (Join-Path $payload 'server-config.json')
        $selectedGpu = Assert-HostPrerequisites $spec
        foreach ($installed in $installedReleases.Values) {
            if ([string]$installed.gpu_uuid -cne [string]$selectedGpu.uuid) {
                throw 'all releases in a state root must retain the same GPU UUID; card replacement requires a separate install'
            }
        }
        Assert-InstallerArchitectureContract $spec $config
        $configSha = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $payload 'server-config.json')).Hash.ToLowerInvariant()
        if ($spec.artifact_type -cne 'ninfer_windows_release_spec' -or
            [int]$spec.schema_version -ne 5 -or
            $config.artifact_type -cne 'ninfer_windows_server_config' -or
            [int]$config.schema_version -ne 4 -or
            $config.release_id -cne $spec.release_id -or
            $config.deployment_profile -cne $spec.deployment_profile -or
            $buildIdentity.artifact_type -cne 'ninfer_release_build_identity' -or
            [int]$buildIdentity.schema_version -ne 2 -or
            $buildIdentity.source_dirty -ne $false) {
            throw 'release package identity mismatch'
        }
        if ([string]$buildIdentity.release_version -cne ('v' + [string]$spec.release_version) -or
            [string]$buildIdentity.platform -cne [string]$spec.platform -or
            [string]$buildIdentity.upstream_base_sha -cne [string]$spec.source.upstream_base_sha -or
            [string]$buildIdentity.lineage_base_sha -cne [string]$spec.source.lineage_base_sha -or
            [string]$buildIdentity.build_profile -cne [string]$spec.build_profile -or
            [string]$buildIdentity.cuda_architecture -cne [string]$spec.gpu.cmake_cuda_architecture -or
            [string]$buildIdentity.configuration_sha256 -cne $configSha) {
            throw 'release package source, build, or configuration identity mismatch'
        }
        foreach ($binaryName in @('ninfer', 'ninfer-serve', 'ninfer_bench')) {
            if ([string]$buildIdentity.binaries.$binaryName -cnotmatch '^[0-9a-f]{64}$') {
                throw "release package binary identity is invalid: $binaryName"
            }
        }

        $taskName = Resolve-LifecycleTaskName $spec $StateRoot $ScratchTaskSuffix
        $ownerProtocol = $spec.lifecycle.gpu_owner_controller_protocol
        $ownerRecord['qualified_power_limit_w'] = if ($null -eq $ownerProtocol.qualified_power_limit_w) { $null } else { [int]$ownerProtocol.qualified_power_limit_w }
        $ownerRecord['prior_power_limit_range_w'] = @([int]$ownerProtocol.prior_power_limit_range_w[0], [int]$ownerProtocol.prior_power_limit_range_w[1])
        $expectedPackageName = [string]$spec.product_prefix + '-v' + [string]$spec.release_version + '-' + [string]$spec.platform + '.tar.gz'
        if ([IO.Path]::GetFileName($package) -cne $expectedPackageName) { throw 'release package filename does not match its specification' }
        if ((Split-Path -Leaf $StateRoot) -cne [string]$spec.lifecycle.state_root_name) { throw 'state root does not match the lane specification' }
        $script:releaseGpuName = [string]$spec.gpu.name
        if ($null -ne $oldState -and [string]$oldState.task_name -cne $taskName) {
            throw 'installed lifecycle task identity mismatch'
        }
        $oldTask = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
        if ($null -eq $oldState -and $null -ne $oldTask) {
            throw 'managed task exists without lifecycle state'
        }
        $oldTaskExisted = $null -ne $oldTask
        $oldTaskWasRunning = $false
        $taskXmlPath = $null
        if ($oldTaskExisted) {
            $oldTaskWasRunning = [string]$oldTask.State -ceq 'Running'
            $taskXmlPath = Join-Path $snapshotRoot 'scheduled-task.xml'
            [IO.File]::WriteAllText(
                $taskXmlPath,
                [string](Export-ScheduledTask -TaskName $taskName),
                [Text.UTF8Encoding]::new($false)
            )
        }
        $successorOwnerPolicy = Get-GpuOwnerPolicyArguments ([pscustomobject]$ownerRecord) ([string]$selectedGpu.uuid)
        Assert-GpuOwnerUpgradeReady $oldState $oldTaskWasRunning $ownerControllerSource $successorOwnerPolicy $selectedGpu
        Set-ObjectProperty $transaction 'task_name' $taskName
        Set-ObjectProperty $transaction 'task_existed' $oldTaskExisted
        Set-ObjectProperty $transaction 'task_was_running' $oldTaskWasRunning
        Set-ObjectProperty $transaction 'task_xml_path' $taskXmlPath
        Set-TransactionPhase $transaction 'package_verified'

        $patchStackSha = [string]$buildIdentity.patch_stack_sha
        if ($patchStackSha -cnotmatch '^[0-9a-f]{40}$') {
            throw 'release package patch-stack identity is invalid'
        }
        $instanceId = "$([string]$spec.release_id)-$($patchStackSha.Substring(0, 12))"
        if ($instanceId -notmatch '^[A-Za-z0-9._-]+$') {
            throw 'release instance identity is invalid'
        }
        $releaseRoot = Join-Path (Join-Path $StateRoot 'releases') $instanceId
        $secretDirectory = Join-Path (Join-Path $StateRoot 'secrets') $instanceId
        $cacheRoot = Join-Path (Join-Path $StateRoot 'cache') $instanceId

        $sourceModel = (Resolve-Path -LiteralPath $ModelArtifactPath).Path
        Assert-ExternalModelReference $sourceModel $StateRoot
        $existing = $installedReleases[$instanceId]
        if ($null -ne $existing) {
            if ([string]$oldState.active_release -cne $instanceId) {
                throw 'release instance is installed but inactive; use exact rollback instead of reinstalling it'
            }
            if ([string]$existing.gpu_uuid -cne [string]$selectedGpu.uuid -or
                [string]$existing.package_sha256 -cne $actualPackageSha -or
                [string]$existing.patch_stack_sha -cne $patchStackSha -or
                [string]$existing.build_profile -cne [string]$buildIdentity.build_profile -or
                [string]$existing.config_sha256 -cne $configSha -or
                -not [string]::Equals(
                    [IO.Path]::GetFullPath([string]$existing.model_artifact),
                    [IO.Path]::GetFullPath($sourceModel),
                    [StringComparison]::OrdinalIgnoreCase
                )) {
                throw 'already-installed release identity differs from the requested package or model reference'
            }
            if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path (Join-Path ([string]$existing.release_root) 'bin') 'ninfer.exe')).Hash.ToLowerInvariant() -cne
                    [string]$existing.ninfer_sha256 -or
                (Get-FileHash -Algorithm SHA256 -LiteralPath ([string]$existing.server_executable)).Hash.ToLowerInvariant() -cne
                    [string]$existing.binary_sha256 -or
                (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path (Join-Path ([string]$existing.release_root) 'bin') 'ninfer_bench.exe')).Hash.ToLowerInvariant() -cne
                    [string]$existing.benchmark_sha256 -or
                (Get-FileHash -Algorithm SHA256 -LiteralPath ([string]$existing.config_file)).Hash.ToLowerInvariant() -cne
                    [string]$existing.config_sha256) {
                throw 'already-installed release files no longer match their recorded identity'
            }
            Assert-AppLocalDlls $existing
            if (-not $NoStart) {
                & $controllerPath -Action Start -StateRoot $StateRoot | Out-Null
            }
            $stageCleanup = Remove-InstallTreeStreaming ([pscustomobject]@{
                    kind = 'package_stage'; path = $stage
                })
            $alreadyInstalledReceipt = [ordered]@{
                artifact_type = 'ninfer_windows_release_install_receipt'
                schema_version = 2
                status = 'already_installed'
                operation_id = $operationId
                release_id = $instanceId
                package_sha256 = $actualPackageSha
                model_rehashed = $false
                lifecycle_pointers_changed = $false
                runtime_start_requested = (-not $NoStart)
                secret_values_recorded = 0
            }
            $transactionReceiptPath = Join-Path $transactionRoot 'install-receipt.json'
            Write-JsonAtomic $transactionReceiptPath $alreadyInstalledReceipt
            Set-ObjectProperty $transaction 'status' 'completed'
            Set-ObjectProperty $transaction 'phase' 'already_installed'
            Set-ObjectProperty $transaction 'final_receipt_path' $transactionReceiptPath
            Write-InstallTransaction $transaction
            Write-Output ($alreadyInstalledReceipt | ConvertTo-Json -Depth 12 -Compress)
            return
        }

        Set-ObjectProperty $transaction 'instance_id' $instanceId

        $sourceModelItem = Get-Item -LiteralPath $sourceModel
        if (($sourceModelItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'pinned model artifact must be a direct external file, not a reparse point'
        }
        if ([Int64]$sourceModelItem.Length -ne [Int64]$spec.model.bytes) {
            throw 'model artifact byte length mismatch'
        }
        $modelIdentity = Get-StreamingSha256 $sourceModel 'pinned model artifact'
        if ([string]$modelIdentity.sha256 -cne [string]$spec.model.sha256) {
            throw 'model artifact SHA-256 mismatch'
        }
        Set-TransactionPhase $transaction 'model_reference_verified'

        New-CandidateDirectories $transaction $releaseRoot $secretDirectory $cacheRoot
        $configRoot = Join-Path $releaseRoot 'config'
        $logsRoot = Join-Path $releaseRoot 'logs'
        $candidateReceiptsRoot = Join-Path $releaseRoot 'receipts'
        New-Item -ItemType Directory -Path $configRoot, $logsRoot, $candidateReceiptsRoot | Out-Null
        Move-Item -LiteralPath (Join-Path $payload 'bin') -Destination (Join-Path $releaseRoot 'bin')
        $lifecycleBin = Join-Path (Join-Path $releaseRoot 'bin') 'lifecycle'
        $qualificationBin = Join-Path (Join-Path $releaseRoot 'bin') 'qualification'
        New-Item -ItemType Directory -Path $lifecycleBin, $qualificationBin | Out-Null
        foreach ($name in @('Control-Release.ps1', 'Control-GpuOwner.ps1', 'Protect-StateRoot.ps1', 'Install-Release.ps1')) {
            $source = Join-Path $payload $name
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                Move-Item -LiteralPath $source -Destination (Join-Path $lifecycleBin $name)
            }
            else {
                throw "release package is missing lifecycle binary: $name"
            }
        }
        $smokeSource = Join-Path $payload 'smoke'
        foreach ($name in @('agent_protocol.py', 'serve_contract.py')) {
            $source = Join-Path $smokeSource $name
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                Move-Item -LiteralPath $source -Destination $qualificationBin
            }
            else {
                throw "release package is missing its qualification smoke asset: $name"
            }
        }
        if (Test-Path -LiteralPath $smokeSource -PathType Container) {
            Remove-Item -LiteralPath $smokeSource -Force
        }
        Move-Item -LiteralPath (Join-Path $payload 'server-config.json') -Destination (Join-Path $configRoot 'server-config.json')
        foreach ($name in @('build-identity.json', 'release-spec.json', 'SHA256SUMS.txt')) {
            Move-Item -LiteralPath (Join-Path $payload $name) -Destination (Join-Path $candidateReceiptsRoot $name)
        }
        foreach ($name in @('README.md', 'RELEASE_NOTES.md', 'LICENSE', 'VERSION')) {
            $source = Join-Path $payload $name
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                Move-Item -LiteralPath $source -Destination (Join-Path $candidateReceiptsRoot $name)
            }
            else {
                throw "release package is missing support file: $name"
            }
        }
        $payloadResidue = @(Get-ChildItem -LiteralPath $payload -Force)
        if ($payloadResidue.Count -ne 0) { throw 'release package contains an unclassified candidate-root entry' }
        Set-TransactionPhase $transaction 'candidate_layout_created'

        $installedKey = Join-Path $secretDirectory 'api-key.txt'
        Copy-Item -LiteralPath $apiKeySource -Destination $installedKey
        Assert-OneLineSecret $installedKey

        $serverExecutable = Join-Path (Join-Path $releaseRoot 'bin') 'ninfer-serve.exe'
        $ninferExecutable = Join-Path (Join-Path $releaseRoot 'bin') 'ninfer.exe'
        $benchmarkExecutable = Join-Path (Join-Path $releaseRoot 'bin') 'ninfer_bench.exe'
        $configFile = Join-Path $configRoot 'server-config.json'
        $controllerSource = Join-Path $lifecycleBin 'Control-Release.ps1'
        $ninferSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $ninferExecutable).Hash.ToLowerInvariant()
        $binarySha = (Get-FileHash -Algorithm SHA256 -LiteralPath $serverExecutable).Hash.ToLowerInvariant()
        $benchmarkSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $benchmarkExecutable).Hash.ToLowerInvariant()
        $configSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $configFile).Hash.ToLowerInvariant()
        if ($ninferSha -cne [string]$buildIdentity.binaries.ninfer -or
            $binarySha -cne [string]$buildIdentity.binaries.'ninfer-serve' -or
            $benchmarkSha -cne [string]$buildIdentity.binaries.ninfer_bench -or
            $configSha -cne [string]$buildIdentity.configuration_sha256) {
            throw 'installed release file identity mismatch'
        }

        $releases = $installedReleases
        $releases[$instanceId] = [ordered]@{
            release_root = $releaseRoot
            server_executable = $serverExecutable
            model_artifact = $sourceModel
            model_reference = 'external-pinned-read-only'
            config_file = $configFile
            cache_root = $cacheRoot
            receipts_root = $candidateReceiptsRoot
            api_key_file = $installedKey
            host = [string]$config.listen.host
            port = [int]$config.listen.port
            deployment_profile = [string]$spec.deployment_profile
            upstream_base_sha = [string]$buildIdentity.upstream_base_sha
            lineage_base_sha = [string]$buildIdentity.lineage_base_sha
            patch_stack_sha = [string]$buildIdentity.patch_stack_sha
            build_profile = [string]$buildIdentity.build_profile
            cuda_architecture = [string]$spec.gpu.cuda_architecture
            ninfer_sha256 = $ninferSha
            binary_sha256 = $binarySha
            benchmark_sha256 = $benchmarkSha
            package_sha256 = $actualPackageSha
            inner_checksums_sha256 = $innerChecksumsSha
            model_artifact_sha256 = [string]$modelIdentity.sha256
            model_bytes = [Int64]$modelIdentity.bytes
            model_creation_utc_ticks = [Int64]$modelIdentity.creation_utc_ticks
            model_last_write_utc_ticks = [Int64]$modelIdentity.last_write_utc_ticks
            config_sha256 = $configSha
            gpu_index = [int]$selectedGpu.index
            gpu_uuid = [string]$selectedGpu.uuid
            gpu_name = [string]$selectedGpu.name
            gpu_pci_bus_id = [string]$selectedGpu.pci_bus_id
            installed_utc = [DateTime]::UtcNow.ToString('o')
            # The controller passes --stop-event only to releases whose record declares it; a
            # release installed before this field existed is stopped by termination.
            managed_stop = [string]$spec.lifecycle.managed_stop
            graceful_stop_timeout_seconds = [int]$spec.lifecycle.graceful_stop_timeout_seconds
        }
        $incumbent = if ($null -eq $oldState) { $null } else { [string]$oldState.active_release }
        $priorPrevious = if ($null -eq $oldState) { $null } else { [string]$oldState.previous_release }
        $state = [ordered]@{
            artifact_type = 'ninfer_windows_lifecycle_state'
            schema_version = 4
            task_name = $taskName
            prepared_release = $instanceId
            active_release = $incumbent
            previous_release = $priorPrevious
            gpu_owner = $ownerRecord
            releases = $releases
        }

        # Release the incumbent with its original controller and hash-bound owner before
        # replacing either. A live outer lease belongs to those exact controller bytes and their
        # policy; the successor's policy judges the owner only once that stop retired it.
        if ($oldTaskWasRunning) {
            Stop-IncumbentForUpgrade $transaction $ownerControllerSource $successorOwnerPolicy $selectedGpu
        }

        Set-TransactionFlag $transaction 'state_helpers_touched'
        if ($null -ne $ownerControllerSource) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ownerControllerPath) | Out-Null
            Set-TransactionFlag $transaction 'owner_controller_touched'
            Copy-Item -LiteralPath $ownerControllerSource -Destination $ownerControllerPath -Force
            $ownerHelperSource = Join-Path (Split-Path -Parent $ownerControllerSource) 'Protect-StateRoot.ps1'
            if (Test-Path -LiteralPath $ownerHelperSource -PathType Leaf) {
                Copy-Item -LiteralPath $ownerHelperSource -Destination `
                    (Join-Path (Split-Path -Parent $ownerControllerPath) 'Protect-StateRoot.ps1') -Force
            }
            $installedOwnerSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $ownerControllerPath).Hash.ToLowerInvariant()
            if ($installedOwnerSha -cne [string]$ownerRecord.controller_sha256) {
                throw 'managed GPU-owner controller SHA-256 mismatch'
            }
        }
        $stateHelperSource = Join-Path $lifecycleBin 'Protect-StateRoot.ps1'
        Copy-Item -LiteralPath $stateHelperSource -Destination $stateHelperPath -Force
        Set-NInferProtectedRootAcl $secretDirectory
        Set-NInferProtectedFileAcl $installedKey
        Assert-NInferProtectedStateTree $StateRoot

        Set-TransactionFlag $transaction 'controller_touched'
        Copy-Item -LiteralPath $controllerSource -Destination $controllerPath -Force

        Set-TransactionFlag $transaction 'state_activated'
        Write-JsonAtomic $statePath $state
        Set-TransactionPhase $transaction 'candidate_prepared'

        $state.prepared_release = $null
        $state.active_release = $instanceId
        $state.previous_release = $incumbent
        Write-JsonAtomic $statePath $state
        Set-TransactionPhase $transaction 'candidate_activated'

        Set-TransactionFlag $transaction 'task_touched'
        Register-ReleaseTask $taskName $controllerPath

        if (-not $NoStart) {
            & $controllerPath -Action Start -StateRoot $StateRoot | Out-Null
        }
        $statusOutput = @(& $controllerPath -Action Status -StateRoot $StateRoot)
        $lifecycleStatus = Convert-CommandOutputToJson $statusOutput 'release lifecycle status'

        $stageCleanup = Remove-InstallTreeStreaming ([pscustomobject]@{
                kind = 'package_stage'
                path = $stage
            })
        $installReceipt = [ordered]@{
            artifact_type = 'ninfer_windows_release_install_receipt'
            schema_version = 2
            status = 'passed'
            operation_id = $operationId
            installed_utc = [DateTime]::UtcNow.ToString('o')
            release_id = $instanceId
            candidate_root_directories = @('bin', 'config', 'logs', 'receipts')
            identity = [ordered]@{
                upstream_base_sha = [string]$buildIdentity.upstream_base_sha
                lineage_base_sha = [string]$buildIdentity.lineage_base_sha
                patch_stack_sha = [string]$buildIdentity.patch_stack_sha
                build_profile = [string]$buildIdentity.build_profile
                cuda_architecture = [string]$spec.gpu.cuda_architecture
                ninfer_sha256 = $ninferSha
                server_sha256 = $binarySha
                benchmark_sha256 = $benchmarkSha
                config_sha256 = $configSha
                model_sha256 = [string]$modelIdentity.sha256
                package_sha256 = $actualPackageSha
                inner_checksums_sha256 = $innerChecksumsSha
            }
            model = [ordered]@{
                reference = 'external-pinned-read-only'
                bytes = [Int64]$modelIdentity.bytes
                sha256 = [string]$modelIdentity.sha256
                copied_into_candidate = $false
            }
            selected_gpu = $selectedGpu
            pointers = [ordered]@{
                prepared_release = $null
                active_release = $instanceId
                previous_release = $incumbent
            }
            lifecycle_status = $lifecycleStatus
            progress_transport = 'flushed-jsonl-console'
            diagnostics_retention = 'bounded-transaction-receipts'
            interrupted_install_reentry = 'repair-required'
            secret_values_recorded = 0
        }
        $candidateInstallReceiptPath = Join-Path $candidateReceiptsRoot 'install.json'
        $transactionInstallReceiptPath = Join-Path $transactionRoot 'install-receipt.json'
        Write-JsonAtomic $candidateInstallReceiptPath $installReceipt
        Write-JsonAtomic $transactionInstallReceiptPath $installReceipt
        Set-ObjectProperty $transaction 'status' 'completed'
        Set-ObjectProperty $transaction 'phase' 'completed'
        Set-ObjectProperty $transaction 'final_receipt_path' $transactionInstallReceiptPath
        Write-InstallTransaction $transaction
        Write-InstallEvent 'install_completed' 'Installer transaction completed' ([ordered]@{
                operation_id = $operationId
                receipt_path = $candidateInstallReceiptPath
            })
        Write-Output ($installReceipt | ConvertTo-Json -Depth 20 -Compress)
    }
    catch {
        $installFailure = $_
        try {
            $rollbackActions = @(Invoke-InstallTransactionRollback $transaction $statePath $controllerPath $ownerControllerPath 'rollback')
            $failureReceipt = [ordered]@{
                artifact_type = 'ninfer_windows_release_install_failure_receipt'
                schema_version = 1
                status = 'rolled_back'
                operation_id = $operationId
                failed_utc = [DateTime]::UtcNow.ToString('o')
                failure = Get-BoundedDiagnostic $installFailure.Exception.Message
                cleanup_actions = @($rollbackActions | ForEach-Object { $_ })
                retry_disposition = 'new-install-allowed-after-complete-rollback'
            }
            $failureReceiptPath = Join-Path $transactionRoot 'failure-receipt.json'
            Write-JsonAtomic $failureReceiptPath $failureReceipt
            Write-InstallEvent 'install_failed' 'Installer rolled back the failed install' ([ordered]@{
                    operation_id = $operationId
                    failure = [string]$failureReceipt.failure
                    receipt_path = $failureReceiptPath
                })
        }
        catch {
            $rollbackFailure = $_
            throw [InvalidOperationException]::new(
                "release install failed and installer repair is required: $(Get-BoundedDiagnostic $rollbackFailure.Exception.Message)",
                $installFailure.Exception
            )
        }
        throw $installFailure
    }
}
finally {
    if ($null -ne $installLock) { $installLock.Dispose() }
}
