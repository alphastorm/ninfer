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
    $identity = "$Program upstream_base_sha=$UpstreamSha patch_stack_sha=$ReleaseSha build_profile=omp-v0.2.1-rtx3090 build_type=Release cxx_compiler=fixture cuda_compiler=fixture cuda_toolkit=13.3 cuda_architecture=86 source_dirty=false"
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
    Assert-Equal ([string]$spec.build_profile) 'omp-v0.2.1-rtx3090' 'release build profile changed'
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
    Assert-True ([bool]$config.session_checkpoint.enabled) 'durable session checkpoints are disabled'
    Assert-Equal ([int]$config.session_checkpoint.quota_mib) 65536 'checkpoint quota changed'
    Assert-Equal ([int]$config.session_checkpoint.staging_mib) 256 'checkpoint staging bound changed'
    Assert-True ($config.PSObject.Properties.Name -notcontains 'persistent_cache') 'unsupported persistent prompt cache is configured'
    Assert-Equal ([string]::Join(',', @($spec.qualification.required_gates))) 'gpu-only-thermal-envelope,sm86-driver-hardware-identity,authenticated-real-client-protocol,advertised-context-retrieval,process-restart-session-continuation,omp-client-end-to-end,bounded-gpu-performance-at-qualified-cap,gaming-drain-restart-rollback' 'qualification gate contract changed'

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
    Assert-Equal ([string]$receiptOne.support_assets.installer_sha256) (Get-Sha256 (Join-Path $outOne 'Install-Release.ps1')) 'installer support asset hash mismatch'
    Assert-Equal ([string]$receiptOne.support_assets.controller_sha256) (Get-Sha256 (Join-Path $outOne 'Control-Release.ps1')) 'controller support asset hash mismatch'
    Assert-Equal ([string]$receiptOne.support_assets.gpu_owner_controller_sha256) (Get-Sha256 (Join-Path $outOne 'Control-GpuOwner.ps1')) 'GPU-owner support asset hash mismatch'
    Assert-Equal ([string]$receiptOne.support_assets.state_protection_sha256) (Get-Sha256 (Join-Path $outOne 'Protect-StateRoot.ps1')) 'state-protection support asset hash mismatch'
    Assert-Equal ([string]$receiptOne.qualification_status) 'hardware-pending' 'package claimed unperformed hardware qualification'
    Assert-Equal ([string]$receiptOne.runtime_source_sha) $releaseHead 'default package runtime source changed'
    Assert-Equal ([string]$receiptOne.package_source_sha) $releaseHead 'default package source changed'
    Assert-Equal ([string]$receiptOne.qualification_status_authority) 'immutable-pre-hardware-package-build' 'package build status authority changed'
    Assert-Equal ([string]$receiptOne.later_external_beta_authority.artifact_type) 'ninfer_rtx3090_beta_qualification' 'package omitted the later beta authority type'
    Assert-Equal ([int]$receiptOne.later_external_beta_authority.schema_version) 3 'package omitted the later beta authority schema'
    Assert-True ([bool]$receiptOne.later_external_beta_authority.supersedes_only_when_exact_package_sha256_matches) 'package beta authority is not exact-asset-bound'
    Assert-Equal ([bool]$receiptOne.later_external_beta_authority.mutates_this_build_receipt) $false 'package beta authority mutates build history'
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
            'Control-Release.ps1', 'Control-GpuOwner.ps1', 'Protect-StateRoot.ps1', 'New-QualificationReceipt.ps1', 'release-spec.json',
            'server-config.json', 'build-identity.json', 'SHA256SUMS.txt',
            'smoke/agent_protocol.py', 'smoke/serve_contract.py'
        )) {
        Assert-True ($members -contains "$rootName/$name") "generated package omitted $name"
    }
    $innerChecksumLines = @(& $tar '-xOf' $packagePath "$rootName/SHA256SUMS.txt")
    if ($LASTEXITCODE -ne 0) { throw 'generated package checksum manifest extraction failed' }
    $innerChecksums = @{}
    foreach ($line in $innerChecksumLines) {
        if ($line -cnotmatch '^([0-9a-f]{64})  (.+)$' -or $innerChecksums.ContainsKey($Matches[2])) {
            throw 'inner package checksum manifest is malformed or duplicated'
        }
        $innerChecksums[$Matches[2]] = $Matches[1]
    }
    $scriptMemberHashes = [ordered]@{
        'Install-Release.ps1' = [string]$receiptOne.support_assets.installer_sha256
        'Control-Release.ps1' = [string]$receiptOne.support_assets.controller_sha256
        'Control-GpuOwner.ps1' = [string]$receiptOne.support_assets.gpu_owner_controller_sha256
        'Protect-StateRoot.ps1' = [string]$receiptOne.support_assets.state_protection_sha256
        'New-QualificationReceipt.ps1' = [string]$receiptOne.support_assets.qualification_constructor_sha256
    }
    foreach ($entry in $scriptMemberHashes.GetEnumerator()) {
        Assert-True $innerChecksums.ContainsKey([string]$entry.Key) "inner manifest omitted shipped script: $($entry.Key)"
        Assert-Equal ([string]$innerChecksums[[string]$entry.Key]) ([string]$entry.Value) "shipped script member hash mismatch: $($entry.Key)"
    }
    $forbiddenHookLiterals = @(
        'InstallTestMode', 'Invoke-InstallFault', 'NINFER_TEST_INSTALL_',
        'NInferSimulatedInterruption', 'NInferLifecycleHarness',
        'InternalSourceTestMode', 'TestBypass', 'FaultInjection',
        'SimulatedFailure', 'SimulatedInterruption'
    )
    foreach ($scriptName in $scriptMemberHashes.Keys) {
        $scriptLines = @(& $tar '-xOf' $packagePath "$rootName/$scriptName")
        if ($LASTEXITCODE -ne 0) { throw "published script extraction failed: $scriptName" }
        $scriptText = [string]::Join([string][char]10, $scriptLines)
        foreach ($literal in $forbiddenHookLiterals) {
            Assert-True (-not $scriptText.Contains($literal)) "published script contains test/fault hook: ${scriptName}:$literal"
        }
    }
    Assert-True (@($members | Where-Object { $_ -match '4090|DirectStorage|directstorage' }).Count -eq 0) 'generated package contains a forbidden old identity'

    $outerSums = Join-Path $outOne 'SHA256SUMS'
    Assert-Equal ([string]$receiptOne.checksums.role) 'closed-outer-distribution-set' 'outer checksum role mismatch'
    Assert-Equal ([int]$receiptOne.checksums.entries) 9 'outer checksum entry count mismatch'
    Assert-Equal ([string]$receiptOne.checksums.line_ending) 'LF' 'outer checksum line-ending claim mismatch'
    $outerSumBytes = [IO.File]::ReadAllBytes($outerSums)
    Assert-True ($outerSumBytes.Length -gt 0 -and $outerSumBytes[-1] -eq 10) 'outer checksum manifest lacks a terminal LF'
    Assert-True (-not ($outerSumBytes -contains 13)) 'outer checksum manifest contains checkout-dependent CR bytes'
    $outerLines = [IO.File]::ReadAllLines($outerSums, [Text.Encoding]::ASCII)
    $spdxName = 'ninfer-rtx3090-omp-v0.2.1-beta.1-windows-x86_64-cuda13.3-rtx3090.spdx.json'
    Assert-Equal @($outerLines | Where-Object { $_.EndsWith("  $spdxName") }).Count 1 'outer checksum manifest omitted or duplicated the SPDX SBOM'
    foreach ($line in $outerLines) {
        if ($line -cnotmatch '^([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._-]*)$') {
            throw 'outer checksum line is malformed'
        }
        Assert-Equal (Get-Sha256 (Join-Path $outOne $Matches[2])) $Matches[1] "outer checksum mismatch: $($Matches[2])"
    }
    Assert-Equal @($outerLines | Where-Object { $_.EndsWith('  package-build-receipt.json') }).Count 1 'outer checksum manifest omitted or duplicated the package-build receipt'
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

    $candidate = [pscustomobject]@{
        runtime_source_commit = $releaseHead
        package_source_commit = $releaseHead
        runtime_source_paths_changed = $false
        runtime_source_diff_sha256 = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
        upstream_base_commit = $upstream
        lineage_base_commit = 'c467349e375d6aa76afca63c0042bbc0869549aa'
        server_binary_sha256 = ('1' * 64)
        cli_binary_sha256 = ('2' * 64)
        benchmark_binary_sha256 = ('3' * 64)
        config_sha256 = ('4' * 64)
        model_artifact_sha256 = ('5' * 64)
    }
    $platform = [pscustomobject]@{
        gpu = 'NVIDIA GeForce RTX 3090'
        vram_mib = 24576
        cuda_architecture = 'sm_86'
        driver_version = '570.00'
        operating_system = 'Microsoft Windows 11 Pro fixture'
        qualified_power_limit_w = 300
        single_gpu = $true
    }
    $releaseAssets = [pscustomobject]@{
        package = [pscustomobject]@{
            filename = [string]$receiptOne.package.filename
            sha256 = [string]$receiptOne.package.sha256
            bytes = [Int64]$receiptOne.package.bytes
        }
        source_archive = [pscustomobject]@{
            filename = 'ninfer-rtx3090-omp-v0.2.1-beta.1-source.tar.gz'
            sha256 = ('6' * 64)
            bytes = 123
        }
        spdx_sbom = [pscustomobject]@{
            filename = 'ninfer-rtx3090-omp-v0.2.1-beta.1-windows-x86_64-cuda13.3-rtx3090.spdx.json'
            sha256 = ('7' * 64)
            bytes = 456
        }
        checksums = [pscustomobject]@{
            filename = 'SHA256SUMS'
            sha256 = Get-Sha256 $outerSums
            bytes = [Int64](Get-Item -LiteralPath $outerSums).Length
        }
        package_build_receipt = [pscustomobject]@{
            filename = 'package-build-receipt.json'
            sha256 = Get-Sha256 (Join-Path $outOne 'package-build-receipt.json')
            bytes = [Int64](Get-Item -LiteralPath (Join-Path $outOne 'package-build-receipt.json')).Length
        }
        inner_checksums_sha256 = ('8' * 64)
        installer_sha256 = Get-Sha256 (Join-Path $outOne 'Install-Release.ps1')
        lifecycle_controller_sha256 = Get-Sha256 (Join-Path $outOne 'Control-Release.ps1')
        gpu_owner_controller_sha256 = Get-Sha256 (Join-Path $outOne 'Control-GpuOwner.ps1')
        state_protection_helper_sha256 = Get-Sha256 (Join-Path $outOne 'Protect-StateRoot.ps1')
        qualification_constructor_sha256 = Get-Sha256 $ReceiptConstructorPath
        package_build_qualification_status = 'hardware-pending'
        packaged_release_spec_qualification_status = 'hardware-pending'
    }
    $protocol = [pscustomobject]@{
        status = 'passed'; evidence_platform = 'remote-linux-rtx3090-runtime'; deferred_reason = ''
        checks_passed = 15; checks_total = 15; receipt_sha256 = ('9' * 64)
    }
    $remoteNative = [pscustomobject]@{
        status = 'passed'; evidence_platform = 'remote-linux-rtx3090-runtime'
        source_commit = $releaseHead; gpu = 'NVIDIA GeForce RTX 3090'
        vram_mib = 24576; driver_version = 'test-driver'; operating_system = 'remote Linux test fixture'
        cuda_architecture = 'sm_86'
        focused_tests = @('ninfer_response_store_test', 'ninfer_session_checkpoint_store_test', 'ninfer_http_contract_test')
        pod_deleted = $true
        hourly_price_usd = 0.22; receipt_sha256 = ('6' * 64)
    }
    $longContext = [pscustomobject]@{
        status = 'passed'; evidence_platform = 'remote-linux-rtx3090-runtime'; deferred_reason = ''; prompt_tokens = 64512; completion_tokens = 17
        elapsed_seconds = 1.0; exact_output = 'ORCHID=493817; COLOR=COBALT'
        receipt_sha256 = ('a' * 64)
    }
    $checkpointRestart = [pscustomobject]@{
        status = 'passed'; evidence_platform = 'remote-linux-rtx3090-runtime'; deferred_reason = ''; checkpoint_files = 2; checkpoint_bytes = 1024
        cached_input_tokens = 45; continuation_elapsed_seconds = 1.0
        exact_output = 'CHECKPOINT-3090-731942'; failed_delete_commit_regression = 'passed'
        cross_session_eviction_regression = 'passed'; receipt_sha256 = ('b' * 64)
        middle_delete_restart_regression = 'passed'
        latest_delete_nonrestorable_regression = 'passed'
        standalone_delete_nonrestorable_regression = 'passed'
        durable_only_lru_delete_regression = 'passed'
        post_commit_sync_regression = 'passed'
        superseded_generation_reclamation = 'eager-unless-active-reader-or-cleanup-failure'
        deletion_semantics = 'logical-object-deletion'; secure_erasure_claimed = $false
    }
    $performance = [pscustomobject]@{
        status = 'passed'; evidence_platform = 'remote-linux-rtx3090-runtime'; deferred_reason = ''; cohort = 1; generation_tokens = 1024
        decode_tokens_per_second = 1.0; prefill_prompt_tokens = 4403
        prefill_tokens_per_second = 1.0; max_power_w = 300.0; max_temperature_c = 50
        max_gpu_utilization_percent = 100; max_memory_used_mib = 20000
        receipt_sha256 = ('c' * 64)
    }
    $instrumentedLifecycle = [pscustomobject]@{
        status = 'passed'; evidence_class = 'generated-instrumented-transaction-harness'
        production_installer_sha256 = [string]$releaseAssets.installer_sha256
        instrumented_installer_sha256 = ('d' * 64); production_installer_executed = $false
        production_controller_sha256 = ('c' * 64); instrumented_controller_sha256 = ('d' * 64)
        instrumented_state_protection_sha256 = ('e' * 64)
        state_protection_evidence_class = 'generated-stub-no-acl-semantics'
        substitution_manifest_sha256 = ('0' * 64); substitution_count = 22
        security_claims_included = $false
        production_state_protection_executed = $false
        effective_acl_evidence = $false; injected_failures = 10; interrupted_repairs = 2
        receipt_sha256 = ('f' * 64)
    }
    $shippedLifecycle = [pscustomobject]@{
        status = 'passed'; installer_sha256 = [string]$releaseAssets.installer_sha256
        evidence_scope = 'exact-current-package'; fresh_package_gate_deferred = $false
        deferred_reason = ''
        production_installer_executed = $true; instrumented_harness_used = $false
        clean_installs = 1; upgrades = 1; rollback_directions = 2; identity_gates_verified = 8
        receipt_sha256 = ('f' * 64)
    }
    $releaseAssetTests = [pscustomobject]@{
        status = 'passed'; deterministic_packages = 2; package_members_verified = 16
        public_listen_rejections = 1; secret_free_receipts = 3; published_scripts_hook_scanned = 5
        receipt_sha256 = ('1' * 64)
    }
    $stateSecurity = [pscustomobject]@{
        status = 'passed'; root_dacl_protected = $true; atomic_race_collision_rejections = 1
        evidence_scope = 'exact-current-package'; fresh_package_gate_deferred = $false
        deferred_reason = ''
        raced_state_recursive_deletions = 0; low_privilege_effective_read_denials = 2
        null_dacl_rejections = 1
        low_privilege_effective_write_denials = 2; precreated_or_unowned_root_rejections = 2
        root_or_child_junction_rejections = 2; installer_prewrite_root_rejections = 3
        active_interactive_gpu_rejections = 1; request_log_effective_access_denials = 2
        managed_release_acl_state_assertions = 6; managed_request_log_acl_state_assertions = 1
        managed_rollback_directions = 2; shipped_test_bypass = $false; restored_power_limit_w = 370
        retained_state_helper_hash_assertions = 4
        populated_root_status_milliseconds = 1
        gpu_power_evidence_class = 'real-trusted-absolute-nvidia-smi'
        absolute_nvidia_shim_interceptions = 0
        receipt_sha256 = ('2' * 64)
    }
    $stateSecurityFixture = [pscustomobject]@{
        status = 'passed'
        gpu_power_evidence_class = 'instrumented-function-shim-no-hardware-claim'
        hardware_claimed = $false
        gpu_power_fixture_calls = 1
        prepared_lease_restore_assertions = 1
        null_dacl_rejections = 1
        atomic_race_collision_rejections = 1
        receipt_sha256 = ('7' * 64)
    }
    $historicalWindowsEvidence = [pscustomobject]@{
        status = 'historical-passed'; evidence_scope = 'previous-exact-package-only'
        package_sha256 = 'e74c097f064279f860a0d6a738bb4470503f63df0e8b531ccaeb48402c923ef9'
        package_source_commit = '7555db29d2e5d517f74bd05d45020028d0f454c9'
        runtime_source_commit = '7555db29d2e5d517f74bd05d45020028d0f454c9'
        state_security_receipt_sha256 = ('8' * 64)
        shipped_lifecycle_receipt_sha256 = ('9' * 64)
        omp_client_receipt_sha256 = ('a' * 64)
        applies_to_current_package = $false
    }
    $ompClient = [pscustomobject]@{
        status = 'passed'; omp_version = 'omp/18.0.9'; archive_sha256 = ('3' * 64)
        evidence_scope = 'exact-current-package'; fresh_package_gate_deferred = $false; deferred_reason = ''
        binary_sha256 = ('4' * 64); events = 1; typed_tool_name = 'read'; tool_results = 1
        exact_final_answer = $true; receipt_sha256 = ('5' * 64)
    }
    $publicDisclosure = [pscustomobject]@{
        status = 'passed_with_classified_generic_references'; tracked_source_files_scanned = 100
        tracked_source_private_marker_or_unclassified_credential_findings = 0
        release_bytes_scanned = 1000; release_files_scanned = 20
        binary_package_private_path_findings = 0; source_archive_generic_path_references = 2
        classified_private_fleet_or_credential_findings = 0; raw_scan_sha256 = ('6' * 64)
    }
    $receiptArguments = @{
        QualifiedUtc = [DateTimeOffset]::UtcNow.ToString('o')
        Candidate = $candidate; Platform = $platform; ReleaseAssets = $releaseAssets
        Protocol = $protocol; RemoteNative = $remoteNative; LongContext = $longContext; CheckpointRestart = $checkpointRestart
        Performance = $performance; InstrumentedLifecycle = $instrumentedLifecycle
        ShippedLifecycle = $shippedLifecycle; ReleaseAssetTests = $releaseAssetTests
        StateSecurityFixture = $stateSecurityFixture; StateSecurity = $stateSecurity
        HistoricalWindowsEvidence = $historicalWindowsEvidence
        OmpClient = $ompClient; PublicDisclosure = $publicDisclosure
    }

    $originalProtocolStatus = $protocol.status
    $originalProtocolReceipt = $protocol.receipt_sha256
    $protocol.status = 'not_run'
    $protocol.deferred_reason = 'community-rtx3090-cuda-runtime-unavailable'
    $protocol.receipt_sha256 = $null
    $incomplete = New-NInferQualificationReceipt @receiptArguments
    $protocol.status = $originalProtocolStatus
    $protocol.deferred_reason = ''
    $protocol.receipt_sha256 = $originalProtocolReceipt
    Assert-Equal ([string]$incomplete.status) 'incomplete' 'receipt constructor passed an unrun gate'
    Assert-Equal ([bool]$incomplete.beta_qualified) $false 'incomplete receipt claimed beta qualification'
    Assert-Equal ([bool]$incomplete.qualification_authority.supersession_performed) $false 'incomplete receipt superseded pending build authority'
    Assert-Equal ([int]$incomplete.qualification.authenticated_agent_protocol.checks_passed) 0 'deferred protocol retained passed counters'
    Assert-Equal ([int]$incomplete.scope.supported_clients.Count) 0 'incomplete receipt advertised a supported client'
    Assert-Equal ([int]$incomplete.scope.supported_api_surfaces.Count) 0 'incomplete receipt advertised supported API surfaces'
    $incompleteJson = $incomplete | ConvertTo-Json -Depth 24 -Compress
    Assert-True $incompleteJson.Contains('"supported_clients":[]') 'incomplete receipt serialized supported_clients as null rather than an empty array'
    Assert-True $incompleteJson.Contains('"supported_api_surfaces":[]') 'incomplete receipt serialized supported_api_surfaces as null rather than an empty array'
    Assert-Equal ([bool]$incomplete.platform.fresh_exact_package_windows_gates_passed) $false 'incomplete receipt marked the target Windows platform as proven'

    $ompClient.status = 'not_run'
    $ompClient.evidence_scope = 'exact-current-package-deferred'
    $ompClient.fresh_package_gate_deferred = $true
    $ompClient.deferred_reason = 'fresh-windows-rtx3090-unavailable-after-user-handoff'
    $ompClient.receipt_sha256 = $null
    $deferredOmp = New-NInferQualificationReceipt @receiptArguments
    Assert-Equal ([string]$deferredOmp.status) 'incomplete' 'receipt constructor passed a deferred OMP gate'
    Assert-Equal $deferredOmp.qualification.omp_windows_client.archive_sha256 $null 'deferred OMP gate retained a pass-shaped archive hash'
    Assert-Equal ([int]$deferredOmp.qualification.omp_windows_client.events) 0 'deferred OMP gate retained passed event counters'
    Assert-Equal ([bool]$deferredOmp.qualification.omp_windows_client.exact_final_answer) $false 'deferred OMP gate retained passed acceptance'
    foreach ($gate in @($shippedLifecycle, $stateSecurity)) {
        $gate.status = 'not_run'
        $gate.evidence_scope = 'exact-current-package-deferred'
        $gate.fresh_package_gate_deferred = $true
        $gate.deferred_reason = 'fresh-windows-rtx3090-unavailable-after-user-handoff'
        $gate.receipt_sha256 = $null
    }
    $deferredWindows = New-NInferQualificationReceipt @receiptArguments
    Assert-Equal ([int]$deferredWindows.qualification.windows_lifecycle_shipped.upgrades) 0 'deferred shipped lifecycle retained passed counters'
    Assert-Equal $deferredWindows.qualification.windows_lifecycle_shipped.receipt_sha256 $null 'deferred shipped lifecycle retained a historical evidence hash'
    Assert-Equal ([int]$deferredWindows.qualification.windows_state_security.low_privilege_effective_read_denials) 0 'deferred state security retained passed counters'
    Assert-Equal $deferredWindows.qualification.windows_state_security.receipt_sha256 $null 'deferred state security retained a historical evidence hash'
    foreach ($gate in @($shippedLifecycle, $stateSecurity)) {
        $gate.status = 'passed'
        $gate.evidence_scope = 'exact-current-package'
        $gate.fresh_package_gate_deferred = $false
        $gate.deferred_reason = ''
    }
    $shippedLifecycle.receipt_sha256 = ('f' * 64)
    $stateSecurity.receipt_sha256 = ('2' * 64)
    $ompClient.status = 'passed'
    $ompClient.evidence_scope = 'exact-current-package'
    $ompClient.fresh_package_gate_deferred = $false
    $ompClient.deferred_reason = ''
    $ompClient.receipt_sha256 = ('5' * 64)

    $passed = New-NInferQualificationReceipt @receiptArguments
    Assert-Equal ([string]$passed.artifact_type) 'ninfer_rtx3090_beta_qualification' 'constructor emitted the wrong public receipt type'
    Assert-Equal ([int]$passed.schema_version) 3 'constructor emitted the wrong public receipt schema'
    Assert-Equal ([string]$passed.status) 'passed' 'receipt constructor did not pass complete gates'
    Assert-Equal ([bool]$passed.beta_qualified) $true 'passed receipt omitted beta authority'
    Assert-Equal ([bool]$passed.automatic_route_activation_allowed) $false 'beta receipt authorized automatic routing'
    Assert-Equal ([bool]$passed.stable_promotion_performed) $false 'beta receipt claimed stable promotion'
    Assert-Equal ([bool]$passed.production_route_activation_performed) $false 'beta receipt claimed production activation'
    Assert-Equal ([bool]$passed.qualification_authority.supersession_performed) $true 'passed beta authority did not perform supersession'
    Assert-Equal ([string]$passed.qualification_authority.supersedes_package_build_receipt_qualification_status) 'hardware-pending' 'beta authority did not supersede the build receipt status'
    Assert-Equal ([string]$passed.qualification_authority.supersedes_source_archive_receipt_status) 'pending-requalification' 'beta authority did not supersede the source-archive pending receipt'
    Assert-Equal ([bool]$passed.qualification_authority.historical_build_receipts_mutated) $false 'beta authority claimed to mutate build history'
    Assert-Equal ([string]::Join(',', @($passed.public_disclosure.forbidden_marker_classes))) 'private-fleet-identifiers,private-home-paths,credential-material' 'public disclosure marker policy changed'
    $passedText = $passed | ConvertTo-Json -Depth 24 -Compress
    Assert-True (-not $passedText.Contains('GPU-fixture-3090')) 'qualification receipt exposed a GPU UUID'

    foreach ($gate in @($protocol, $longContext, $checkpointRestart, $performance)) {
        $gate.evidence_platform = 'native-windows-rtx3090-package'
    }
    $remoteNative.evidence_platform = 'native-windows-rtx3090-build'
    $remoteNative.operating_system = 'Windows test fixture'
    $remoteNative.pod_deleted = $false
    $remoteNative.hourly_price_usd = 0
    $nativeWindowsPassed = New-NInferQualificationReceipt @receiptArguments
    Assert-Equal ([string]$nativeWindowsPassed.status) 'passed' 'constructor rejected exact native-Windows package evidence'
    Assert-Equal ([string]$nativeWindowsPassed.qualification.checkpoint_process_restart.evidence_platform) 'native-windows-rtx3090-package' 'constructor lost native-Windows restart evidence scope'
    Assert-Equal ([string]$nativeWindowsPassed.qualification.remote_linux_sm86_native.evidence_platform) 'native-windows-rtx3090-build' 'constructor lost native-Windows contract evidence scope'

    $candidate | Add-Member -NotePropertyName api_key -NotePropertyValue 'must-not-appear'
    $credentialPropertyRejected = $false
    try { New-NInferQualificationReceipt @receiptArguments | Out-Null }
    catch { $credentialPropertyRejected = $_.Exception.Message -like '*forbidden property*' }
    $candidate.PSObject.Properties.Remove('api_key')
    Assert-True $credentialPropertyRejected 'qualification constructor accepted an undeclared credential property'

    $platform | Add-Member -NotePropertyName gpu_uuid -NotePropertyValue 'GPU-fixture-3090'
    $gpuUuidRejected = $false
    try { New-NInferQualificationReceipt @receiptArguments | Out-Null }
    catch { $gpuUuidRejected = $_.Exception.Message -like '*forbidden property*' }
    $platform.PSObject.Properties.Remove('gpu_uuid')
    Assert-True $gpuUuidRejected 'qualification constructor accepted a GPU UUID input'

    $originalOperatingSystem = $platform.operating_system
    $platform.operating_system = 'C:\Users\Example Operator\private-build'
    $privateWindowsPathRejected = $false
    try { New-NInferQualificationReceipt @receiptArguments | Out-Null }
    catch { $privateWindowsPathRejected = $_.Exception.Message -like '*private path or credential*' }
    $platform.operating_system = $originalOperatingSystem
    Assert-True $privateWindowsPathRejected 'qualification constructor accepted a JSON-escaped canonical Windows home path'

    $deepRoot = [pscustomobject]@{}
    $deepCursor = $deepRoot
    foreach ($depth in 1..35) {
        $child = [pscustomobject]@{}
        $deepCursor | Add-Member -NotePropertyName child -NotePropertyValue $child
        $deepCursor = $child
    }
    $deepCursor | Add-Member -NotePropertyName gpu_uuid -NotePropertyValue 'GPU-deep-fixture'
    $candidate | Add-Member -NotePropertyName deep_disclosure_fixture -NotePropertyValue $deepRoot
    $deepForbiddenRejected = $false
    try { New-NInferQualificationReceipt @receiptArguments | Out-Null }
    catch { $deepForbiddenRejected = $_.Exception.Message -like '*forbidden property*' }
    $candidate.PSObject.Properties.Remove('deep_disclosure_fixture')
    Assert-True $deepForbiddenRejected 'qualification constructor accepted a forbidden property below depth 32'

    $originalPackageFilename = $releaseAssets.package.filename
    $releaseAssets.package.filename = 'subdirectory\package.tar.gz'
    $assetBindingRejected = $false
    try { New-NInferQualificationReceipt @receiptArguments | Out-Null }
    catch { $assetBindingRejected = $_.Exception.Message -like '*final public asset name*' }
    $releaseAssets.package.filename = $originalPackageFilename
    Assert-True $assetBindingRejected 'qualification constructor accepted an inexact package filename'

    [ordered]@{
        artifact_type = 'ninfer_windows_release_assets_regression'
        schema_version = 2
        status = 'passed'
        deterministic_packages = 2
        package_members_verified = 16
        public_listen_rejections = 1
        secret_free_receipts = 3
        published_scripts_hook_scanned = $scriptMemberHashes.Count
    } | ConvertTo-Json -Compress
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
