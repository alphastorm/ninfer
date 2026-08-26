[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ServerExecutable,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$PatchStackSha,

    [string[]]$RuntimeFile = @(),

    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'out')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$releaseId = 'qwen38-4090-v0.1'
$profile = 'sf-qwen38-4090-v0.1'
$upstreamBaseSha = '9ec1b82c7afa021314682d7a95390f8935ead7c2'
$server = (Resolve-Path -LiteralPath $ServerExecutable).Path
$versionOutput = (& $server --version 2>&1 | Out-String).Trim()
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

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
$stage = Join-Path $outputRoot ('.stage-' + [Guid]::NewGuid().ToString('N'))
$payload = Join-Path $stage $releaseId
$bin = Join-Path $payload 'bin'
New-Item -ItemType Directory -Force -Path $bin | Out-Null

try {
    Copy-Item -LiteralPath $server -Destination (Join-Path $bin 'ninfer-serve.exe')
    foreach ($file in $RuntimeFile) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "runtime file does not exist: $file"
        }
        Copy-Item -LiteralPath $file -Destination (Join-Path $bin ([IO.Path]::GetFileName($file)))
    }

    foreach ($name in @(
            'release-spec.json',
            'server-config.json',
            'Control-Release.ps1',
            'Install-Release.ps1',
            'Invoke-Qualification.ps1'
        )) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $payload $name)
    }

    $repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
    $smokeDirectory = Join-Path $payload 'smoke'
    New-Item -ItemType Directory -Force -Path $smokeDirectory | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'agent_protocol.py') -Destination $smokeDirectory
    foreach ($name in @('serve_contract.py', '__init__.py')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "tools/smoke/$name") -Destination $smokeDirectory
    }

    $binaryPath = Join-Path $bin 'ninfer-serve.exe'
    $configPath = Join-Path $payload 'server-config.json'
    $specPath = Join-Path $payload 'release-spec.json'
    $manifest = [ordered]@{
        artifact_type = 'ninfer_windows_release_manifest'
        schema_version = 1
        release_id = $releaseId
        release_instance_id = "$releaseId-$($PatchStackSha.Substring(0, 12))"
        deployment_profile = $profile
        upstream_base_sha = $upstreamBaseSha
        patch_stack_sha = $PatchStackSha
        source_dirty = $false
        binary_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $binaryPath).Hash.ToLowerInvariant()
        config_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $configPath).Hash.ToLowerInvariant()
        release_spec_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $specPath).Hash.ToLowerInvariant()
        created_utc = [DateTime]::UtcNow.ToString('o')
    }
    $manifestPath = Join-Path $payload 'release-manifest.json'
    [IO.File]::WriteAllText(
        $manifestPath,
        ($manifest | ConvertTo-Json -Depth 8),
        [Text.UTF8Encoding]::new($false)
    )

    $checksums = [ordered]@{
        artifact_type = 'ninfer_package_checksums'
        schema_version = 1
        files = @(
            Get-ChildItem -LiteralPath $payload -File -Recurse |
                Sort-Object FullName |
                ForEach-Object {
                    [ordered]@{
                        relative_path = $_.FullName.Substring($payload.Length + 1).Replace('\', '/')
                        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
                        bytes = $_.Length
                    }
                }
        )
    }
    [IO.File]::WriteAllText(
        (Join-Path $payload 'checksums.json'),
        ($checksums | ConvertTo-Json -Depth 8),
        [Text.UTF8Encoding]::new($false)
    )

    $zipPath = Join-Path $outputRoot ("ninfer-$releaseId-windows-x86_64.zip")
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    Compress-Archive -LiteralPath $payload -DestinationPath $zipPath -CompressionLevel Optimal
    $zipHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash.ToLowerInvariant()
    $sidecarPath = "$zipPath.sha256"
    [IO.File]::WriteAllText(
        $sidecarPath,
        "$zipHash  $([IO.Path]::GetFileName($zipPath))`n",
        [Text.UTF8Encoding]::new($false)
    )
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Install-Release.ps1') -Destination $outputRoot -Force

    [ordered]@{
        artifact_type = 'ninfer_package_build_receipt'
        schema_version = 1
        release_id = $releaseId
        patch_stack_sha = $PatchStackSha
        package_sha256 = $zipHash
        package_bytes = (Get-Item -LiteralPath $zipPath).Length
        package_file = [IO.Path]::GetFileName($zipPath)
        checksum_file = [IO.Path]::GetFileName($sidecarPath)
    } | ConvertTo-Json -Depth 6
}
finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
