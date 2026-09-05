[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$SourceRoot,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$NInferExecutable,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ServerExecutable,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$BenchmarkExecutable,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$ReleaseHeadSha,

    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$RuntimeSourceSha = $ReleaseHeadSha,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string[]]$RuntimeFile,

    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$ServerConfig = (Join-Path $PSScriptRoot 'server-config.json'),

    [string]$PythonExecutable = 'python3',

    [Nullable[Int64]]$SourceDateEpoch,

    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'out')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$releaseId = 'qwen38-3090-omp-v0.2.4-beta.1'
$releaseVersion = 'v0.2.4-beta.1'
$deploymentProfile = 'qwen38-3090-omp-v0.2.4-beta.1-c1'
$buildProfile = 'omp-v0.2.4-rtx3090'
$platform = 'windows-x86_64-cuda13.3-rtx3090'
$upstreamBaseSha = 'ef6ecc3c139b43fc4d3e1b92df474305e8429544'
$lineageBaseSha = 'c467349e375d6aa76afca63c0042bbc0869549aa'
$assetStem = "ninfer-rtx3090-omp-$releaseVersion-$platform"

function Read-JsonFile([string]$Path) {
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Get-LowerSha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Get-TarCommand {
    $command = Get-Command tar.exe -ErrorAction SilentlyContinue
    if ($null -eq $command) { $command = Get-Command tar -ErrorAction SilentlyContinue }
    if ($null -eq $command) { throw 'the operating-system tar command is required' }
    return $command.Source
}

function Assert-AllowedListenHost([object]$Spec, [object]$Config) {
    $expected = @('loopback', 'tailscale-ipv4')
    foreach ($classes in @(@($Spec.network.allowed_listen_host_classes), @($Config.listen.allowed_host_classes))) {
        if ($classes.Count -ne $expected.Count -or
            [string]::Join(',', @($classes)) -cne [string]::Join(',', $expected)) {
            throw 'server configuration must remain loopback/Tailscale-only'
        }
    }
    if ([string]$Spec.network.authentication -cne 'required-api-key-file' -or
        [string]$Config.authentication.mode -cne 'required-api-key-file') {
        throw 'server configuration must require API-key-file authentication'
    }
    $address = $null
    if (-not [Net.IPAddress]::TryParse([string]$Config.listen.host, [ref]$address) -or
        $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
        throw 'server listen host must be an IPv4 loopback or Tailscale address'
    }
    $bytes = $address.GetAddressBytes()
    if ($bytes[0] -ne 127 -and
        -not ($bytes[0] -eq 100 -and $bytes[1] -ge 64 -and $bytes[1] -le 127)) {
        throw 'server listen host must be an IPv4 loopback or Tailscale address'
    }
}

$source = (Resolve-Path -LiteralPath $SourceRoot).Path
$ninfer = (Resolve-Path -LiteralPath $NInferExecutable).Path
$server = (Resolve-Path -LiteralPath $ServerExecutable).Path
$benchmark = (Resolve-Path -LiteralPath $BenchmarkExecutable).Path
$configPath = (Resolve-Path -LiteralPath $ServerConfig).Path
$packageTool = Join-Path $source 'tools/release/package.py'
if (-not (Test-Path -LiteralPath $packageTool -PathType Leaf)) {
    throw 'source tree does not contain tools/release/package.py'
}

$spec = Read-JsonFile (Join-Path $PSScriptRoot 'release-spec.json')
$config = Read-JsonFile $configPath
if ($spec.artifact_type -cne 'ninfer_windows_release_spec' -or
    [int]$spec.schema_version -ne 2 -or
    [string]$spec.release_id -cne $releaseId -or
    [string]$spec.release_version -cne $releaseVersion.Substring(1) -or
    [string]$spec.deployment_profile -cne $deploymentProfile -or
    [string]$spec.build_profile -cne $buildProfile -or
    [string]$spec.platform -cne $platform -or
    [string]$spec.source.upstream_base_sha -cne $upstreamBaseSha -or
    [string]$spec.source.lineage_base_sha -cne $lineageBaseSha -or
    [string]$spec.gpu.cuda_architecture -cne 'sm_86' -or
    [string]$spec.gpu.cmake_cuda_architecture -cne '86' -or
    [string]$spec.model.revision -cne '18dfc887423fa5aabf3cb56fac41490e462b3fab' -or
    [Int64]$spec.model.bytes -ne 18210531328 -or
    [string]$spec.model.sha256 -cne 'eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e') {
    throw 'release specification immutable identity mismatch'
}
if ($config.artifact_type -cne 'ninfer_windows_server_config' -or
    [int]$config.schema_version -ne 2 -or
    [string]$config.release_id -cne $releaseId -or
    [string]$config.deployment_profile -cne $deploymentProfile -or
    [int]$config.engine.max_context -ne 131072 -or
    [string]$config.engine.kv_capacity -cne 'auto' -or
    [int]$config.engine.prefill_chunk -ne 1024 -or
    [string]$config.engine.kv_dtype -cne 'int8' -or
    [int]$config.engine.max_concurrency -ne 1 -or
    [string]$config.speculative.backend -cne 'mtp' -or
    [int]$config.speculative.draft_tokens -ne 3 -or
    -not [bool]$config.session_checkpoint.enabled -or
    [int]$config.session_checkpoint.quota_mib -ne 65536 -or
    [int]$config.session_checkpoint.staging_mib -ne 256 -or
    $null -ne $config.PSObject.Properties['persistent_cache']) {
    throw 'server configuration is not the immutable authenticated C1 profile'
}
Assert-AllowedListenHost $spec $config
$expectedConfigSha256 = Get-LowerSha256 $configPath

$runtimePaths = [Collections.Generic.List[string]]::new()
$runtimeNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($file in $RuntimeFile) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "runtime DLL does not exist: $file" }
    $resolved = (Resolve-Path -LiteralPath $file).Path
    $name = [IO.Path]::GetFileName($resolved)
    if ([IO.Path]::GetExtension($name) -ine '.dll' -or -not $runtimeNames.Add($name)) {
        throw "runtime dependency is not a unique app-local DLL: $name"
    }
    $runtimePaths.Add($resolved)
}
if ($runtimePaths.Count -eq 0) { throw 'at least one app-local runtime DLL is required' }

