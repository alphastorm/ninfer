[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PackageBuilderPath,
    [Parameter(Mandatory = $true)][string]$ReceiptConstructorPath,
    [Parameter(Mandatory = $true)][string]$ServerConfigPath,
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [string]$PythonExecutable = 'python3'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PackageBuilderPath = (Resolve-Path -LiteralPath $PackageBuilderPath).Path
$ReceiptConstructorPath = (Resolve-Path -LiteralPath $ReceiptConstructorPath).Path
$ServerConfigPath = (Resolve-Path -LiteralPath $ServerConfigPath).Path
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
. $ReceiptConstructorPath

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-Equal([object]$Actual, [object]$Expected, [string]$Message) {
    if ([string]$Actual -cne [string]$Expected) { throw $Message }
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Write-FakeBinary(
    [string]$Path,
    [string]$Program,
    [string]$UpstreamSha,
    [string]$ReleaseSha
) {
    $identity = "$Program upstream_base_sha=$UpstreamSha patch_stack_sha=$ReleaseSha build_profile=omp-v0.2.0-rtx3090 build_type=Release cxx_compiler=fixture cuda_compiler=fixture cuda_toolkit=12.8 cuda_architecture=86 source_dirty=false"
    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
        $compiler = (Get-Command cl.exe -ErrorAction Stop).Source
        $sourcePath = [IO.Path]::ChangeExtension($Path, '.cpp')
        $objectPath = [IO.Path]::ChangeExtension($Path, '.obj')
        $source = @"
#include <iostream>
int main() {
    std::cout << R"NINFER($identity)NINFER" << std::endl;
    return 0;
}
"@
        [IO.File]::WriteAllText($sourcePath, $source, [Text.UTF8Encoding]::new($false))
        & $compiler /nologo /EHsc $sourcePath "/Fe:$Path" "/Fo:$objectPath" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "failed to compile fixture executable: $Program" }
    }
    else {
        [IO.File]::WriteAllLines(
            $Path,
            @('#!/bin/sh', "printf '%s\n' '$identity'"),
            [Text.UTF8Encoding]::new($false)
        )
        & chmod '+x' $Path
        if ($LASTEXITCODE -ne 0) { throw "failed to make fixture executable: $Program" }
    }
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('ninfer-windows-assets-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $specPath = Join-Path (Split-Path -Parent $PackageBuilderPath) 'release-spec.json'
    $spec = Get-Content -LiteralPath $specPath -Raw | ConvertFrom-Json
    $config = Get-Content -LiteralPath $ServerConfigPath -Raw | ConvertFrom-Json
    Assert-Equal ([string]$spec.source.lineage_base_sha) 'c467349e375d6aa76afca63c0042bbc0869549aa' 'release spec lineage changed'
    Assert-Equal ([string]$spec.build_profile) 'omp-v0.2.0-rtx3090' 'release build profile changed'
    Assert-Equal ([string]$spec.gpu.cuda_architecture) 'sm_86' 'release CUDA architecture changed'
    Assert-Equal ([string]$spec.model.revision) '18dfc887423fa5aabf3cb56fac41490e462b3fab' 'model revision changed'
    Assert-Equal ([Int64]$spec.model.bytes) 18210531328 'model byte identity changed'
    Assert-Equal ([string]$spec.model.sha256) 'eec39564993d6e9c7d5e383382a760f093465c9d163ec9a1bd6b80199514bf3e' 'model SHA-256 changed'
    Assert-Equal ([string]::Join(',', @($spec.lifecycle.state_pointers))) 'prepared_release,active_release,previous_release' 'lifecycle pointer contract changed'
    Assert-Equal ([string]::Join(',', @($spec.network.allowed_listen_host_classes))) 'loopback,tailscale-ipv4' 'release network policy changed'
    Assert-Equal ([string]$spec.network.authentication) 'required-api-key-file' 'release authentication policy changed'
    Assert-Equal ([string]$config.listen.host) '127.0.0.1' 'default listen host is not loopback'
    Assert-Equal ([int]$config.engine.max_concurrency) 1 'default profile is not C1'
    Assert-Equal ([string]$config.engine.kv_dtype) 'int8' 'default KV profile changed'
    Assert-Equal ([string]$config.speculative.backend) 'mtp' 'default speculative profile changed'
    Assert-Equal ([int]$config.speculative.draft_tokens) 3 'default MTP draft count changed'

    $releaseHead = ((& git -C $SourceRoot rev-parse HEAD) | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $releaseHead -cnotmatch '^[0-9a-f]{40}$') {
        throw 'failed to resolve source release head'
    }
    $upstream = 'ef6ecc3c139b43fc4d3e1b92df474305e8429544'
    $binaryDirectory = Join-Path $root 'bin'
    New-Item -ItemType Directory -Path $binaryDirectory | Out-Null
    $extension = if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) { '.exe' } else { '' }
    $ninfer = Join-Path $binaryDirectory "ninfer$extension"
    $server = Join-Path $binaryDirectory "ninfer-serve$extension"
    $benchmark = Join-Path $binaryDirectory "ninfer_bench$extension"
    Write-FakeBinary $ninfer 'ninfer' $upstream $releaseHead
    Write-FakeBinary $server 'ninfer-serve' $upstream $releaseHead
    Write-FakeBinary $benchmark 'ninfer_bench' $upstream $releaseHead
    $runtimeA = Join-Path $binaryDirectory 'runtime-a.dll'
    $runtimeB = Join-Path $binaryDirectory 'runtime-b.dll'
    [IO.File]::WriteAllText($runtimeA, 'runtime-a', [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($runtimeB, 'runtime-b', [Text.UTF8Encoding]::new($false))

    $outOne = Join-Path $root 'out-one'
    $outTwo = Join-Path $root 'out-two'
    $common = @{
        SourceRoot = $SourceRoot
        NInferExecutable = $ninfer
        ServerExecutable = $server
        BenchmarkExecutable = $benchmark
        ReleaseHeadSha = $releaseHead
        RuntimeFile = @($runtimeA, $runtimeB)
        ServerConfig = $ServerConfigPath
        PythonExecutable = $PythonExecutable
        SourceDateEpoch = 1700000000
    }
    $receiptOne = ((& $PackageBuilderPath @common -OutputDirectory $outOne) | Out-String).Trim() | ConvertFrom-Json
    $receiptTwo = ((& $PackageBuilderPath @common -OutputDirectory $outTwo) | Out-String).Trim() | ConvertFrom-Json
    Assert-Equal ([string]$receiptOne.package.sha256) ([string]$receiptTwo.package.sha256) 'deterministic package SHA-256 changed between runs'
    Assert-Equal ([Int64]$receiptOne.package.bytes) ([Int64]$receiptTwo.package.bytes) 'deterministic package size changed between runs'
    Assert-Equal ([string]$receiptOne.config_sha256) (Get-Sha256 $ServerConfigPath) 'package receipt config identity mismatch'
    Assert-Equal ([string]$receiptOne.qualification_status) 'hardware-pending' 'package claimed unperformed hardware qualification'
    Assert-Equal ([int]$receiptOne.secret_values_recorded) 0 'package receipt recorded a secret value'

    $packagePath = Join-Path $outOne ([string]$receiptOne.package.filename)
    Assert-Equal (Get-Sha256 $packagePath) ([string]$receiptOne.package.sha256) 'package receipt hash mismatch'
    $tar = (Get-Command tar -ErrorAction Stop).Source
    $members = @(& $tar '-tzf' $packagePath)
    if ($LASTEXITCODE -ne 0) { throw 'generated package inventory failed' }
    $rootName = [IO.Path]::GetFileName($packagePath).Replace('.tar.gz', '')
    foreach ($name in @(
            'bin/ninfer.exe', 'bin/ninfer-serve.exe', 'bin/ninfer_bench.exe',
            'bin/runtime-a.dll', 'bin/runtime-b.dll', 'Install-Release.ps1',
            'Control-Release.ps1', 'New-QualificationReceipt.ps1', 'release-spec.json',
            'server-config.json', 'build-identity.json', 'SHA256SUMS.txt'
        )) {
        Assert-True ($members -contains "$rootName/$name") "generated package omitted $name"
    }
    Assert-True (@($members | Where-Object { $_ -match '4090|DirectStorage|directstorage' }).Count -eq 0) 'generated package contains a forbidden old identity'

    $outerSums = Join-Path $outOne 'SHA256SUMS'
    Assert-Equal (Get-Sha256 $outerSums) ([string]$receiptOne.checksums.sha256) 'outer checksum identity mismatch'
    foreach ($line in [IO.File]::ReadAllLines($outerSums, [Text.Encoding]::ASCII)) {
        if ($line -cnotmatch '^([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._-]*)$') {
            throw 'outer checksum line is malformed'
        }
        Assert-Equal (Get-Sha256 (Join-Path $outOne $Matches[2])) $Matches[1] "outer checksum mismatch: $($Matches[2])"
    }
    Assert-True (Test-Path -LiteralPath (Join-Path $outOne 'package-build-receipt.json') -PathType Leaf) 'package build receipt was not published'

    $badConfigPath = Join-Path $root 'bad-server-config.json'
    $badConfig = Get-Content -LiteralPath $ServerConfigPath -Raw | ConvertFrom-Json
    $badConfig.listen.host = '0.0.0.0'
    [IO.File]::WriteAllText($badConfigPath, ($badConfig | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
    $badOut = Join-Path $root 'bad-out'
    $badCommon = $common.Clone()
    $badCommon.ServerConfig = $badConfigPath
    $badCommon.OutputDirectory = $badOut
    $badRejected = $false
    $badError = 'none'
    try { & $PackageBuilderPath @badCommon | Out-Null }
    catch {
        $badError = $_.Exception.Message
        $badRejected = $badError -like '*IPv4 loopback or Tailscale*'
    }
    Assert-True $badRejected "package builder accepted a public wildcard listen host; observed error: $badError"
    Assert-True (-not (Test-Path -LiteralPath $badOut)) 'rejected network config published output'

    $identity = [pscustomobject]@{
        release_head_sha = $releaseHead
        upstream_base_sha = $upstream
        lineage_base_sha = 'c467349e375d6aa76afca63c0042bbc0869549aa'
        build_profile = 'omp-v0.2.0-rtx3090'
        cuda_architecture = 'sm_86'
        ninfer_sha256 = ('1' * 64)
        server_sha256 = ('2' * 64)
        benchmark_sha256 = ('3' * 64)
        config_sha256 = ('4' * 64)
        model_sha256 = ('5' * 64)
        package_sha256 = ('6' * 64)
        checksums_sha256 = ('7' * 64)
        api_key = 'must-not-appear'
    }
    $configuration = [pscustomobject]@{
        deployment_profile = 'qwen38-3090-omp-v0.2.0-c1'
        max_context = 65536
        kv_capacity = 'auto'
        kv_dtype = 'int8'
        prefill_chunk = 1024
        concurrency = 1
        speculative_backend = 'mtp'
        speculative_draft_tokens = 3
    }
    $hardware = [pscustomobject]@{
        gpu_name = 'NVIDIA GeForce RTX 3090'
        gpu_uuid = 'GPU-fixture-3090'
        driver_version = '570.00'
        compute_capability = '8.6'
    }
    $notRun = [pscustomobject]@{ status = 'not_run'; evidence_sha256 = $null }
    $incomplete = New-NInferQualificationReceipt -QualifiedUtc ([DateTimeOffset]::UtcNow.ToString('o')) -ReleaseId 'fixture' -Identity $identity -Configuration $configuration -Hardware $hardware -Thermal $notRun -Protocol $notRun -LongContext $notRun -RoleCorpus $notRun -PowerSweep $notRun -Lifecycle $notRun
    Assert-Equal ([string]$incomplete.status) 'incomplete' 'receipt constructor passed unrun hardware gates'
    $incompleteText = $incomplete | ConvertTo-Json -Depth 16 -Compress
    Assert-True (-not $incompleteText.Contains('must-not-appear')) 'qualification receipt copied an undeclared secret field'
    $passedGate = [pscustomobject]@{ status = 'passed'; evidence_sha256 = ('a' * 64) }
    $passed = New-NInferQualificationReceipt -QualifiedUtc ([DateTimeOffset]::UtcNow.ToString('o')) -ReleaseId 'fixture' -Identity $identity -Configuration $configuration -Hardware $hardware -Thermal $passedGate -Protocol $passedGate -LongContext $passedGate -RoleCorpus $passedGate -PowerSweep $passedGate -Lifecycle $passedGate
    Assert-Equal ([string]$passed.status) 'passed' 'receipt constructor did not pass complete gates'

    [ordered]@{
        artifact_type = 'ninfer_windows_release_assets_regression'
        schema_version = 2
        status = 'passed'
        deterministic_packages = 2
        package_members_verified = 12
        public_listen_rejections = 1
        secret_free_receipts = 3
    } | ConvertTo-Json -Compress
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
