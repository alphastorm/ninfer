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

$principal = [Security.Principal.WindowsPrincipal]::new(
    [Security.Principal.WindowsIdentity]::GetCurrent()
)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Install-Release.ps1 must run from an elevated PowerShell session'
}

function Read-JsonFile([string]$Path) {
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
    $root = [IO.Path]::GetFullPath($PayloadRoot).TrimEnd('\') + '\'
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
Assert-OneLineSecret (Resolve-Path -LiteralPath $ApiKeyFile).Path

New-Item -ItemType Directory -Force -Path $StateRoot | Out-Null
$StateRoot = (Resolve-Path -LiteralPath $StateRoot).Path
$stage = Join-Path $StateRoot ('staging-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $stage | Out-Null

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
    if (Test-Path -LiteralPath $releaseRoot) { throw 'release instance is already installed' }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $releaseRoot) | Out-Null
    Move-Item -LiteralPath $payload -Destination $releaseRoot

    $modelDirectory = Join-Path $releaseRoot 'model'
    New-Item -ItemType Directory -Force -Path $modelDirectory | Out-Null
    $installedModel = Join-Path $modelDirectory 'model.ninfer'
    Copy-Item -LiteralPath $sourceModel -Destination $installedModel
    $installedModelSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $installedModel).Hash.ToLowerInvariant()
    if ($installedModelSha -cne $modelSha) { throw 'installed model artifact SHA-256 mismatch' }

    $secretDirectory = Join-Path $StateRoot 'secrets'
    New-Item -ItemType Directory -Force -Path $secretDirectory | Out-Null
    $installedKey = Join-Path $secretDirectory 'api-key.txt'
    Copy-Item -LiteralPath (Resolve-Path -LiteralPath $ApiKeyFile).Path -Destination $installedKey -Force
    Assert-OneLineSecret $installedKey

    $serverExecutable = Join-Path $releaseRoot 'bin\ninfer-serve.exe'
    $configFile = Join-Path $releaseRoot 'server-config.json'
    $binarySha = (Get-FileHash -Algorithm SHA256 -LiteralPath $serverExecutable).Hash.ToLowerInvariant()
    $configSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $configFile).Hash.ToLowerInvariant()
    if ($binarySha -cne [string]$manifest.binary_sha256 -or
        $configSha -cne [string]$manifest.config_sha256) {
        throw 'installed release file identity mismatch'
    }

    $statePath = Join-Path $StateRoot 'state.json'
    $oldState = if (Test-Path -LiteralPath $statePath -PathType Leaf) {
        Read-JsonFile $statePath
    } else {
        $null
    }
    $taskName = [string]$spec.lifecycle.task_name
    if ($taskName -cne 'NInfer-Qwen38-4090-v0.1') { throw 'unexpected lifecycle task identity' }
    if ($null -ne $oldState) {
        $task = Get-ScheduledTask -TaskName ([string]$oldState.task_name) -ErrorAction SilentlyContinue
        if ($null -ne $task -and $task.State -eq 'Running') {
            & (Join-Path $StateRoot 'Control-Release.ps1') -Action Stop -StateRoot $StateRoot | Out-Null
        }
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
    Write-JsonAtomic $statePath $state
    Copy-Item -LiteralPath (Join-Path $releaseRoot 'Control-Release.ps1') `
        -Destination (Join-Path $StateRoot 'Control-Release.ps1') -Force

    & icacls.exe $StateRoot '/inheritance:r' '/grant:r' `
        'SYSTEM:(OI)(CI)F' 'Administrators:(OI)(CI)F' "$env:USERNAME`:(OI)(CI)M" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'failed to restrict release state ACL' }
    & icacls.exe (Join-Path $StateRoot '*') '/reset' '/T' '/C' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'failed to propagate release state ACL' }
    Register-ReleaseTask $taskName (Join-Path $StateRoot 'Control-Release.ps1')

    if (-not $NoStart) {
        & (Join-Path $StateRoot 'Control-Release.ps1') -Action Start -StateRoot $StateRoot | Out-Null
    }
    & (Join-Path $StateRoot 'Control-Release.ps1') -Action Status -StateRoot $StateRoot
}
finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