$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "package output directory already exists: $output" }
$outputParent = Split-Path -Parent $output
New-Item -ItemType Directory -Force -Path $outputParent | Out-Null
$stage = Join-Path $outputParent ('.ninfer-windows-package-' + [Guid]::NewGuid().ToString('N'))
$packageOutput = Join-Path $stage 'published'
$inspection = Join-Path $stage 'inspection'
New-Item -ItemType Directory -Path $stage, $inspection | Out-Null

try {
    $arguments = [Collections.Generic.List[string]]::new()
    foreach ($argument in @(
            $packageTool,
            '--source', $source,
            '--ninfer', $ninfer,
            '--ninfer-serve', $server,
            '--ninfer-bench', $benchmark,
            '--output-dir', $packageOutput,
            '--release-version', $releaseVersion,
            '--platform', $platform,
            '--upstream-base-sha', $upstreamBaseSha,
            '--release-head-sha', $ReleaseHeadSha,
            '--runtime-source-sha', $RuntimeSourceSha,
            '--build-profile', $buildProfile,
            '--lineage-base-sha', $lineageBaseSha,
            '--windows-server-config', $configPath
        )) {
        $arguments.Add([string]$argument)
    }
    if ($null -ne $SourceDateEpoch) {
        $arguments.Add('--source-date-epoch')
        $arguments.Add([string]$SourceDateEpoch)
    }
    foreach ($runtime in $runtimePaths) {
        $arguments.Add('--runtime-dependency')
        $arguments.Add($runtime)
    }

    $packageOutputLines = @(& $PythonExecutable @arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "package.py failed: $([string]::Join([Environment]::NewLine, @($packageOutputLines | ForEach-Object { [string]$_ })))"
    }
    $packageReceipt = [string]::Join([Environment]::NewLine, @($packageOutputLines | ForEach-Object { [string]$_ })) | ConvertFrom-Json
    if ($packageReceipt.artifact_type -cne 'ninfer_local_release_receipt' -or
        [int]$packageReceipt.schema_version -ne 2 -or
        [string]$packageReceipt.release_version -cne $releaseVersion -or
        [string]$packageReceipt.platform -cne $platform -or
        [string]$packageReceipt.upstream_base_sha -cne $upstreamBaseSha -or
        [string]$packageReceipt.lineage_base_sha -cne $lineageBaseSha -or
        [string]$packageReceipt.patch_stack_sha -cne $RuntimeSourceSha -or
        [string]$packageReceipt.package_source_sha -cne $ReleaseHeadSha -or
        [string]$packageReceipt.build_profile -cne $buildProfile) {
        throw 'package.py receipt identity mismatch'
    }

    $packagePath = Join-Path $packageOutput ([string]$packageReceipt.binary_asset.name)
    if ([IO.Path]::GetFileName($packagePath) -cne "$assetStem.tar.gz" -or
        (Get-LowerSha256 $packagePath) -cne [string]$packageReceipt.binary_asset.sha256) {
        throw 'package.py binary asset identity mismatch'
    }
    $tar = Get-TarCommand
    & $tar '-xzf' $packagePath '-C' $inspection
    if ($LASTEXITCODE -ne 0) { throw 'failed to inspect generated package' }
    $roots = @(Get-ChildItem -LiteralPath $inspection -Directory)
    if ($roots.Count -ne 1 -or $roots[0].Name -cne $assetStem) {
        throw 'generated package root identity mismatch'
    }
    $payload = $roots[0].FullName
    $identity = Read-JsonFile (Join-Path $payload 'build-identity.json')
    $packagedSpec = Read-JsonFile (Join-Path $payload 'release-spec.json')
    $packagedConfig = Read-JsonFile (Join-Path $payload 'server-config.json')
    if ([string]$identity.patch_stack_sha -cne $RuntimeSourceSha -or
        [string]$identity.lineage_base_sha -cne $lineageBaseSha -or
        [string]$identity.build_profile -cne $buildProfile -or
        [string]$identity.cuda_architecture -cne '86' -or
        [string]$identity.configuration_sha256 -cne $expectedConfigSha256 -or
        (Get-LowerSha256 (Join-Path $payload 'server-config.json')) -cne $expectedConfigSha256 -or
        [string]$packagedSpec.release_id -cne $releaseId -or
        [string]$packagedConfig.deployment_profile -cne $deploymentProfile) {
        throw 'generated package contents are not bound to the release identity'
    }
    foreach ($name in @('ninfer.exe', 'ninfer-serve.exe', 'ninfer_bench.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path (Join-Path $payload 'bin') $name) -PathType Leaf)) {
            throw "generated package omitted binary: $name"
        }
    }
    foreach ($runtimeName in $runtimeNames) {
        if (-not (Test-Path -LiteralPath (Join-Path (Join-Path $payload 'bin') $runtimeName) -PathType Leaf)) {
            throw "generated package omitted app-local DLL: $runtimeName"
        }
    }
    foreach ($name in @('Install-Release.ps1', 'Control-Release.ps1', 'Control-GpuOwner.ps1', 'Protect-StateRoot.ps1', 'New-QualificationReceipt.ps1', 'SHA256SUMS.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $payload $name) -PathType Leaf)) {
            throw "generated package omitted lifecycle asset: $name"
        }
    }
    foreach ($name in @('agent_protocol.py', 'serve_contract.py')) {
        if (-not (Test-Path -LiteralPath (Join-Path (Join-Path $payload 'smoke') $name) -PathType Leaf)) {
            throw "generated package omitted smoke asset: $name"
        }
    }

    foreach ($name in @('Install-Release.ps1', 'Control-Release.ps1', 'Control-GpuOwner.ps1', 'Protect-StateRoot.ps1')) {
        Copy-Item -LiteralPath (Join-Path $payload $name) -Destination (Join-Path $packageOutput $name)
    }

    $checksumsPath = Join-Path $packageOutput 'SHA256SUMS'
    $lf = [string][char]10
    $buildReceipt = [ordered]@{
        artifact_type = 'ninfer_windows_package_build_receipt'
        schema_version = 2
        release_id = $releaseId
        release_version = $releaseVersion.Substring(1)
        deployment_profile = $deploymentProfile
        build_profile = $buildProfile
        upstream_base_sha = $upstreamBaseSha
        lineage_base_sha = $lineageBaseSha
        patch_stack_sha = $RuntimeSourceSha
        runtime_source_sha = $RuntimeSourceSha
        package_source_sha = $ReleaseHeadSha
        cuda_architecture = 'sm_86'
        binaries = [ordered]@{
            ninfer_sha256 = [string]$identity.binaries.ninfer
            server_sha256 = [string]$identity.binaries.'ninfer-serve'
            benchmark_sha256 = [string]$identity.binaries.ninfer_bench
        }
        config_sha256 = [string]$identity.configuration_sha256
        model_sha256 = [string]$spec.model.sha256
        package = [ordered]@{
            filename = [IO.Path]::GetFileName($packagePath)
            sha256 = Get-LowerSha256 $packagePath
            bytes = [Int64](Get-Item -LiteralPath $packagePath).Length
        }
        checksums = [ordered]@{
            filename = 'SHA256SUMS'
            role = 'closed-outer-distribution-set'
            entries = 9
            line_ending = 'LF'
        }
        support_assets = [ordered]@{
            installer_sha256 = Get-LowerSha256 (Join-Path $packageOutput 'Install-Release.ps1')
            controller_sha256 = Get-LowerSha256 (Join-Path $packageOutput 'Control-Release.ps1')
            gpu_owner_controller_sha256 = Get-LowerSha256 (Join-Path $packageOutput 'Control-GpuOwner.ps1')
            state_protection_sha256 = Get-LowerSha256 (Join-Path $packageOutput 'Protect-StateRoot.ps1')
            qualification_constructor_sha256 = Get-LowerSha256 (Join-Path $payload 'New-QualificationReceipt.ps1')
        }
        qualification_status = 'hardware-pending'
        qualification_status_authority = 'immutable-pre-hardware-package-build'
        later_external_beta_authority = [ordered]@{
            artifact_type = 'ninfer_rtx3090_beta_qualification'
            schema_version = 3
            public_receipt_path = 'docs/qualification/receipts/qwen3.8-27b-rtx-3090-v0.2.4-beta.1.json'
            supersedes_only_when_exact_package_sha256_matches = $true
            mutates_this_build_receipt = $false
        }
        secret_values_recorded = 0
    }
    [IO.File]::WriteAllText(
        (Join-Path $packageOutput 'package-build-receipt.json'),
        (($buildReceipt | ConvertTo-Json -Depth 12) + $lf),
        [Text.UTF8Encoding]::new($false)
    )

    $expectedChecksumNames = @(
        'Control-GpuOwner.ps1',
        'Control-Release.ps1',
        'Install-Release.ps1',
        'Protect-StateRoot.ps1',
        [IO.Path]::GetFileName($packagePath),
        [string]$packageReceipt.source_archive.name,
        [string]$packageReceipt.sbom.name,
        [string]$packageReceipt.checksums,
        'package-build-receipt.json'
    )
    [Array]::Sort($expectedChecksumNames, [StringComparer]::Ordinal)
    $checksumNames = @(
        Get-ChildItem -LiteralPath $packageOutput -File |
            ForEach-Object { $_.Name }
    )
    [Array]::Sort($checksumNames, [StringComparer]::Ordinal)
    if ([string]::Join($lf, $checksumNames) -cne
        [string]::Join($lf, $expectedChecksumNames)) {
        throw 'outer checksum asset set is incomplete, duplicated, or contains an unclassified file'
    }
    $checksumLines = foreach ($name in $checksumNames) {
        "$(Get-LowerSha256 (Join-Path $packageOutput $name))  $name"
    }
    [IO.File]::WriteAllText(
        $checksumsPath,
        ([string]::Join($lf, $checksumLines) + $lf),
        [Text.Encoding]::ASCII
    )

    Move-Item -LiteralPath $packageOutput -Destination $output
    Write-Output ($buildReceipt | ConvertTo-Json -Depth 12 -Compress)
}
finally {
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
