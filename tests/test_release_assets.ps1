[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageBuilderPath,

    [Parameter(Mandatory = $true)]
    [string]$ServerConfigPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PackageBuilderPath = (Resolve-Path -LiteralPath $PackageBuilderPath).Path
$ServerConfigPath = (Resolve-Path -LiteralPath $ServerConfigPath).Path

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Write-Json([string]$Path, [object]$Value) {
    [IO.File]::WriteAllText(
        $Path,
        ($Value | ConvertTo-Json -Depth 16),
        [Text.UTF8Encoding]::new($false)
    )
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('ninfer-release-assets-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$patchSha = '028030476c96c71f783b6d8b1abb90684e0b8066'
$assetStem = 'ninfer-4090-qwen38-v0.1.0-win-x64'

try {
    $windowsFixture = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
    $serverName = if ($windowsFixture) { 'fixture-server.cmd' } else { 'fixture-server.sh' }
    $server = Join-Path $root $serverName
    $version = if ($windowsFixture) {
        @(
            '@echo off',
            'echo upstream_base_sha=9ec1b82c7afa021314682d7a95390f8935ead7c2',
            "echo patch_stack_sha=$patchSha",
            'echo build_profile=qwen38-4090-v0.1',
            'echo source_dirty=false'
        )
    }
    else {
        @(
            '#!/bin/sh',
            'echo upstream_base_sha=9ec1b82c7afa021314682d7a95390f8935ead7c2',
            "echo patch_stack_sha=$patchSha",
            'echo build_profile=qwen38-4090-v0.1',
            'echo source_dirty=false'
        )
    }
    [IO.File]::WriteAllLines($server, $version, [Text.UTF8Encoding]::new($false))
    if (-not $windowsFixture) {
        & chmod '+x' $server
        if ($LASTEXITCODE -ne 0) { throw 'failed to make the server fixture executable' }
    }
    $runtimeA = Join-Path $root 'runtime-a.dll'
    $runtimeB = Join-Path $root 'runtime-b.dll'
    [IO.File]::WriteAllText($runtimeA, 'runtime-a', [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($runtimeB, 'runtime-b', [Text.UTF8Encoding]::new($false))
    if ($windowsFixture) { (Get-Item -LiteralPath $runtimeB).Attributes = [IO.FileAttributes]::Hidden }
    $serverSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $server).Hash.ToLowerInvariant()
    $canonicalConfigSha256 = 'ec5e4cdb167ac26fc7cc762f9e3d188b965c4e2a20ecfe3cbb1968f084e872db'
    $configSha256 = $canonicalConfigSha256
    $specPath = Join-Path (Split-Path -Parent $PackageBuilderPath) 'release-spec.json'
    $releaseSpec = Get-Content -LiteralPath $specPath -Raw | ConvertFrom-Json
    $modelSha256 = [string]$releaseSpec.model.sha256
    Assert-True ([int64]$releaseSpec.model.bytes -eq 18210531328) 'release spec lost the pinned immutable model size'
    Assert-True ([string]$releaseSpec.lifecycle.model_artifact_ownership -ceq 'external-pinned-read-only') 'release spec permits candidate-owned model copies'
    Assert-True ([string]$releaseSpec.lifecycle.cache_ownership -ceq 'state-root-per-release') 'release spec permits candidate-owned caches'
    Assert-True ([string]$releaseSpec.lifecycle.interrupted_install_reentry -ceq 'repair-required') 'release spec permits qualification retry after an interrupted install'
    Assert-True ([string]::Join(',', @($releaseSpec.lifecycle.candidate_root_directories)) -ceq 'bin,config,logs,receipts') 'release spec candidate root contract changed'
    Assert-True ([string]$releaseSpec.golden_equivalent.contract_id -ceq 'qwen38-4090-omp-golden-equivalent-v1') 'release spec lost the source-controlled Golden-equivalent contract'
    Assert-True ($releaseSpec.golden_equivalent.historical_private_corpus_reused -eq $false) 'release spec claims reuse of the unavailable historical private corpus'
    $sourceConfigSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $ServerConfigPath).Hash.ToLowerInvariant()
    $encoding = [Text.UTF8Encoding]::new($false, $true)
    $configText = $encoding.GetString([IO.File]::ReadAllBytes($ServerConfigPath))
    if ($configText.Length -gt 0 -and $configText[0] -eq [char]0xfeff) {
        $configText = $configText.Substring(1)
    }
    $lfText = $configText.Replace("`r`n", "`n").Replace("`r", "`n")
    $crlfText = $lfText.Replace("`n", "`r`n")
    $lfConfigPath = Join-Path $root 'server-config-lf.json'
    $crlfConfigPath = Join-Path $root 'server-config-crlf.json'
    [IO.File]::WriteAllText($lfConfigPath, $lfText, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($crlfConfigPath, $crlfText, [Text.UTF8Encoding]::new($false))
    Assert-True ((Get-FileHash -Algorithm SHA256 -LiteralPath $crlfConfigPath).Hash.ToLowerInvariant() -ceq $canonicalConfigSha256) 'current JSON does not canonicalize to the qualified CRLF hash'

    $qualificationPath = Join-Path $root 'qualification.json'
    Write-Json $qualificationPath ([ordered]@{
            artifact_type = 'ninfer_public_windows_release_qualification'
            schema_version = 1
            status = 'candidate_ready'
            release_eligible = $false
            source = [ordered]@{ qualified_commit = $patchSha }
            identity = [ordered]@{
                binary_sha256 = $serverSha256
                config_sha256 = $configSha256
                model_artifact_sha256 = $modelSha256
            }
            release_gates = [ordered]@{
                G = [ordered]@{ status = 'not_run' }
                L = [ordered]@{ status = 'not_run' }
                R = [ordered]@{ status = 'not_run' }
            }
            authority = [ordered]@{
                role = 'external-final-qualification-authority'
                supersedes_package_candidate_status = $false
                superseded_status = 'candidate_ready'
                binding = 'SHA256SUMS-bound-sidecar'
                sidecar_filename = 'ninfer-4090-qwen38-v0.1.0-win-x64-qualification.json'
            }
        })

    $passedRedPath = Join-Path $root 'qualification-passed-red.json'
    $passedRed = Get-Content -LiteralPath $qualificationPath -Raw | ConvertFrom-Json
    $passedRed.status = 'passed'
    $passedRed.release_eligible = $true
    $passedRed.authority.supersedes_package_candidate_status = $true
    $passedRed.release_gates.G.status = 'passed'
    $passedRed.release_gates.L.status = 'not_run'
    $passedRed.release_gates.R.status = 'passed'
    Write-Json $passedRedPath $passedRed
    $passedRedOut = Join-Path $root 'out-passed-red'
    $passedRedRejected = $false
    try {
        & $PackageBuilderPath -ServerExecutable $server -PatchStackSha $patchSha -RuntimeFile @($runtimeA, $runtimeB) -ServerConfig $lfConfigPath -QualificationRecord $passedRedPath -OutputDirectory $passedRedOut | Out-Null
    }
    catch { $passedRedRejected = $_.Exception.Message -ceq 'qualification record claims passed while required release gates remain incomplete' }
    Assert-True $passedRedRejected 'passed qualification with red gate was accepted'
    Assert-True (-not (Test-Path -LiteralPath $passedRedOut)) 'red passed gate created output assets'

    $wrongQualificationPath = Join-Path $root 'qualification-wrong-model.json'
    $wrongQualification = Get-Content -LiteralPath $qualificationPath -Raw | ConvertFrom-Json
    $wrongQualification.identity.model_artifact_sha256 = ('0' * 64)
    Write-Json $wrongQualificationPath $wrongQualification
    $wrongOut = Join-Path $root 'out-wrong-model'
    $wrongModelRejected = $false
    try {
        & $PackageBuilderPath -ServerExecutable $server -PatchStackSha $patchSha `
            -RuntimeFile @($runtimeA, $runtimeB) -ServerConfig $lfConfigPath `
            -QualificationRecord $wrongQualificationPath -OutputDirectory $wrongOut | Out-Null
    }
    catch {
        if ($_.Exception.Message -cne 'qualification record artifact hashes do not match the release inputs') {
            throw
        }
        $wrongModelRejected = $true
    }
    Assert-True $wrongModelRejected 'wrong current model artifact identity was accepted'
    Assert-True (-not (Test-Path -LiteralPath $wrongOut)) 'rejected model identity created output assets'

    $wrongSemanticConfig = Get-Content -LiteralPath $lfConfigPath -Raw | ConvertFrom-Json
    $wrongSemanticConfig.engine.prefill_chunk = 256
    $wrongSemanticConfigPath = Join-Path $root 'server-config-wrong-semantics.json'
    Write-Json $wrongSemanticConfigPath $wrongSemanticConfig
    $wrongSemanticOut = Join-Path $root 'out-wrong-semantics'
    $wrongSemanticRejected = $false
    try {
        & $PackageBuilderPath -ServerExecutable $server -PatchStackSha $patchSha `
            -RuntimeFile @($runtimeA, $runtimeB) -ServerConfig $wrongSemanticConfigPath `
            -QualificationRecord $qualificationPath -OutputDirectory $wrongSemanticOut | Out-Null
    }
    catch {
        if ($_.Exception.Message -cne 'qualification record artifact hashes do not match the release inputs') {
            throw
        }
        $wrongSemanticRejected = $true
    }
    Assert-True $wrongSemanticRejected 'wrong configuration semantics were accepted'
    Assert-True (-not (Test-Path -LiteralPath $wrongSemanticOut)) 'rejected configuration semantics created output assets'

    $out = Join-Path $root 'out'
    $receipt = ((& $PackageBuilderPath -ServerExecutable $server -PatchStackSha $patchSha `
            -RuntimeFile @($runtimeA, $runtimeB) -ServerConfig $lfConfigPath `
            -QualificationRecord $qualificationPath -OutputDirectory $out) | Out-String) | ConvertFrom-Json
    Assert-True ([string]$receipt.package_file -ceq "$assetStem.zip") 'package filename mismatch'

    $crlfOut = Join-Path $root 'out-crlf'
    $crlfReceipt = ((& $PackageBuilderPath -ServerExecutable $server -PatchStackSha $patchSha `
            -RuntimeFile @($runtimeA, $runtimeB) -ServerConfig $crlfConfigPath `
            -QualificationRecord $qualificationPath -OutputDirectory $crlfOut) | Out-String) | ConvertFrom-Json
    Assert-True ([string]$crlfReceipt.package_file -ceq "$assetStem.zip") 'CRLF package filename mismatch'

    $expectedAssets = @(
        'Control-GpuOwner.ps1',
        'Control-Release.ps1',
        'Install-Release.ps1',
        'Protect-StateRoot.ps1',
        'SHA256SUMS',
        "$assetStem-qualification.json",
        "$assetStem.zip"
    ) | Sort-Object
    $actualAssets = @(Get-ChildItem -LiteralPath $out -File | Sort-Object Name | ForEach-Object Name)
    Assert-True ($actualAssets.Count -eq $expectedAssets.Count) 'release asset count mismatch'
    for ($index = 0; $index -lt $expectedAssets.Count; $index++) {
        Assert-True ($actualAssets[$index] -ceq $expectedAssets[$index]) 'release asset set mismatch'
    }

    $sumLines = @(Get-Content -LiteralPath (Join-Path $out 'SHA256SUMS') -Encoding UTF8)
    Assert-True ($sumLines.Count -eq 6) 'SHA256SUMS must cover six non-self assets'
    foreach ($line in $sumLines) {
        if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { throw 'invalid SHA256SUMS line' }
        $asset = Join-Path $out $Matches[2]
        Assert-True (Test-Path -LiteralPath $asset -PathType Leaf) 'SHA256SUMS names a missing asset'
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $asset).Hash.ToLowerInvariant()
        Assert-True ($actual -ceq $Matches[1]) 'SHA256SUMS hash mismatch'
    }

    $expanded = Join-Path $root 'expanded'
    Expand-Archive -LiteralPath (Join-Path $out "$assetStem.zip") -DestinationPath $expanded
    $payloads = @(Get-ChildItem -LiteralPath $expanded -Directory)
    Assert-True ($payloads.Count -eq 1) 'archive does not contain exactly one release root'
    $payload = $payloads[0].FullName
    Assert-True (Test-Path -LiteralPath (Join-Path $payload 'New-QualificationReceipt.ps1') -PathType Leaf) 'package omitted the qualification receipt constructor'
    Assert-True (Test-Path -LiteralPath (Join-Path $payload 'Control-GpuOwner.ps1') -PathType Leaf) 'package omitted the generic GPU-owner controller'
    Assert-True (Test-Path -LiteralPath (Join-Path $payload 'Protect-StateRoot.ps1') -PathType Leaf) 'package omitted the protected-state helper'
    foreach ($name in @('golden_equivalent.py', 'golden_equivalent_extension.ts', 'golden_equivalent_contract.json')) {
        Assert-True (Test-Path -LiteralPath (Join-Path $payload "smoke\$name") -PathType Leaf) "package omitted source-controlled Golden-equivalent asset: $name"
    }
    $manifest = Get-Content -LiteralPath (Join-Path $payload 'release-manifest.json') -Raw | ConvertFrom-Json
    Assert-True ([string]$manifest.release_version -ceq '0.1.0') 'manifest semantic version mismatch'
    Assert-True ([string]$manifest.asset_filename -ceq "$assetStem.zip") 'manifest asset filename mismatch'
    Assert-True ([string]$manifest.gpu_owner_controller_sha256 -cmatch '^[0-9a-f]{64}$') 'manifest omitted the generic GPU-owner controller identity'
    Assert-True ([string]$manifest.state_protection_sha256 -cmatch '^[0-9a-f]{64}$') 'manifest omitted the protected-state helper identity'
    foreach ($name in @('Control-Release.ps1', 'Control-GpuOwner.ps1', 'Protect-StateRoot.ps1', 'Install-Release.ps1')) {
        $standalone = Join-Path $out $name
        $member = Join-Path $payload $name
        Assert-True ((Get-FileHash -Algorithm SHA256 -LiteralPath $standalone).Hash.ToLowerInvariant() -ceq
            (Get-FileHash -Algorithm SHA256 -LiteralPath $member).Hash.ToLowerInvariant()) "standalone and ZIP member differ: $name"
    }

    $packagedConfigPath = Join-Path $payload 'server-config.json'
    Assert-True ((Get-FileHash -Algorithm SHA256 -LiteralPath $packagedConfigPath).Hash.ToLowerInvariant() -ceq $canonicalConfigSha256) 'LF input did not produce the qualified config bytes'
    Assert-True ([Int64](Get-Item -LiteralPath $packagedConfigPath).Length -eq 1092) 'qualified config byte count changed'
    $expandedCrlf = Join-Path $root 'expanded-crlf'
    Expand-Archive -LiteralPath (Join-Path $crlfOut "$assetStem.zip") -DestinationPath $expandedCrlf
    $crlfPayloads = @(Get-ChildItem -LiteralPath $expandedCrlf -Directory)
    Assert-True ($crlfPayloads.Count -eq 1) 'CRLF archive does not contain exactly one release root'
    $packagedCrlfConfigPath = Join-Path $crlfPayloads[0].FullName 'server-config.json'
    Assert-True ((Get-FileHash -Algorithm SHA256 -LiteralPath $packagedCrlfConfigPath).Hash.ToLowerInvariant() -ceq $canonicalConfigSha256) 'CRLF input did not preserve the qualified config bytes'
    Assert-True ([Int64](Get-Item -LiteralPath $packagedCrlfConfigPath).Length -eq 1092) 'CRLF qualified config byte count changed'
    Assert-True ((Get-FileHash -Algorithm SHA256 -LiteralPath $ServerConfigPath).Hash.ToLowerInvariant() -ceq $sourceConfigSha256) 'package builder changed its source config'

    $checksums = Get-Content -LiteralPath (Join-Path $payload 'checksums.json') -Raw | ConvertFrom-Json
    $listed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $checksums.files) {
        $relative = ([string]$entry.relative_path).Replace('/', [IO.Path]::DirectorySeparatorChar)
        Assert-True ($listed.Add($relative)) 'duplicate payload checksum entry'
        $path = Join-Path $payload $relative
        Assert-True (Test-Path -LiteralPath $path -PathType Leaf) 'payload checksum names a missing file'
        Assert-True ((Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() -ceq
            [string]$entry.sha256) 'payload checksum mismatch'
    }
    $payloadFiles = @(Get-ChildItem -LiteralPath $payload -File -Recurse |
        Where-Object Name -cne 'checksums.json')
    Assert-True ($payloadFiles.Count -eq $listed.Count) 'payload has an unverified file'

    $qualification = Get-Content -LiteralPath (Join-Path $out "$assetStem-qualification.json") -Raw |
        ConvertFrom-Json
    $zipHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $out "$assetStem.zip")).Hash.ToLowerInvariant()
    Assert-True ([string]$qualification.package.sha256 -ceq $zipHash) 'qualification is not bound to archive hash'
    Assert-True ([int]$qualification.package.runtime_dlls -eq 2) 'qualification runtime DLL count mismatch'
    Assert-True ([string]$qualification.status -ceq 'candidate_ready') 'package builder changed candidate qualification status'
    Assert-True ($qualification.release_eligible -eq $false) 'assembled package sidecar claims release eligibility'
    Assert-True ($qualification.authority.supersedes_package_candidate_status -eq $false) 'assembled package sidecar claims final authority supersession'
    Assert-True ([string]$receipt.qualification_status -ceq 'candidate_ready') 'package receipt omitted candidate status'
    Assert-True ($receipt.release_eligible_at_assembly -eq $false) 'package receipt claims assembly-time eligibility'
    Assert-True ($receipt.external_final_qualification_required -eq $true) 'package receipt omitted external final authority requirement'
    Assert-True ($receipt.candidate_status_superseded -eq $false) 'package receipt claims candidate status was already superseded'

    [ordered]@{
        artifact_type = 'ninfer_release_asset_regression'
        schema_version = 1
        status = 'passed'
        assets = $expectedAssets.Count
        payload_entries = $listed.Count
        wrong_model_artifact_identity_rejections = 1
        passed_red_gate_rejections = 1
        hidden_payload_coverage = $true
        pinned_model_bytes = [Int64]$releaseSpec.model.bytes
        candidate_root_directories = @($releaseSpec.lifecycle.candidate_root_directories).Count
        model_artifact_ownership = [string]$releaseSpec.lifecycle.model_artifact_ownership
        interrupted_install_reentry = [string]$releaseSpec.lifecycle.interrupted_install_reentry
        historical_private_corpus_reused = [bool]$releaseSpec.golden_equivalent.historical_private_corpus_reused
        canonical_config_sha256 = $canonicalConfigSha256
        lf_config_normalizations = 1
        crlf_config_normalizations = 1
        wrong_config_semantic_rejections = 1
        source_config_unchanged = $true
    } | ConvertTo-Json -Compress
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
