[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ServerExecutable,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$PatchStackSha,

    [string[]]$RuntimeFile = @(),

    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ServerConfig = (Join-Path $PSScriptRoot 'server-config.json'),

    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$QualificationRecord = (Join-Path $PSScriptRoot '../../../docs/qualification/qwen3.8-27b-rtx-4090-v0.1.json'),

    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'out')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-JsonFile([string]$Path) {
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Set-JsonProperty([object]$Object, [string]$Name, [object]$Value) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        Add-Member -InputObject $Object -MemberType NoteProperty -Name $Name -Value $Value
    }
    else {
        $property.Value = $Value
    }
}

function ConvertTo-ReleaseTextBytes([string]$Path) {
    $encoding = [Text.UTF8Encoding]::new($false, $true)
    $text = $encoding.GetString([IO.File]::ReadAllBytes($Path))
    if ($text.Length -gt 0 -and $text[0] -eq [char]0xfeff) {
        $text = $text.Substring(1)
    }
    $text = $text.Replace("`r`n", "`n").Replace("`r", "`n").Replace("`n", "`r`n")
    return [Text.UTF8Encoding]::new($false).GetBytes($text)
}

function Get-BytesSha256([byte[]]$Bytes) {
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($sha256.ComputeHash($Bytes)).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
    }
}

$releaseId = 'qwen38-4090-v0.1'
$releaseVersion = '0.1.0'
$assetStem = 'ninfer-4090-qwen38-v0.1.0-win-x64'
$profile = 'qwen38-4090-v0.1'
$upstreamBaseSha = '9ec1b82c7afa021314682d7a95390f8935ead7c2'
$server = (Resolve-Path -LiteralPath $ServerExecutable).Path
$configSource = (Resolve-Path -LiteralPath $ServerConfig).Path
$qualificationSource = (Resolve-Path -LiteralPath $QualificationRecord).Path
$versionOutput = (& $server --version 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "ninfer-serve --version failed with exit code $LASTEXITCODE"
}
foreach ($required in @(
        "upstream_base_sha=$upstreamBaseSha",
        "patch_stack_sha=$PatchStackSha",
        "build_profile=$profile",
        'source_dirty=false'
    )) {
    if ($versionOutput.IndexOf($required, [StringComparison]::Ordinal) -lt 0) {
        throw "ninfer-serve --version is missing required release identity: $required"
    }
}

