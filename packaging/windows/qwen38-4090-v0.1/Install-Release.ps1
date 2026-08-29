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
    [string]$GpuOwnerControllerPath = (Join-Path $PSScriptRoot 'Control-GpuOwner.ps1'),

    [string]$StateRoot = (Join-Path $env:ProgramData 'NInfer\qwen38-4090'),

    [Parameter(ParameterSetName = 'Install')]
    [switch]$NoStart,

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

function Invoke-TrustedNvidiaSmi([string]$Arguments) {
    $systemDirectory = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)
    $application = [IO.Path]::GetFullPath((Join-Path $systemDirectory 'nvidia-smi.exe'))
    if (-not (Test-Path -LiteralPath $application -PathType Leaf)) {
        throw 'trusted System32 nvidia-smi application is missing'
    }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $application
    $start.Arguments = $Arguments
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
            throw "trusted nvidia-smi query exited $($process.ExitCode): $($stderr.Substring(0, [Math]::Min(256, $stderr.Length)))"
        }
        if (-not [string]::IsNullOrWhiteSpace($stderr)) {
            throw "trusted nvidia-smi query wrote stderr: $($stderr.Substring(0, [Math]::Min(256, $stderr.Length)))"
        }
    }
    finally { $process.Dispose() }
    $trimmed = $stdout.TrimEnd([char[]]@(13, 10))
    if ([string]::IsNullOrWhiteSpace($trimmed)) { return @() }
    $rows = @($trimmed -split '[\r\n]+')
    if (@($rows | Where-Object { [string]::IsNullOrWhiteSpace([string]$_) }).Count -ne 0) {
        throw 'trusted nvidia-smi query returned an empty row'
    }
    return $rows
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
    if (Get-TransactionFlag $Transaction 'owner_state_created') {
        $descriptors += [ordered]@{
            field = 'owner_state_root'; kind = 'gpu_owner_state'; root = $StateRoot
        }
    }
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
    $leasePath = Join-Path $StateRoot 'gpu-owner-lease.json'
    if ((Test-Path -LiteralPath $leasePath -PathType Leaf) -and
        -not [string]::IsNullOrWhiteSpace([string]$Transaction.instance_id)) {
        $lease = Read-JsonFile $leasePath
        if ([string]$lease.release_id -ceq [string]$Transaction.instance_id) {
            [pscustomobject]@{ kind = 'gpu_owner_lease'; path = $leasePath }
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
        Remove-Item -LiteralPath $path -Force
        $filesDeleted = 1
    }
    elseif (Test-Path -LiteralPath $path -PathType Container) {
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

function ConvertTo-Schema3Release(
    [string]$ReleaseId,
    [object]$Release,
    [int]$SourceSchemaVersion
) {
    $values = [ordered]@{}
    foreach ($field in @(
            'release_root', 'server_executable', 'model_artifact', 'config_file',
            'api_key_file', 'host', 'port', 'deployment_profile', 'upstream_base_sha',
            'patch_stack_sha', 'binary_sha256', 'config_sha256', 'installed_utc'
        )) {
        $values[$field] = Get-RequiredReleaseField $Release $ReleaseId $field
    }

    $digestField = if ($SourceSchemaVersion -eq 1) { 'model_sha256' } else { 'model_artifact_sha256' }
    $modelDigest = [string](Get-RequiredReleaseField $Release $ReleaseId $digestField)
    if ($modelDigest -cnotmatch '^[0-9a-f]{64}$') {
        throw "installed release '$ReleaseId' has an invalid verified model digest"
    }

    $modelPath = [IO.Path]::GetFullPath([string]$values.model_artifact)
    if (-not (Test-Path -LiteralPath $modelPath -PathType Leaf)) {
        throw "cannot migrate installed release '$ReleaseId': model artifact is missing"
    }
    $modelItem = Get-Item -LiteralPath $modelPath
    if ($SourceSchemaVersion -ge 2) {
        $modelBytes = [Int64](Get-RequiredReleaseField $Release $ReleaseId 'model_bytes')
        $modelCreationUtcTicks = [Int64](Get-RequiredReleaseField $Release $ReleaseId 'model_creation_utc_ticks')
        $modelLastWriteUtcTicks = [Int64](Get-RequiredReleaseField $Release $ReleaseId 'model_last_write_utc_ticks')
        if ([Int64]$modelItem.Length -ne $modelBytes -or
            [Int64]$modelItem.CreationTimeUtc.Ticks -ne $modelCreationUtcTicks -or
            [Int64]$modelItem.LastWriteTimeUtc.Ticks -ne $modelLastWriteUtcTicks) {
            throw "cannot migrate installed release '$ReleaseId': model artifact changed after install"
        }
    }
    else {
        $modelBytes = [Int64]$modelItem.Length
        $modelCreationUtcTicks = [Int64]$modelItem.CreationTimeUtc.Ticks
        $modelLastWriteUtcTicks = [Int64]$modelItem.LastWriteTimeUtc.Ticks
    }

    $releaseRoot = [IO.Path]::GetFullPath([string]$values.release_root)
    if ($SourceSchemaVersion -eq 3) {
        $modelReference = [string](Get-RequiredReleaseField $Release $ReleaseId 'model_reference')
        $cacheRoot = [string](Get-RequiredReleaseField $Release $ReleaseId 'cache_root')
        $receiptsRoot = [string](Get-RequiredReleaseField $Release $ReleaseId 'receipts_root')
    }
    else {
        if (Test-PathWithinRoot $modelPath $releaseRoot) {
            $modelReference = 'legacy-managed-copy'
            $cacheRoot = Join-Path $releaseRoot 'cache'
            $receiptsRoot = $releaseRoot
        }
        else {
            $modelReference = 'external-pinned-read-only'
            $cacheRoot = Join-Path (Join-Path $StateRoot 'cache') $ReleaseId
            $receiptsRoot = Join-Path $releaseRoot 'receipts'
        }
    }
    if ($modelReference -cnotin @('external-pinned-read-only', 'legacy-managed-copy')) {
        throw "installed release '$ReleaseId' has an unsupported model reference kind"
    }
    if ($modelReference -ceq 'external-pinned-read-only' -and
        (Test-PathWithinRoot $modelPath $releaseRoot)) {
        throw "installed release '$ReleaseId' stores an external model reference inside its candidate root"
    }

    if ($SourceSchemaVersion -eq 3) {
        $gpuIndex = [int](Get-RequiredReleaseField $Release $ReleaseId 'gpu_index')
        $gpuUuid = [string](Get-RequiredReleaseField $Release $ReleaseId 'gpu_uuid')
        $gpuName = [string](Get-RequiredReleaseField $Release $ReleaseId 'gpu_name')
    }
    else { throw "cannot migrate installed release '$ReleaseId': selected GPU identity is missing" }

    return [ordered]@{
        release_root = $releaseRoot
        server_executable = [string]$values.server_executable
        model_artifact = $modelPath
        model_reference = $modelReference
        config_file = [string]$values.config_file
        cache_root = [string]$cacheRoot
        receipts_root = [string]$receiptsRoot
        api_key_file = [string]$values.api_key_file
        host = [string]$values.host
        port = [int]$values.port
        deployment_profile = [string]$values.deployment_profile
        upstream_base_sha = [string]$values.upstream_base_sha
        patch_stack_sha = [string]$values.patch_stack_sha
        binary_sha256 = [string]$values.binary_sha256
        model_artifact_sha256 = $modelDigest
        model_bytes = $modelBytes
        model_creation_utc_ticks = $modelCreationUtcTicks
        model_last_write_utc_ticks = $modelLastWriteUtcTicks
        config_sha256 = [string]$values.config_sha256
        gpu_index = $gpuIndex
        gpu_uuid = $gpuUuid
        gpu_name = $gpuName
        installed_utc = [string]$values.installed_utc
    }
}

function ConvertTo-Schema3Releases([object]$State) {
    if ($State.artifact_type -cne 'ninfer_windows_lifecycle_state' -or
        [int]$State.schema_version -notin @(1, 2, 3)) {
        throw 'installed release lifecycle state envelope mismatch'
    }
    $releasesProperty = $State.PSObject.Properties['releases']
    if ($null -eq $releasesProperty -or $null -eq $releasesProperty.Value) {
        throw 'installed release lifecycle state has no release records'
    }

    $converted = [ordered]@{}
    foreach ($property in $releasesProperty.Value.PSObject.Properties) {
        if ([string]::IsNullOrWhiteSpace([string]$property.Name)) {
            throw 'installed release lifecycle state has an invalid release identity'
        }
        $converted[$property.Name] = ConvertTo-Schema3Release $property.Name $property.Value ([int]$State.schema_version)
    }
    $activeRelease = [string](Get-RequiredReleaseField $State 'lifecycle state' 'active_release')
    if (-not $converted.Contains($activeRelease)) {
        throw 'installed lifecycle active release record is missing'
    }
    return $converted
}

function Assert-HostPrerequisites([object]$Spec, [object]$Config) {
    $deviceText = [string]$Config.engine.device
    if ($deviceText -notmatch '^(?:cuda:)?([0-9]+)$') { throw 'configured GPU device is invalid' }
    $deviceIndex = [int]$Matches[1]
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or -not [Environment]::Is64BitOperatingSystem -or [IntPtr]::Size -ne 8) {
        throw 'this release requires 64-bit Windows on x86-64'
    }
    if (Test-Path Env:CUDA_VISIBLE_DEVICES) { throw 'CUDA_VISIBLE_DEVICES must be absent for bound GPU ordinal identity' }
    $rows = @(Invoke-TrustedNvidiaSmi '--query-gpu=index,uuid,name,compute_cap,driver_version --format=csv,noheader,nounits')
    if ($rows.Count -eq 0) { throw 'NVIDIA GPU prerequisite query returned no rows' }
    foreach ($row in $rows) {
        $parts = @(([string]$row -split ',') | ForEach-Object { $_.Trim() })
        [uint32]$rowIndex = 0
        if ($parts.Count -ne 5 -or @($parts | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -ne 0 -or
            -not [uint32]::TryParse($parts[0], [Globalization.NumberStyles]::None,
                [Globalization.CultureInfo]::InvariantCulture, [ref]$rowIndex)) {
            throw 'trusted nvidia-smi prerequisite query returned a malformed row'
        }
        if ($rowIndex -ne [uint32]$deviceIndex) { continue }
        if ($parts[2] -cne [string]$Spec.gpu.name -or $parts[3] -cne [string]$Spec.gpu.compute_capability) { throw 'configured GPU device is not the qualified RTX 4090' }
        if ([Version]$parts[4] -lt [Version]("$([int]$Spec.gpu.minimum_driver_major).0")) { throw 'NVIDIA driver is too old' }
        return [ordered]@{ index = [int]$parts[0]; uuid = [string]$parts[1]; name = [string]$parts[2] }
    }
    throw 'configured GPU ordinal is unavailable'
}

function Assert-PayloadChecksums([string]$PayloadRoot) {
    $checksums = Read-JsonFile (Join-Path $PayloadRoot 'checksums.json')
    if ($checksums.artifact_type -cne 'ninfer_package_checksums' -or
        [int]$checksums.schema_version -ne 1) {
        throw 'package checksum manifest envelope mismatch'
    }
    $directorySeparators = [char[]]@(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar
    )
    $root = [IO.Path]::GetFullPath($PayloadRoot).TrimEnd($directorySeparators) +
        [IO.Path]::DirectorySeparatorChar
    $listed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $checksums.files) {
        $relative = ([string]$entry.relative_path).Replace('/', [IO.Path]::DirectorySeparatorChar)
        if ([string]::IsNullOrWhiteSpace($relative) -or
            -not $listed.Add($relative) -or
            [string]::Equals($relative, 'checksums.json', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'package checksum manifest contains an invalid or duplicate path'
        }
        $candidate = [IO.Path]::GetFullPath((Join-Path $PayloadRoot $relative))
        if (-not $candidate.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'package checksum path escapes its payload root'
        }
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            throw "package file is missing: $relative"
        }
        $item = Get-Item -LiteralPath $candidate
        if ([Int64]$item.Length -ne [Int64]$entry.bytes) {
            throw "package file size mismatch: $relative"
        }
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $candidate).Hash.ToLowerInvariant()
        if ($actual -cne [string]$entry.sha256) {
            throw "package file SHA-256 mismatch: $relative"
        }
    }
    foreach ($file in Get-ChildItem -LiteralPath $PayloadRoot -File -Recurse -Force) {
        $relative = $file.FullName.Substring($root.Length)
        if ([string]::Equals($relative, 'checksums.json', [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        if (-not $listed.Contains($relative)) { throw "package contains an unverified file: $relative" }
    }
}

function Register-ReleaseTask([string]$TaskName, [string]$ControllerPath) {
    $quotedController = '"' + $ControllerPath.Replace('"', '""') + '"'
    $quotedState = '"' + $StateRoot.Replace('"', '""') + '"'
    $arguments = "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $quotedController -Action Run -StateRoot $quotedState"
    $taskAction = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arguments
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -RestartCount 3 -RestartInterval ([TimeSpan]::FromMinutes(1)) -StartWhenAvailable
    $taskPrincipal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $TaskName -Action $taskAction -Description 'Owns only the isolated NInfer Qwen3.8 RTX 4090 release process; explicit start only.' -Settings $settings -Principal $taskPrincipal -Force | Out-Null
}

function Assert-InstallerArchitectureContract([object]$Spec) {
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
    if ([Int64]$Spec.model.bytes -ne 18210531328) {
        throw 'release specification pinned model byte length mismatch'
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

function Convert-CommandOutputToJson([object[]]$Output, [string]$Label) {
    $lines = @($Output | ForEach-Object { [string]$_ })
    if ($lines.Count -eq 0) { throw "$Label returned no JSON" }
    return ([string]::Join([Environment]::NewLine, $lines) | ConvertFrom-Json)
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

    if ((Get-TransactionFlag $Transaction 'state_activated') -and
        (Test-Path -LiteralPath $ControllerPath -PathType Leaf) -and
        (Test-Path -LiteralPath $StatePath -PathType Leaf)) {
        try {
            $activeState = Read-JsonFile $StatePath
            if ([string]$activeState.active_release -ceq [string]$Transaction.instance_id) {
                & $ControllerPath -Action Stop -StateRoot $StateRoot | Out-Null
            }
        }
        catch { $failures.Add("candidate stop: $(Get-BoundedDiagnostic $_.Exception.Message)") }
    }
    if (Get-TransactionFlag $Transaction 'state_activated') {
        try { Restore-PersistedFileSnapshot $StatePath $Transaction.snapshots.state }
        catch { $failures.Add("state restore: $(Get-BoundedDiagnostic $_.Exception.Message)") }
    }
    if (Get-TransactionFlag $Transaction 'controller_touched') {
        try { Restore-PersistedFileSnapshot $ControllerPath $Transaction.snapshots.controller }
        catch { $failures.Add("controller restore: $(Get-BoundedDiagnostic $_.Exception.Message)") }
        try { Restore-PersistedFileSnapshot $StateProtectionPath $Transaction.snapshots.state_protection }
        catch { $failures.Add("state-protection helper restore: $(Get-BoundedDiagnostic $_.Exception.Message)") }
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
        try { Restore-PersistedFileSnapshot $OwnerProtectionPath $Transaction.snapshots.owner_protection }
        catch { $failures.Add("GPU-owner protection helper restore: $(Get-BoundedDiagnostic $_.Exception.Message)") }
    }

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
Assert-NInferProtectedStateTree $StateRoot
$statePath = Join-Path $StateRoot 'state.json'
$controllerPath = Join-Path $StateRoot 'Control-Release.ps1'
$ownerControllerPath = Join-Path (Join-Path $StateRoot 'gpu-owner') 'Control-GpuOwner.ps1'
$ownerStateRoot = Join-Path $StateRoot 'gpu-owner-state'
$stateProtectionPath = Join-Path $StateRoot 'Protect-StateRoot.ps1'
$ownerProtectionPath = Join-Path (Split-Path -Parent $ownerControllerPath) 'Protect-StateRoot.ps1'
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
    if ([IO.Path]::GetFileName($package) -cne 'ninfer-4090-qwen38-v0.1.0-win-x64.zip') {
        throw 'unexpected release package filename'
    }
    Write-InstallEvent 'package_identity_started' 'Verifying release package identity' ([ordered]@{ path = $package })
    $actualPackageSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $package).Hash.ToLowerInvariant()
    if ($actualPackageSha -cne $PackageSha256) { throw 'release package SHA-256 mismatch' }
    Write-InstallEvent 'package_identity_completed' 'Verified release package identity' ([ordered]@{ sha256 = $actualPackageSha })
    $apiKeySource = (Resolve-Path -LiteralPath $ApiKeyFile).Path
    Assert-OneLineSecret $apiKeySource

    $oldState = if (Test-Path -LiteralPath $statePath -PathType Leaf) { Read-JsonFile $statePath } else { $null }
    $migratedReleases = if ($null -eq $oldState) { [ordered]@{} } else { ConvertTo-Schema3Releases $oldState }
    $ownerControllerSource = $null
    $ownerRecord = $null
    $ownerStateCreated = $false
    if (-not [string]::IsNullOrWhiteSpace($GpuOwnerControllerPath)) {
        if (-not (Test-Path -LiteralPath $GpuOwnerControllerPath -PathType Leaf)) {
            throw 'GPU-owner controller does not exist'
        }
        $ownerControllerSource = (Resolve-Path -LiteralPath $GpuOwnerControllerPath).Path
        $ownerCommand = Get-Command -Name $ownerControllerSource -CommandType ExternalScript -ErrorAction Stop
        if ($null -eq $ownerCommand.Parameters['StateRoot']) {
            throw 'GPU-owner controller must accept the protected StateRoot parameter'
        }
        $ownerStateExisted = Test-Path -LiteralPath $ownerStateRoot
        try {
            $ownerStatusOutput = @(& $ownerControllerSource -Action status -StateRoot $ownerStateRoot)
        }
        catch {
            if (-not $ownerStateExisted -and (Test-Path -LiteralPath $ownerStateRoot)) {
                Assert-NInferProtectedStateTree $ownerStateRoot
                Remove-Item -LiteralPath $ownerStateRoot -Recurse -Force
            }
            throw
        }
        $ownerStateCreated = -not $ownerStateExisted
        $ownerStatus = Convert-CommandOutputToJson $ownerStatusOutput 'GPU-owner controller status'
        if ($null -eq $ownerStatus.PSObject.Properties['paused'] -or
            $ownerStatus.paused -isnot [bool]) {
            throw 'GPU-owner controller status must expose a paused boolean'
        }
        $ownerRecord = [ordered]@{
            controller_path = $ownerControllerPath
            controller_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $ownerControllerSource).Hash.ToLowerInvariant()
            state_root = $ownerStateRoot
        }
    }
    elseif ($null -ne $oldState -and $null -ne $oldState.PSObject.Properties['gpu_owner']) {
        $ownerRecord = [ordered]@{
            controller_path = [string]$oldState.gpu_owner.controller_path
            controller_sha256 = [string]$oldState.gpu_owner.controller_sha256
            state_root = $ownerStateRoot
        }
    }
    if ($null -eq $ownerRecord) {
        throw 'a GPU-owner controller is required for the first install; upgrades inherit the managed adapter'
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
        stage_path = $stage
        instance_id = $null
        release_root = $null
        secret_directory = $null
        cache_root = $null
        owner_state_root = $ownerStateRoot
        task_name = $null
        task_existed = $false
        task_was_running = $false
        task_xml_path = $null
        reentry_policy = 'repair-required-before-install'
        snapshots = [pscustomobject][ordered]@{
            state = Save-PersistedFileSnapshot $statePath $snapshotRoot 'state'
            controller = Save-PersistedFileSnapshot $controllerPath $snapshotRoot 'controller'
            state_protection = Save-PersistedFileSnapshot $stateProtectionPath $snapshotRoot 'state-protection'
            owner_controller = Save-PersistedFileSnapshot $ownerControllerPath $snapshotRoot 'owner-controller'
            owner_protection = Save-PersistedFileSnapshot $ownerProtectionPath $snapshotRoot 'owner-protection'
        }
        flags = [pscustomobject][ordered]@{
            incumbent_touched = $false
            owner_controller_touched = $false
            owner_state_created = $ownerStateCreated
            controller_touched = $false
            state_activated = $false
            task_touched = $false
        }
    }
    Write-InstallTransaction $transaction
    New-Item -ItemType Directory -Path $stage | Out-Null
    Write-InstallEvent 'install_started' 'Started durable installer transaction' ([ordered]@{
            operation_id = $operationId
            journal_path = $journalPath
        })
    try {
        Write-InstallEvent 'package_expand_started' 'Expanding verified release package' ([ordered]@{ stage_path = $stage })
        Expand-Archive -LiteralPath $package -DestinationPath $stage
        Write-InstallEvent 'package_expand_completed' 'Expanded verified release package' ([ordered]@{ stage_path = $stage })
        $payloadDirectories = @(Get-ChildItem -LiteralPath $stage -Directory)
        if ($payloadDirectories.Count -ne 1) { throw 'package must contain exactly one release root' }
        $payload = $payloadDirectories[0].FullName
        Assert-PayloadChecksums $payload

        $manifest = Read-JsonFile (Join-Path $payload 'release-manifest.json')
        $spec = Read-JsonFile (Join-Path $payload 'release-spec.json')
        $config = Read-JsonFile (Join-Path $payload 'server-config.json')
        $selectedGpu = Assert-HostPrerequisites $spec $config
        Assert-InstallerArchitectureContract $spec
        if ($manifest.artifact_type -cne 'ninfer_windows_release_manifest' -or
            [int]$manifest.schema_version -ne 1 -or
            $manifest.release_id -cne $spec.release_id -or
            $config.release_id -cne $spec.release_id -or
            $manifest.deployment_profile -cne $config.deployment_profile -or
            $manifest.source_dirty -ne $false) {
            throw 'release package identity mismatch'
        }
        if ([string]$manifest.release_version -cne '0.1.0' -or
            [string]$manifest.asset_filename -cne 'ninfer-4090-qwen38-v0.1.0-win-x64.zip') {
            throw 'release package semantic version mismatch'
        }
        if ([string]$manifest.upstream_base_sha -cne [string]$spec.source.upstream_base_sha) {
            throw 'release package upstream identity mismatch'
        }

        $taskName = [string]$spec.lifecycle.task_name
        if ($taskName -cne 'NInfer-Qwen38-4090-v0.1') { throw 'unexpected lifecycle task identity' }
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
        Set-ObjectProperty $transaction 'task_name' $taskName
        Set-ObjectProperty $transaction 'task_existed' $oldTaskExisted
        Set-ObjectProperty $transaction 'task_was_running' $oldTaskWasRunning
        Set-ObjectProperty $transaction 'task_xml_path' $taskXmlPath
        Set-TransactionPhase $transaction 'package_verified'

        $sourceModel = (Resolve-Path -LiteralPath $ModelArtifactPath).Path
        Assert-ExternalModelReference $sourceModel $StateRoot
        $sourceModelItem = Get-Item -LiteralPath $sourceModel
        if ([Int64]$sourceModelItem.Length -ne [Int64]$spec.model.bytes) {
            throw 'model artifact byte length mismatch'
        }
        $modelIdentity = Get-StreamingSha256 $sourceModel 'pinned model artifact'
        if ([string]$modelIdentity.sha256 -cne [string]$spec.model.sha256) {
            throw 'model artifact SHA-256 mismatch'
        }
        Set-TransactionPhase $transaction 'model_reference_verified'

        $instanceId = [string]$manifest.release_instance_id
        if ([string]::IsNullOrWhiteSpace($instanceId) -or $instanceId -notmatch '^[A-Za-z0-9._-]+$') {
            throw 'release instance identity is invalid'
        }
        $releaseRoot = Join-Path (Join-Path $StateRoot 'releases') $instanceId
        $secretDirectory = Join-Path (Join-Path $StateRoot 'secrets') $instanceId
        $cacheRoot = Join-Path (Join-Path $StateRoot 'cache') $instanceId
        if (Test-Path -LiteralPath $releaseRoot) { throw 'release instance is already installed' }
        if (Test-Path -LiteralPath $secretDirectory) { throw 'release secret instance is already installed' }
        if (Test-Path -LiteralPath $cacheRoot) { throw 'release cache instance is already installed' }
        Set-ObjectProperty $transaction 'instance_id' $instanceId
        Set-ObjectProperty $transaction 'release_root' $releaseRoot
        Set-ObjectProperty $transaction 'secret_directory' $secretDirectory
        Set-ObjectProperty $transaction 'cache_root' $cacheRoot

        New-Item -ItemType Directory -Force -Path $releaseRoot | Out-Null
        $configRoot = Join-Path $releaseRoot 'config'
        $logsRoot = Join-Path $releaseRoot 'logs'
        $candidateReceiptsRoot = Join-Path $releaseRoot 'receipts'
        New-Item -ItemType Directory -Path $configRoot, $logsRoot, $candidateReceiptsRoot | Out-Null
        Move-Item -LiteralPath (Join-Path $payload 'bin') -Destination (Join-Path $releaseRoot 'bin')
        $lifecycleBin = Join-Path (Join-Path $releaseRoot 'bin') 'lifecycle'
        $qualificationBin = Join-Path (Join-Path $releaseRoot 'bin') 'qualification'
        New-Item -ItemType Directory -Path $lifecycleBin, $qualificationBin | Out-Null
        foreach ($name in @(
                'Control-Release.ps1', 'Control-GpuOwner.ps1', 'Protect-StateRoot.ps1',
                'Install-Release.ps1'
            )) {
            $source = Join-Path $payload $name
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                Move-Item -LiteralPath $source -Destination (Join-Path $lifecycleBin $name)
            }
            else {
                throw "release package is missing lifecycle binary: $name"
            }
        }
        foreach ($name in @('New-QualificationReceipt.ps1', 'Invoke-Qualification.ps1', 'Compare-MtpQualification.ps1')) {
            $source = Join-Path $payload $name
            if (Test-Path -LiteralPath $source -PathType Leaf) {
                Move-Item -LiteralPath $source -Destination (Join-Path $qualificationBin $name)
            }
            else {
                throw "release package is missing qualification binary: $name"
            }
        }
        $smokeSource = Join-Path $payload 'smoke'
        if (Test-Path -LiteralPath $smokeSource -PathType Container) {
            Move-Item -LiteralPath $smokeSource -Destination (Join-Path $qualificationBin 'smoke')
        }
        else {
            throw 'release package is missing qualification smoke binaries'
        }
        Move-Item -LiteralPath (Join-Path $payload 'server-config.json') -Destination (Join-Path $configRoot 'server-config.json')
        foreach ($name in @('release-manifest.json', 'release-spec.json', 'checksums.json')) {
            Move-Item -LiteralPath (Join-Path $payload $name) -Destination (Join-Path $candidateReceiptsRoot $name)
        }
        $payloadResidue = @(Get-ChildItem -LiteralPath $payload -Force)
        if ($payloadResidue.Count -ne 0) { throw 'release package contains an unclassified candidate-root entry' }
        New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
        Set-TransactionPhase $transaction 'candidate_layout_created'

        New-Item -ItemType Directory -Path $secretDirectory | Out-Null
        $installedKey = Join-Path $secretDirectory 'api-key.txt'
        Copy-Item -LiteralPath $apiKeySource -Destination $installedKey
        Assert-OneLineSecret $installedKey

        $serverExecutable = Join-Path (Join-Path $releaseRoot 'bin') 'ninfer-serve.exe'
        $configFile = Join-Path $configRoot 'server-config.json'
        $controllerSource = Join-Path $lifecycleBin 'Control-Release.ps1'
        $binarySha = (Get-FileHash -Algorithm SHA256 -LiteralPath $serverExecutable).Hash.ToLowerInvariant()
        $configSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $configFile).Hash.ToLowerInvariant()
        if ($binarySha -cne [string]$manifest.binary_sha256 -or
            $configSha -cne [string]$manifest.config_sha256) {
            throw 'installed release file identity mismatch'
        }

        $releases = $migratedReleases
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
            deployment_profile = [string]$manifest.deployment_profile
            upstream_base_sha = [string]$manifest.upstream_base_sha
            patch_stack_sha = [string]$manifest.patch_stack_sha
            binary_sha256 = $binarySha
            model_artifact_sha256 = [string]$modelIdentity.sha256
            model_bytes = [Int64]$modelIdentity.bytes
            model_creation_utc_ticks = [Int64]$modelIdentity.creation_utc_ticks
            model_last_write_utc_ticks = [Int64]$modelIdentity.last_write_utc_ticks
            config_sha256 = $configSha
            gpu_index = [int]$selectedGpu.index
            gpu_uuid = [string]$selectedGpu.uuid
            gpu_name = [string]$selectedGpu.name
            installed_utc = [DateTime]::UtcNow.ToString('o')
        }
        $previous = if ($null -eq $oldState) { $null } else { [string]$oldState.active_release }
        $state = [ordered]@{
            artifact_type = 'ninfer_windows_lifecycle_state'
            schema_version = 3
            task_name = $taskName
            active_release = $instanceId
            previous_release = $previous
            gpu_owner = $ownerRecord
            releases = $releases
        }

        if ($oldTaskWasRunning) {
            Set-TransactionFlag $transaction 'incumbent_touched'
            & $controllerPath -Action Stop -StateRoot $StateRoot | Out-Null
        }
        if ($null -ne $ownerControllerSource) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ownerControllerPath) | Out-Null
            Set-TransactionFlag $transaction 'owner_controller_touched'
            Copy-Item -LiteralPath $ownerControllerSource -Destination $ownerControllerPath -Force
            $ownerHelperSource = Join-Path (Split-Path -Parent $ownerControllerSource) 'Protect-StateRoot.ps1'
            if (Test-Path -LiteralPath $ownerHelperSource -PathType Leaf) {
                Copy-Item -LiteralPath $ownerHelperSource -Destination $ownerProtectionPath -Force
            }
            $installedOwnerSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $ownerControllerPath).Hash.ToLowerInvariant()
            if ($installedOwnerSha -cne [string]$ownerRecord.controller_sha256) {
                throw 'managed GPU-owner controller SHA-256 mismatch'
            }
        }
        elseif ($null -ne $ownerRecord) {
            if (-not [string]::Equals(
                    [IO.Path]::GetFullPath([string]$ownerRecord.controller_path),
                    [IO.Path]::GetFullPath($ownerControllerPath),
                    [StringComparison]::OrdinalIgnoreCase) -or
                -not (Test-Path -LiteralPath $ownerControllerPath -PathType Leaf) -or
                (Get-FileHash -Algorithm SHA256 -LiteralPath $ownerControllerPath).Hash.ToLowerInvariant() -cne
                    [string]$ownerRecord.controller_sha256) {
                throw 'installed GPU-owner controller identity mismatch'
            }
        }
        Set-TransactionFlag $transaction 'controller_touched'
        $stateHelperSource = Join-Path $lifecycleBin 'Protect-StateRoot.ps1'
        Copy-Item -LiteralPath $stateHelperSource -Destination $stateProtectionPath -Force
        Assert-NInferProtectedStateTree $StateRoot

        Copy-Item -LiteralPath $controllerSource -Destination $controllerPath -Force

        Set-TransactionFlag $transaction 'state_activated'
        Write-JsonAtomic $statePath $state

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
            schema_version = 1
            status = 'passed'
            operation_id = $operationId
            installed_utc = [DateTime]::UtcNow.ToString('o')
            release_id = $instanceId
            candidate_root = $releaseRoot
            candidate_root_directories = @('bin', 'config', 'logs', 'receipts')
            model = [ordered]@{
                path = $sourceModel
                reference = 'external-pinned-read-only'
                bytes = [Int64]$modelIdentity.bytes
                sha256 = [string]$modelIdentity.sha256
                copied_into_candidate = $false
            }
            cache_root = $cacheRoot
            selected_gpu = $selectedGpu
            lifecycle_status = $lifecycleStatus
            progress_transport = 'flushed-jsonl-console'
            diagnostics_retention = 'bounded-transaction-receipts'
            interrupted_install_reentry = 'repair-required'
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
