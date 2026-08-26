[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$PackagePath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{64}$')]
    [string]$PackageSha256,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ModelArtifactPath,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ApiKeyFile,

    [string]$StateRoot = (Join-Path $env:ProgramData 'NInfer\qwen38-4090'),

    [switch]$NoStart
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:InstallTestMode = [string]$env:NINFER_INSTALL_TEST_MODE -ceq 'transaction'
if ($script:InstallTestMode) {
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd(
        [IO.Path]::DirectorySeparatorChar
    ) + [IO.Path]::DirectorySeparatorChar
    $testStateRoot = [IO.Path]::GetFullPath($StateRoot).TrimEnd(
        [IO.Path]::DirectorySeparatorChar
    ) + [IO.Path]::DirectorySeparatorChar
    if (-not $testStateRoot.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'transaction test state must remain under the operating-system temporary directory'
    }
}
else {
    $principal = [Security.Principal.WindowsPrincipal]::new(
        [Security.Principal.WindowsIdentity]::GetCurrent()
    )
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Install-Release.ps1 must run from an elevated PowerShell session'
    }
}

function Read-JsonFile([string]$Path) {
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Write-JsonAtomic([string]$Path, [object]$Value) {
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText(
            $temporary,
            ($Value | ConvertTo-Json -Depth 12),
            [Text.UTF8Encoding]::new($false)
        )
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    }
    finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

function Get-FileSnapshot([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        return [pscustomobject]@{
            exists = $true
            bytes = [IO.File]::ReadAllBytes($Path)
        }
    }
    return [pscustomobject]@{ exists = $false; bytes = $null }
}

function Restore-FileSnapshot([string]$Path, [object]$Snapshot) {
    if (-not [bool]$Snapshot.exists) {
        Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
        return
    }
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).restore"
    try {
        [IO.File]::WriteAllBytes($temporary, [byte[]]$Snapshot.bytes)
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    }
    finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-InstallFault([string]$Point) {
    if ($script:InstallTestMode -and
        [string]$env:NINFER_TEST_INSTALL_FAILURE_AFTER -ceq $Point) {
        throw "injected install failure after $Point"
    }
}

function Restore-ReleaseTask([string]$TaskName, [bool]$Existed, [string]$Xml) {
    if ($Existed) {
        Register-ScheduledTask -TaskName $TaskName -Xml $Xml -Force | Out-Null
        return
    }
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
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
    foreach ($entry in $checksums.files) {
        $relative = [string]$entry.relative_path
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
}

function Register-ReleaseTask([string]$TaskName, [string]$ControllerPath) {
    $quotedController = '"' + $ControllerPath.Replace('"', '""') + '"'
    $quotedState = '"' + $StateRoot.Replace('"', '""') + '"'
    $arguments = "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $quotedController -Action Run -StateRoot $quotedState"
    $taskAction = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arguments
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -RestartCount 3 `
        -RestartInterval ([TimeSpan]::FromMinutes(1)) -StartWhenAvailable
    $taskPrincipal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount `
        -RunLevel Highest
    Register-ScheduledTask -TaskName $TaskName -Action $taskAction -Trigger $trigger `
        -Settings $settings -Principal $taskPrincipal -Force | Out-Null
}

$package = (Resolve-Path -LiteralPath $PackagePath).Path
$actualPackageSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $package).Hash.ToLowerInvariant()
if ($actualPackageSha -cne $PackageSha256) { throw 'release package SHA-256 mismatch' }
$apiKeySource = (Resolve-Path -LiteralPath $ApiKeyFile).Path
Assert-OneLineSecret $apiKeySource

New-Item -ItemType Directory -Force -Path $StateRoot | Out-Null
$StateRoot = (Resolve-Path -LiteralPath $StateRoot).Path
$statePath = Join-Path $StateRoot 'state.json'
$controllerPath = Join-Path $StateRoot 'Control-Release.ps1'
$stateSnapshot = Get-FileSnapshot $statePath
$controllerSnapshot = Get-FileSnapshot $controllerPath
$oldState = if ([bool]$stateSnapshot.exists) { Read-JsonFile $statePath } else { $null }
$stage = Join-Path $StateRoot ('staging-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$instanceId = $null
$releaseRoot = $null
$secretDirectory = $null
$taskName = $null
$oldTaskExisted = $false
$oldTaskWasRunning = $false
$oldTaskXml = $null
$candidateMoved = $false
$candidateSecretCreated = $false
$incumbentTouched = $false
$stateActivated = $false
$controllerTouched = $false
$taskTouched = $false

try {
    Expand-Archive -LiteralPath $package -DestinationPath $stage
    $payloadDirectories = @(Get-ChildItem -LiteralPath $stage -Directory)
    if ($payloadDirectories.Count -ne 1) { throw 'package must contain exactly one release root' }
    $payload = $payloadDirectories[0].FullName
    Assert-PayloadChecksums $payload

    $manifest = Read-JsonFile (Join-Path $payload 'release-manifest.json')
    $spec = Read-JsonFile (Join-Path $payload 'release-spec.json')
    $config = Read-JsonFile (Join-Path $payload 'server-config.json')
    if ($manifest.artifact_type -cne 'ninfer_windows_release_manifest' -or
        [int]$manifest.schema_version -ne 1 -or
        $manifest.release_id -cne $spec.release_id -or
        $config.release_id -cne $spec.release_id -or
        $manifest.deployment_profile -cne $config.deployment_profile -or
        $manifest.source_dirty -ne $false) {
        throw 'release package identity mismatch'
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
    if ($oldTaskExisted) {
        $oldTaskWasRunning = [string]$oldTask.State -ceq 'Running'
        $oldTaskXml = [string](Export-ScheduledTask -TaskName $taskName)
    }

    $sourceModel = (Resolve-Path -LiteralPath $ModelArtifactPath).Path
    $sourceModelItem = Get-Item -LiteralPath $sourceModel
    if ([Int64]$sourceModelItem.Length -ne [Int64]$spec.model.bytes) {
        throw 'model artifact byte length mismatch'
    }
    $modelSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourceModel).Hash.ToLowerInvariant()
    if ($modelSha -cne [string]$spec.model.sha256) { throw 'model artifact SHA-256 mismatch' }

    $instanceId = [string]$manifest.release_instance_id
    if ([string]::IsNullOrWhiteSpace($instanceId) -or $instanceId -notmatch '^[A-Za-z0-9._-]+$') {
        throw 'release instance identity is invalid'
    }
    $releaseRoot = Join-Path (Join-Path $StateRoot 'releases') $instanceId
    $secretDirectory = Join-Path (Join-Path $StateRoot 'secrets') $instanceId
    if (Test-Path -LiteralPath $releaseRoot) { throw 'release instance is already installed' }
    if (Test-Path -LiteralPath $secretDirectory) { throw 'release secret instance is already installed' }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $releaseRoot) | Out-Null
    Move-Item -LiteralPath $payload -Destination $releaseRoot
    $candidateMoved = $true
    Invoke-InstallFault 'release_move'

    $modelDirectory = Join-Path $releaseRoot 'model'
    New-Item -ItemType Directory -Force -Path $modelDirectory | Out-Null
    $installedModel = Join-Path $modelDirectory 'model.ninfer'
    Copy-Item -LiteralPath $sourceModel -Destination $installedModel
    $installedModelSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $installedModel).Hash.ToLowerInvariant()
    if ($installedModelSha -cne $modelSha) { throw 'installed model artifact SHA-256 mismatch' }
    Invoke-InstallFault 'model_copy'

    New-Item -ItemType Directory -Path $secretDirectory | Out-Null
    $candidateSecretCreated = $true
    $installedKey = Join-Path $secretDirectory 'api-key.txt'
    Copy-Item -LiteralPath $apiKeySource -Destination $installedKey
    Assert-OneLineSecret $installedKey
    Invoke-InstallFault 'secret_copy'

    $serverExecutable = Join-Path (Join-Path $releaseRoot 'bin') 'ninfer-serve.exe'
    $configFile = Join-Path $releaseRoot 'server-config.json'
    $binarySha = (Get-FileHash -Algorithm SHA256 -LiteralPath $serverExecutable).Hash.ToLowerInvariant()
    $configSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $configFile).Hash.ToLowerInvariant()
    if ($binarySha -cne [string]$manifest.binary_sha256 -or
        $configSha -cne [string]$manifest.config_sha256) {
        throw 'installed release file identity mismatch'
    }

    $releases = [ordered]@{}
    if ($null -ne $oldState) {
        foreach ($property in $oldState.releases.PSObject.Properties) {
            $releases[$property.Name] = $property.Value
        }
    }
    $releases[$instanceId] = [ordered]@{
        release_root = $releaseRoot
        server_executable = $serverExecutable
        model_artifact = $installedModel
        config_file = $configFile
        api_key_file = $installedKey
        host = [string]$config.listen.host
        port = [int]$config.listen.port
        deployment_profile = [string]$manifest.deployment_profile
        upstream_base_sha = [string]$manifest.upstream_base_sha
        patch_stack_sha = [string]$manifest.patch_stack_sha
        binary_sha256 = $binarySha
        model_sha256 = $modelSha
        config_sha256 = $configSha
        installed_utc = [DateTime]::UtcNow.ToString('o')
    }
    $previous = if ($null -eq $oldState) { $null } else { [string]$oldState.active_release }
    $state = [ordered]@{
        artifact_type = 'ninfer_windows_lifecycle_state'
        schema_version = 1
        task_name = $taskName
        active_release = $instanceId
        previous_release = $previous
        releases = $releases
    }

    if ($oldTaskWasRunning) {
        $incumbentTouched = $true
        & $controllerPath -Action Stop -StateRoot $StateRoot | Out-Null
    }
    Invoke-InstallFault 'incumbent_stop'

    Write-JsonAtomic $statePath $state
    $stateActivated = $true
    Invoke-InstallFault 'state_activation'

    $controllerTouched = $true
    Copy-Item -LiteralPath (Join-Path $releaseRoot 'Control-Release.ps1') -Destination $controllerPath -Force
    Invoke-InstallFault 'controller_copy'

    & icacls.exe $StateRoot '/inheritance:r' '/grant:r' 'SYSTEM:(OI)(CI)F' 'Administrators:(OI)(CI)F' ([string]::Concat($env:USERNAME, ':(OI)(CI)M')) | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'failed to restrict release state ACL' }
    & icacls.exe (Join-Path $StateRoot '*') '/reset' '/T' '/C' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'failed to propagate release state ACL' }
    Invoke-InstallFault 'acl'

    $taskTouched = $true
    Register-ReleaseTask $taskName $controllerPath
    Invoke-InstallFault 'task_registration'

    if (-not $NoStart) {
        & $controllerPath -Action Start -StateRoot $StateRoot | Out-Null
        Invoke-InstallFault 'candidate_start'
    }
    & $controllerPath -Action Status -StateRoot $StateRoot
}
catch {
    $installFailure = $_
    $rollbackFailures = [Collections.Generic.List[string]]::new()

    if ($stateActivated -and (Test-Path -LiteralPath $controllerPath -PathType Leaf)) {
        try {
            $activeState = Read-JsonFile $statePath
            if ([string]$activeState.active_release -ceq [string]$instanceId) {
                & $controllerPath -Action Stop -StateRoot $StateRoot | Out-Null
            }
        }
        catch { $rollbackFailures.Add("candidate stop: $($_.Exception.Message)") }
    }
    if ($stateActivated) {
        try { Restore-FileSnapshot $statePath $stateSnapshot }
        catch { $rollbackFailures.Add("state restore: $($_.Exception.Message)") }
    }
    if ($controllerTouched) {
        try { Restore-FileSnapshot $controllerPath $controllerSnapshot }
        catch { $rollbackFailures.Add("controller restore: $($_.Exception.Message)") }
    }
    if ($taskTouched) {
        try { Restore-ReleaseTask $taskName $oldTaskExisted $oldTaskXml }
        catch { $rollbackFailures.Add("task restore: $($_.Exception.Message)") }
    }
    if ($candidateMoved -and $null -ne $releaseRoot) {
        try { Remove-Item -LiteralPath $releaseRoot -Recurse -Force }
        catch { $rollbackFailures.Add("candidate payload cleanup: $($_.Exception.Message)") }
    }
    if ($candidateSecretCreated -and $null -ne $secretDirectory) {
        try { Remove-Item -LiteralPath $secretDirectory -Recurse -Force }
        catch { $rollbackFailures.Add("candidate secret cleanup: $($_.Exception.Message)") }
    }
    if ($incumbentTouched) {
        try { & $controllerPath -Action Start -StateRoot $StateRoot | Out-Null }
        catch { $rollbackFailures.Add("incumbent restart: $($_.Exception.Message)") }
    }

    if ($rollbackFailures.Count -ne 0) {
        $details = [string]::Join('; ', $rollbackFailures)
        throw [InvalidOperationException]::new(
            "release install failed and rollback was incomplete: $details",
            $installFailure.Exception
        )
    }
    throw $installFailure
}
finally {
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