$specSource = Join-Path $PSScriptRoot 'release-spec.json'
$spec = Read-JsonFile $specSource
$config = Read-JsonFile $configSource
$qualification = Read-JsonFile $qualificationSource
if ($spec.artifact_type -cne 'ninfer_windows_release_spec' -or
    [int]$spec.schema_version -ne 1 -or
    [string]$spec.release_id -cne $releaseId -or
    [string]$spec.release_version -cne $releaseVersion -or
    [string]$spec.platform -cne 'windows-x86_64') {
    throw 'release specification identity mismatch'
}
$expectedCandidateRoots = @('bin', 'config', 'logs', 'receipts')
$candidateRoots = @($spec.lifecycle.candidate_root_directories)
if ($candidateRoots.Count -ne $expectedCandidateRoots.Count) {
    throw 'release specification installer architecture mismatch'
}
for ($index = 0; $index -lt $expectedCandidateRoots.Count; $index++) {
    if ([string]$candidateRoots[$index] -cne $expectedCandidateRoots[$index]) {
        throw 'release specification installer architecture mismatch'
    }
}
if ([Int64]$spec.model.bytes -ne 18210531328 -or
    [string]$spec.lifecycle.model_artifact_ownership -cne 'external-pinned-read-only' -or
    [string]$spec.lifecycle.cache_ownership -cne 'state-root-per-release' -or
    [string]$spec.lifecycle.interrupted_install_reentry -cne 'repair-required') {
    throw 'release specification installer architecture mismatch'
}
if ($config.artifact_type -cne 'ninfer_windows_server_config' -or
    [int]$config.schema_version -ne 1 -or
    [string]$config.release_id -cne $releaseId -or
    [string]$config.deployment_profile -cne $profile) {
    throw 'server configuration identity mismatch'
}
if ([bool]$config.session_checkpoint.enabled -ne $true -or
    [int]$config.session_checkpoint.quota_mib -lt 2048 -or
    [int]$config.session_checkpoint.staging_mib -lt 64 -or
    [int]$config.session_checkpoint.staging_mib -gt [int]$config.session_checkpoint.quota_mib) {
    throw 'server session checkpoint configuration is not release-safe'
}
if ($qualification.artifact_type -cne 'ninfer_public_windows_release_qualification' -or
    [int]$qualification.schema_version -ne 1 -or
    [string]$qualification.source.qualified_commit -cne $PatchStackSha) {
    throw 'qualification record is not bound to the executable source identity'
}
if ([string]$spec.source.qualified_source_head -cne $PatchStackSha) {
    throw 'release specification is not bound to the executable source identity'
}
$serverSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $server).Hash.ToLowerInvariant()
$configBytes = ConvertTo-ReleaseTextBytes $configSource
$configSha256 = Get-BytesSha256 $configBytes
if ([string]$qualification.identity.binary_sha256 -cne $serverSha256 -or
    [string]$qualification.identity.config_sha256 -cne $configSha256 -or
    [string]$qualification.identity.model_artifact_sha256 -cne [string]$spec.model.sha256) {
    throw 'qualification record artifact hashes do not match the release inputs'
}
if ([string]$qualification.status -ceq 'passed') {
    foreach ($gate in @('G', 'L')) {
        $property = $qualification.release_gates.PSObject.Properties[$gate]
        if ($null -eq $property -or $null -eq $property.Value.PSObject.Properties['status'] -or
            [string]$property.Value.status -cne 'passed') {
            throw 'qualification record claims passed while required release gates remain incomplete'
        }
    }
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
$stage = Join-Path $outputRoot ('.stage-' + [Guid]::NewGuid().ToString('N'))
$payload = Join-Path $stage $releaseId
$bin = Join-Path $payload 'bin'
New-Item -ItemType Directory -Force -Path $bin | Out-Null

$zipPath = Join-Path $outputRoot "$assetStem.zip"
$qualificationPath = Join-Path $outputRoot "$assetStem-qualification.json"
$installerAsset = Join-Path $outputRoot 'Install-Release.ps1'
$controllerAsset = Join-Path $outputRoot 'Control-Release.ps1'
$gpuOwnerAsset = Join-Path $outputRoot 'Control-GpuOwner.ps1'
$shaSumsPath = Join-Path $outputRoot 'SHA256SUMS'
foreach ($path in @($zipPath, $qualificationPath, $installerAsset, $controllerAsset, $gpuOwnerAsset, $shaSumsPath)) {
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
}

try {
    Copy-Item -LiteralPath $server -Destination (Join-Path $bin 'ninfer-serve.exe')
    $runtimeNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $runtimeNames.Add('ninfer-serve.exe') | Out-Null
    foreach ($file in $RuntimeFile) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "runtime file does not exist: $file"
        }
        $name = [IO.Path]::GetFileName($file)
        if (-not $runtimeNames.Add($name)) { throw "duplicate runtime filename: $name" }
        Copy-Item -LiteralPath $file -Destination (Join-Path $bin $name)
    }

    foreach ($name in @(
            'release-spec.json',
            'Control-Release.ps1',
            'Control-GpuOwner.ps1',
            'Install-Release.ps1',
            'New-QualificationReceipt.ps1',
            'Invoke-Qualification.ps1',
            'Compare-MtpQualification.ps1'
        )) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $payload $name)
    }
    [IO.File]::WriteAllBytes((Join-Path $payload 'server-config.json'), $configBytes)

    $repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
    $smokeDirectory = Join-Path $payload 'smoke'
    New-Item -ItemType Directory -Force -Path $smokeDirectory | Out-Null
    foreach ($name in @(
            'agent_protocol.py',
            'golden_equivalent.py',
            'golden_equivalent_extension.ts',
            'golden_equivalent_contract.json'
        )) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $smokeDirectory
    }
    foreach ($name in @('serve_contract.py', '__init__.py')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "tools/smoke/$name") -Destination $smokeDirectory
    }

    $binaryPath = Join-Path $bin 'ninfer-serve.exe'
    $configPath = Join-Path $payload 'server-config.json'
    $specPath = Join-Path $payload 'release-spec.json'
    $controllerPath = Join-Path $payload 'Control-Release.ps1'
    $gpuOwnerPath = Join-Path $payload 'Control-GpuOwner.ps1'
    $installerPath = Join-Path $payload 'Install-Release.ps1'
    $manifest = [ordered]@{
        artifact_type = 'ninfer_windows_release_manifest'
        schema_version = 1
        release_id = $releaseId
        release_version = $releaseVersion
        release_instance_id = "$releaseId-$($PatchStackSha.Substring(0, 12))"
        asset_filename = [IO.Path]::GetFileName($zipPath)
        deployment_profile = $profile
        upstream_base_sha = $upstreamBaseSha
        patch_stack_sha = $PatchStackSha
        source_dirty = $false
        binary_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $binaryPath).Hash.ToLowerInvariant()
        config_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $configPath).Hash.ToLowerInvariant()
        release_spec_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $specPath).Hash.ToLowerInvariant()
        installer_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $installerPath).Hash.ToLowerInvariant()
        controller_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $controllerPath).Hash.ToLowerInvariant()
        gpu_owner_controller_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $gpuOwnerPath).Hash.ToLowerInvariant()
        created_utc = [DateTime]::UtcNow.ToString('o')
    }
    $manifestPath = Join-Path $payload 'release-manifest.json'
    [IO.File]::WriteAllText(
        $manifestPath,
        ($manifest | ConvertTo-Json -Depth 8),
        [Text.UTF8Encoding]::new($false)
    )

    foreach ($payloadFile in @(Get-ChildItem -LiteralPath $payload -File -Recurse -Force)) {
        $payloadFile.Attributes = $payloadFile.Attributes -band (-bnot ([IO.FileAttributes]::Hidden -bor [IO.FileAttributes]::System))
    }
    $checksumEntries = @(
        Get-ChildItem -LiteralPath $payload -File -Recurse -Force |
            Sort-Object FullName |
            ForEach-Object {
                [ordered]@{
                    relative_path = $_.FullName.Substring($payload.Length + 1).Replace('\', '/')
                    sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
                    bytes = $_.Length
                }
            }
    )
    $checksums = [ordered]@{
        artifact_type = 'ninfer_package_checksums'
        schema_version = 1
        files = $checksumEntries
    }
    [IO.File]::WriteAllText(
        (Join-Path $payload 'checksums.json'),
        ($checksums | ConvertTo-Json -Depth 8),
        [Text.UTF8Encoding]::new($false)
    )

    Compress-Archive -LiteralPath $payload -DestinationPath $zipPath -CompressionLevel Optimal
    $zipHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash.ToLowerInvariant()
    $runtimeDllCount = @(Get-ChildItem -LiteralPath $bin -File -Filter '*.dll').Count

    Set-JsonProperty $qualification 'release_id' ([string]$manifest.release_instance_id)
    Set-JsonProperty $qualification 'release_version' $releaseVersion
    Set-JsonProperty $qualification 'package_assembled_utc' ([DateTime]::UtcNow.ToString('o'))
    Set-JsonProperty $qualification 'package' ([ordered]@{
            filename = [IO.Path]::GetFileName($zipPath)
            sha256 = $zipHash
            size_bytes = (Get-Item -LiteralPath $zipPath).Length
            manifest_entries = $checksumEntries.Count
            runtime_dlls = $runtimeDllCount
            checksum_verification = 'passed'
            release_manifest_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $manifestPath).Hash.ToLowerInvariant()
            installer_sha256 = [string]$manifest.installer_sha256
            controller_sha256 = [string]$manifest.controller_sha256
        })
    [IO.File]::WriteAllText(
        $qualificationPath,
        ($qualification | ConvertTo-Json -Depth 24),
        [Text.UTF8Encoding]::new($false)
    )

    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Install-Release.ps1') -Destination $installerAsset
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Control-Release.ps1') -Destination $controllerAsset
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Control-GpuOwner.ps1') -Destination $gpuOwnerAsset
    $assets = @($zipPath, $qualificationPath, $installerAsset, $controllerAsset, $gpuOwnerAsset) |
        Sort-Object { [IO.Path]::GetFileName($_) }
    $sumLines = foreach ($asset in $assets) {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $asset).Hash.ToLowerInvariant()
        "$hash  $([IO.Path]::GetFileName($asset))"
    }
    [IO.File]::WriteAllText(
        $shaSumsPath,
        ([string]::Join("`n", $sumLines) + "`n"),
        [Text.UTF8Encoding]::new($false)
    )

    [ordered]@{
        artifact_type = 'ninfer_package_build_receipt'
        schema_version = 1
        release_id = $releaseId
        release_version = $releaseVersion
        patch_stack_sha = $PatchStackSha
        package_sha256 = $zipHash
        package_bytes = (Get-Item -LiteralPath $zipPath).Length
        package_file = [IO.Path]::GetFileName($zipPath)
        qualification_file = [IO.Path]::GetFileName($qualificationPath)
        checksum_file = [IO.Path]::GetFileName($shaSumsPath)
        installer_file = [IO.Path]::GetFileName($installerAsset)
        controller_file = [IO.Path]::GetFileName($controllerAsset)
        gpu_owner_controller_file = [IO.Path]::GetFileName($gpuOwnerAsset)
    } | ConvertTo-Json -Depth 6
}
finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
