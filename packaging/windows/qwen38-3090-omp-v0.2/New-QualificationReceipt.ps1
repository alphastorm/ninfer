Set-StrictMode -Version Latest

function Get-NInferQualificationProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Label
    )
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { throw "$Label.$Name is required" }
    return $property.Value
}

function Get-NInferQualificationSha256 {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Label
    )
    $value = [string](Get-NInferQualificationProperty $Object $Name $Label)
    if ($value -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Label.$Name must be a lower-case SHA-256"
    }
    return $value
}

function Get-NInferQualificationGitSha {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Label
    )
    $value = [string](Get-NInferQualificationProperty $Object $Name $Label)
    if ($value -cnotmatch '^[0-9a-f]{40}$') {
        throw "$Label.$Name must be a full lower-case Git SHA"
    }
    return $value
}

function Get-NInferQualificationStatus {
    param([Parameter(Mandatory = $true)][object]$Gate, [Parameter(Mandatory = $true)][string]$Name)
    $status = [string](Get-NInferQualificationProperty $Gate 'status' $Name)
    if ($status -cnotin @('passed', 'failed', 'not_run')) {
        throw "$Name.status must be passed, failed, or not_run"
    }
    return $status
}

function Get-NInferQualificationEvidenceSha256 {
    param([Parameter(Mandatory = $true)][object]$Gate, [Parameter(Mandatory = $true)][string]$Name)
    $status = Get-NInferQualificationStatus $Gate $Name
    $property = $Gate.PSObject.Properties['receipt_sha256']
    $value = if ($null -eq $property) { '' } else { [string]$property.Value }
    if ($status -ceq 'passed' -and $value -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Name.receipt_sha256 must be a lower-case SHA-256 when passed"
    }
    if ($status -cne 'passed' -and -not [string]::IsNullOrEmpty($value) -and
        $value -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Name.receipt_sha256 must be empty or a lower-case SHA-256"
    }
    return $(if ([string]::IsNullOrEmpty($value)) { $null } else { $value })
}

function Get-NInferQualificationAsset {
    param(
        [Parameter(Mandatory = $true)][object]$Assets,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$ExpectedFilename
    )
    $asset = Get-NInferQualificationProperty $Assets $Name 'release_assets'
    $filename = [string](Get-NInferQualificationProperty $asset 'filename' "release_assets.$Name")
    if ($filename -cne $ExpectedFilename -or [IO.Path]::GetFileName($filename) -cne $filename) {
        throw "release_assets.$Name.filename does not bind the final public asset name"
    }
    $bytes = [Int64](Get-NInferQualificationProperty $asset 'bytes' "release_assets.$Name")
    if ($bytes -le 0) { throw "release_assets.$Name.bytes must be positive" }
    return [ordered]@{
        filename = $filename
        sha256 = Get-NInferQualificationSha256 $asset 'sha256' "release_assets.$Name"
        bytes = $bytes
    }
}

function New-NInferQualificationReceipt {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$QualifiedUtc,
        [Parameter(Mandatory = $true)][object]$Candidate,
        [Parameter(Mandatory = $true)][object]$Platform,
        [Parameter(Mandatory = $true)][object]$ReleaseAssets,
        [Parameter(Mandatory = $true)][object]$Protocol,
        [Parameter(Mandatory = $true)][object]$LongContext,
        [Parameter(Mandatory = $true)][object]$CheckpointRestart,
        [Parameter(Mandatory = $true)][object]$Performance,
        [Parameter(Mandatory = $true)][object]$InstrumentedLifecycle,
        [Parameter(Mandatory = $true)][object]$ShippedLifecycle,
        [Parameter(Mandatory = $true)][object]$ReleaseAssetTests,
        [Parameter(Mandatory = $true)][object]$StateSecurityFixture,
        [Parameter(Mandatory = $true)][object]$StateSecurity,
        [Parameter(Mandatory = $true)][object]$OmpClient,
        [Parameter(Mandatory = $true)][object]$PublicDisclosure
    )

    $qualified = [DateTimeOffset]::MinValue
    if (-not [DateTimeOffset]::TryParseExact(
            $QualifiedUtc,
            'o',
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal,
            [ref]$qualified
        )) {
        throw 'qualified UTC must use the round-trip timestamp format'
    }

    $candidateReceipt = [ordered]@{
        runtime_source_commit = Get-NInferQualificationGitSha $Candidate 'runtime_source_commit' 'candidate'
        package_source_commit = Get-NInferQualificationGitSha $Candidate 'package_source_commit' 'candidate'
        runtime_source_paths_changed = [bool](Get-NInferQualificationProperty $Candidate 'runtime_source_paths_changed' 'candidate')
        runtime_source_diff_sha256 = Get-NInferQualificationSha256 $Candidate 'runtime_source_diff_sha256' 'candidate'
        upstream_base_commit = Get-NInferQualificationGitSha $Candidate 'upstream_base_commit' 'candidate'
        lineage_base_commit = Get-NInferQualificationGitSha $Candidate 'lineage_base_commit' 'candidate'
        server_binary_sha256 = Get-NInferQualificationSha256 $Candidate 'server_binary_sha256' 'candidate'
        cli_binary_sha256 = Get-NInferQualificationSha256 $Candidate 'cli_binary_sha256' 'candidate'
        benchmark_binary_sha256 = Get-NInferQualificationSha256 $Candidate 'benchmark_binary_sha256' 'candidate'
        config_sha256 = Get-NInferQualificationSha256 $Candidate 'config_sha256' 'candidate'
        model_artifact_sha256 = Get-NInferQualificationSha256 $Candidate 'model_artifact_sha256' 'candidate'
    }
    if ($candidateReceipt.runtime_source_paths_changed -or
        $candidateReceipt.runtime_source_diff_sha256 -cne 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855') {
        throw 'candidate package-only source advancement changed runtime source paths'
    }

    $platformReceipt = [ordered]@{
        gpu = [string](Get-NInferQualificationProperty $Platform 'gpu' 'platform')
        vram_mib = [int](Get-NInferQualificationProperty $Platform 'vram_mib' 'platform')
        cuda_architecture = [string](Get-NInferQualificationProperty $Platform 'cuda_architecture' 'platform')
        driver_version = [string](Get-NInferQualificationProperty $Platform 'driver_version' 'platform')
        operating_system = [string](Get-NInferQualificationProperty $Platform 'operating_system' 'platform')
        qualified_power_limit_w = [int](Get-NInferQualificationProperty $Platform 'qualified_power_limit_w' 'platform')
        single_gpu = [bool](Get-NInferQualificationProperty $Platform 'single_gpu' 'platform')
    }
    if ($platformReceipt.gpu -cne 'NVIDIA GeForce RTX 3090' -or
        $platformReceipt.vram_mib -ne 24576 -or
        $platformReceipt.cuda_architecture -cne 'sm_86' -or
        [string]::IsNullOrWhiteSpace($platformReceipt.driver_version) -or
        [string]::IsNullOrWhiteSpace($platformReceipt.operating_system) -or
        $platformReceipt.qualified_power_limit_w -ne 300 -or
        -not $platformReceipt.single_gpu -or
        $null -ne $Platform.PSObject.Properties['gpu_uuid']) {
        throw 'platform does not match the public single-RTX-3090 qualification profile'
    }

    $releaseAssetsReceipt = [ordered]@{
        package = Get-NInferQualificationAsset $ReleaseAssets 'package' 'ninfer-rtx3090-omp-v0.2.0-windows-x86_64-cuda12.8-rtx3090.tar.gz'
        source_archive = Get-NInferQualificationAsset $ReleaseAssets 'source_archive' 'ninfer-rtx3090-omp-v0.2.0-source.tar.gz'
        spdx_sbom = Get-NInferQualificationAsset $ReleaseAssets 'spdx_sbom' 'ninfer-rtx3090-omp-v0.2.0-windows-x86_64-cuda12.8-rtx3090.spdx.json'
        checksums = Get-NInferQualificationAsset $ReleaseAssets 'checksums' 'SHA256SUMS'
        package_build_receipt = Get-NInferQualificationAsset $ReleaseAssets 'package_build_receipt' 'package-build-receipt.json'
        inner_checksums_sha256 = Get-NInferQualificationSha256 $ReleaseAssets 'inner_checksums_sha256' 'release_assets'
        installer_sha256 = Get-NInferQualificationSha256 $ReleaseAssets 'installer_sha256' 'release_assets'
        lifecycle_controller_sha256 = Get-NInferQualificationSha256 $ReleaseAssets 'lifecycle_controller_sha256' 'release_assets'
        gpu_owner_controller_sha256 = Get-NInferQualificationSha256 $ReleaseAssets 'gpu_owner_controller_sha256' 'release_assets'
        state_protection_helper_sha256 = Get-NInferQualificationSha256 $ReleaseAssets 'state_protection_helper_sha256' 'release_assets'
        qualification_constructor_sha256 = Get-NInferQualificationSha256 $ReleaseAssets 'qualification_constructor_sha256' 'release_assets'
    }
    $packageBuildStatus = [string](Get-NInferQualificationProperty $ReleaseAssets 'package_build_qualification_status' 'release_assets')
    $packagedSpecStatus = [string](Get-NInferQualificationProperty $ReleaseAssets 'packaged_release_spec_qualification_status' 'release_assets')
    if ($packageBuildStatus -cne 'hardware-pending' -or $packagedSpecStatus -cne 'hardware-pending') {
        throw 'release assets do not preserve their pre-hardware qualification status'
    }

    $protocolStatus = Get-NInferQualificationStatus $Protocol 'authenticated_agent_protocol'
    $protocolReceipt = [ordered]@{
        status = $protocolStatus
        checks_passed = [int](Get-NInferQualificationProperty $Protocol 'checks_passed' 'authenticated_agent_protocol')
        checks_total = [int](Get-NInferQualificationProperty $Protocol 'checks_total' 'authenticated_agent_protocol')
        receipt_sha256 = Get-NInferQualificationEvidenceSha256 $Protocol 'authenticated_agent_protocol'
    }
    if ($protocolStatus -ceq 'passed' -and
        ($protocolReceipt.checks_passed -ne 15 -or $protocolReceipt.checks_total -ne 15)) {
        throw 'authenticated_agent_protocol must bind the complete 15/15 protocol'
    }

    $longContextStatus = Get-NInferQualificationStatus $LongContext 'long_context_64k'
    $longContextReceipt = [ordered]@{
        status = $longContextStatus
        prompt_tokens = [int](Get-NInferQualificationProperty $LongContext 'prompt_tokens' 'long_context_64k')
        completion_tokens = [int](Get-NInferQualificationProperty $LongContext 'completion_tokens' 'long_context_64k')
        elapsed_seconds = [double](Get-NInferQualificationProperty $LongContext 'elapsed_seconds' 'long_context_64k')
        exact_output = [string](Get-NInferQualificationProperty $LongContext 'exact_output' 'long_context_64k')
        receipt_sha256 = Get-NInferQualificationEvidenceSha256 $LongContext 'long_context_64k'
    }
    if ($longContextStatus -ceq 'passed' -and
        ($longContextReceipt.prompt_tokens -ne 64512 -or
         $longContextReceipt.completion_tokens -le 0 -or
         $longContextReceipt.elapsed_seconds -le 0 -or
         [string]::IsNullOrWhiteSpace($longContextReceipt.exact_output))) {
        throw 'long_context_64k does not bind the exact 64K retrieval gate'
    }

    $checkpointStatus = Get-NInferQualificationStatus $CheckpointRestart 'checkpoint_process_restart'
    $checkpointReceipt = [ordered]@{
        status = $checkpointStatus
        checkpoint_files = [int](Get-NInferQualificationProperty $CheckpointRestart 'checkpoint_files' 'checkpoint_process_restart')
        checkpoint_bytes = [Int64](Get-NInferQualificationProperty $CheckpointRestart 'checkpoint_bytes' 'checkpoint_process_restart')
        cached_input_tokens = [int](Get-NInferQualificationProperty $CheckpointRestart 'cached_input_tokens' 'checkpoint_process_restart')
        continuation_elapsed_seconds = [double](Get-NInferQualificationProperty $CheckpointRestart 'continuation_elapsed_seconds' 'checkpoint_process_restart')
        exact_output = [string](Get-NInferQualificationProperty $CheckpointRestart 'exact_output' 'checkpoint_process_restart')
        failed_delete_commit_regression = [string](Get-NInferQualificationProperty $CheckpointRestart 'failed_delete_commit_regression' 'checkpoint_process_restart')
        cross_session_eviction_regression = [string](Get-NInferQualificationProperty $CheckpointRestart 'cross_session_eviction_regression' 'checkpoint_process_restart')
        middle_delete_restart_regression = [string](Get-NInferQualificationProperty $CheckpointRestart 'middle_delete_restart_regression' 'checkpoint_process_restart')
        latest_delete_nonrestorable_regression = [string](Get-NInferQualificationProperty $CheckpointRestart 'latest_delete_nonrestorable_regression' 'checkpoint_process_restart')
        standalone_delete_nonrestorable_regression = [string](Get-NInferQualificationProperty $CheckpointRestart 'standalone_delete_nonrestorable_regression' 'checkpoint_process_restart')
        deletion_semantics = [string](Get-NInferQualificationProperty $CheckpointRestart 'deletion_semantics' 'checkpoint_process_restart')
        secure_erasure_claimed = [bool](Get-NInferQualificationProperty $CheckpointRestart 'secure_erasure_claimed' 'checkpoint_process_restart')
        receipt_sha256 = Get-NInferQualificationEvidenceSha256 $CheckpointRestart 'checkpoint_process_restart'
    }
    if ($checkpointStatus -ceq 'passed' -and
        ($checkpointReceipt.checkpoint_files -le 0 -or
         $checkpointReceipt.checkpoint_bytes -le 0 -or
         $checkpointReceipt.cached_input_tokens -le 0 -or
         $checkpointReceipt.continuation_elapsed_seconds -le 0 -or
         [string]::IsNullOrWhiteSpace($checkpointReceipt.exact_output) -or
         $checkpointReceipt.failed_delete_commit_regression -cne 'passed' -or
         $checkpointReceipt.cross_session_eviction_regression -cne 'passed' -or
         $checkpointReceipt.middle_delete_restart_regression -cne 'passed' -or
         $checkpointReceipt.latest_delete_nonrestorable_regression -cne 'passed' -or
         $checkpointReceipt.standalone_delete_nonrestorable_regression -cne 'passed' -or
         $checkpointReceipt.deletion_semantics -cne 'logical-object-deletion' -or
         $checkpointReceipt.secure_erasure_claimed)) {
        throw 'checkpoint_process_restart does not bind restart and atomic-delete regressions'
    }

    $performanceStatus = Get-NInferQualificationStatus $Performance 'performance'
    $performanceReceipt = [ordered]@{
        status = $performanceStatus
        cohort = [int](Get-NInferQualificationProperty $Performance 'cohort' 'performance')
        generation_tokens = [int](Get-NInferQualificationProperty $Performance 'generation_tokens' 'performance')
        decode_tokens_per_second = [double](Get-NInferQualificationProperty $Performance 'decode_tokens_per_second' 'performance')
        prefill_prompt_tokens = [int](Get-NInferQualificationProperty $Performance 'prefill_prompt_tokens' 'performance')
        prefill_tokens_per_second = [double](Get-NInferQualificationProperty $Performance 'prefill_tokens_per_second' 'performance')
        max_power_w = [double](Get-NInferQualificationProperty $Performance 'max_power_w' 'performance')
        max_temperature_c = [int](Get-NInferQualificationProperty $Performance 'max_temperature_c' 'performance')
        max_gpu_utilization_percent = [int](Get-NInferQualificationProperty $Performance 'max_gpu_utilization_percent' 'performance')
        max_memory_used_mib = [int](Get-NInferQualificationProperty $Performance 'max_memory_used_mib' 'performance')
        receipt_sha256 = Get-NInferQualificationEvidenceSha256 $Performance 'performance'
    }
    if ($performanceStatus -ceq 'passed' -and
        ($performanceReceipt.cohort -ne 1 -or $performanceReceipt.generation_tokens -ne 1024 -or
         $performanceReceipt.decode_tokens_per_second -le 0 -or
         $performanceReceipt.prefill_tokens_per_second -le 0 -or
         $performanceReceipt.max_power_w -gt 301.0)) {
        throw 'performance does not bind the bounded C1 300 W profile'
    }

    $instrumentedStatus = Get-NInferQualificationStatus $InstrumentedLifecycle 'windows_lifecycle_instrumented'
    $instrumentedReceipt = [ordered]@{
        status = $instrumentedStatus
        evidence_class = [string](Get-NInferQualificationProperty $InstrumentedLifecycle 'evidence_class' 'windows_lifecycle_instrumented')
        production_installer_sha256 = Get-NInferQualificationSha256 $InstrumentedLifecycle 'production_installer_sha256' 'windows_lifecycle_instrumented'
        instrumented_installer_sha256 = Get-NInferQualificationSha256 $InstrumentedLifecycle 'instrumented_installer_sha256' 'windows_lifecycle_instrumented'
        production_controller_sha256 = Get-NInferQualificationSha256 $InstrumentedLifecycle 'production_controller_sha256' 'windows_lifecycle_instrumented'
        instrumented_controller_sha256 = Get-NInferQualificationSha256 $InstrumentedLifecycle 'instrumented_controller_sha256' 'windows_lifecycle_instrumented'
        instrumented_state_protection_sha256 = Get-NInferQualificationSha256 $InstrumentedLifecycle 'instrumented_state_protection_sha256' 'windows_lifecycle_instrumented'
        state_protection_evidence_class = [string](Get-NInferQualificationProperty $InstrumentedLifecycle 'state_protection_evidence_class' 'windows_lifecycle_instrumented')
        substitution_manifest_sha256 = Get-NInferQualificationSha256 $InstrumentedLifecycle 'substitution_manifest_sha256' 'windows_lifecycle_instrumented'
        substitution_count = [int](Get-NInferQualificationProperty $InstrumentedLifecycle 'substitution_count' 'windows_lifecycle_instrumented')
        security_claims_included = [bool](Get-NInferQualificationProperty $InstrumentedLifecycle 'security_claims_included' 'windows_lifecycle_instrumented')
        production_installer_executed = [bool](Get-NInferQualificationProperty $InstrumentedLifecycle 'production_installer_executed' 'windows_lifecycle_instrumented')
        production_state_protection_executed = [bool](Get-NInferQualificationProperty $InstrumentedLifecycle 'production_state_protection_executed' 'windows_lifecycle_instrumented')
        effective_acl_evidence = [bool](Get-NInferQualificationProperty $InstrumentedLifecycle 'effective_acl_evidence' 'windows_lifecycle_instrumented')
        injected_failures = [int](Get-NInferQualificationProperty $InstrumentedLifecycle 'injected_failures' 'windows_lifecycle_instrumented')
        interrupted_repairs = [int](Get-NInferQualificationProperty $InstrumentedLifecycle 'interrupted_repairs' 'windows_lifecycle_instrumented')
        receipt_sha256 = Get-NInferQualificationEvidenceSha256 $InstrumentedLifecycle 'windows_lifecycle_instrumented'
    }
    if ($instrumentedStatus -ceq 'passed' -and
        ($instrumentedReceipt.evidence_class -cne 'generated-instrumented-transaction-harness' -or
         $instrumentedReceipt.production_installer_sha256 -ceq $instrumentedReceipt.instrumented_installer_sha256 -or
         $instrumentedReceipt.state_protection_evidence_class -cne 'generated-stub-no-acl-semantics' -or
         $instrumentedReceipt.production_controller_sha256 -ceq $instrumentedReceipt.instrumented_controller_sha256 -or
         $instrumentedReceipt.substitution_count -ne 22 -or
         $instrumentedReceipt.security_claims_included -or
         $instrumentedReceipt.production_installer_executed -or
         $instrumentedReceipt.production_state_protection_executed -or
         $instrumentedReceipt.effective_acl_evidence -or
         $instrumentedReceipt.injected_failures -ne 10 -or
         $instrumentedReceipt.interrupted_repairs -ne 2)) {
        throw 'windows_lifecycle_instrumented overclaims shipped-byte or ACL evidence'
    }

    $shippedStatus = Get-NInferQualificationStatus $ShippedLifecycle 'windows_lifecycle_shipped'
    $shippedReceipt = [ordered]@{
        status = $shippedStatus
        evidence_scope = [string](Get-NInferQualificationProperty $ShippedLifecycle 'evidence_scope' 'windows_lifecycle_shipped')
        fresh_package_gate_deferred = [bool](Get-NInferQualificationProperty $ShippedLifecycle 'fresh_package_gate_deferred' 'windows_lifecycle_shipped')
        deferred_reason = [string](Get-NInferQualificationProperty $ShippedLifecycle 'deferred_reason' 'windows_lifecycle_shipped')
        installer_sha256 = Get-NInferQualificationSha256 $ShippedLifecycle 'installer_sha256' 'windows_lifecycle_shipped'
        production_installer_executed = [bool](Get-NInferQualificationProperty $ShippedLifecycle 'production_installer_executed' 'windows_lifecycle_shipped')
        instrumented_harness_used = [bool](Get-NInferQualificationProperty $ShippedLifecycle 'instrumented_harness_used' 'windows_lifecycle_shipped')
        clean_installs = [int](Get-NInferQualificationProperty $ShippedLifecycle 'clean_installs' 'windows_lifecycle_shipped')
        upgrades = [int](Get-NInferQualificationProperty $ShippedLifecycle 'upgrades' 'windows_lifecycle_shipped')
        rollback_directions = [int](Get-NInferQualificationProperty $ShippedLifecycle 'rollback_directions' 'windows_lifecycle_shipped')
        identity_gates_verified = [int](Get-NInferQualificationProperty $ShippedLifecycle 'identity_gates_verified' 'windows_lifecycle_shipped')
        receipt_sha256 = Get-NInferQualificationEvidenceSha256 $ShippedLifecycle 'windows_lifecycle_shipped'
    }
    if ($shippedStatus -ceq 'passed' -and
        ($shippedReceipt.fresh_package_gate_deferred -or
         $shippedReceipt.installer_sha256 -cne $releaseAssetsReceipt.installer_sha256 -or
         -not $shippedReceipt.production_installer_executed -or
         $shippedReceipt.instrumented_harness_used -or
         $shippedReceipt.clean_installs -lt 1 -or $shippedReceipt.upgrades -lt 1 -or
         $shippedReceipt.rollback_directions -lt 2 -or
         $shippedReceipt.identity_gates_verified -ne 8)) {
        throw 'windows_lifecycle_shipped does not attest the exact shipped installer path'
    }
    if ($shippedStatus -ceq 'not_run' -and
        (-not $shippedReceipt.fresh_package_gate_deferred -or
         $shippedReceipt.deferred_reason -cne 'fresh-windows-rtx3090-unavailable-after-user-handoff')) {
        throw 'deferred shipped Windows lifecycle gate lacks the authorized evidence boundary'
    }

    $assetStatus = Get-NInferQualificationStatus $ReleaseAssetTests 'windows_release_assets'
    $assetReceipt = [ordered]@{
        status = $assetStatus
        deterministic_packages = [int](Get-NInferQualificationProperty $ReleaseAssetTests 'deterministic_packages' 'windows_release_assets')
        package_members_verified = [int](Get-NInferQualificationProperty $ReleaseAssetTests 'package_members_verified' 'windows_release_assets')
        public_listen_rejections = [int](Get-NInferQualificationProperty $ReleaseAssetTests 'public_listen_rejections' 'windows_release_assets')
        secret_free_receipts = [int](Get-NInferQualificationProperty $ReleaseAssetTests 'secret_free_receipts' 'windows_release_assets')
        receipt_sha256 = Get-NInferQualificationEvidenceSha256 $ReleaseAssetTests 'windows_release_assets'
    }
    if ($assetStatus -ceq 'passed' -and
        ($assetReceipt.deterministic_packages -ne 2 -or $assetReceipt.package_members_verified -lt 16 -or
         $assetReceipt.public_listen_rejections -ne 1 -or $assetReceipt.secret_free_receipts -lt 3)) {
        throw 'windows_release_assets does not bind deterministic final assets'
    }

    $securityStatus = Get-NInferQualificationStatus $StateSecurity 'windows_state_security'
    $securityFixtureStatus = Get-NInferQualificationStatus $StateSecurityFixture 'windows_state_security_fixture'
    $securityFixtureReceipt = [ordered]@{
        status = $securityFixtureStatus
        evidence_class = [string](Get-NInferQualificationProperty $StateSecurityFixture 'gpu_power_evidence_class' 'windows_state_security_fixture')
        hardware_claimed = [bool](Get-NInferQualificationProperty $StateSecurityFixture 'hardware_claimed' 'windows_state_security_fixture')
        gpu_power_fixture_calls = [int](Get-NInferQualificationProperty $StateSecurityFixture 'gpu_power_fixture_calls' 'windows_state_security_fixture')
        null_dacl_rejections = [int](Get-NInferQualificationProperty $StateSecurityFixture 'null_dacl_rejections' 'windows_state_security_fixture')
        atomic_race_collision_rejections = [int](Get-NInferQualificationProperty $StateSecurityFixture 'atomic_race_collision_rejections' 'windows_state_security_fixture')
        receipt_sha256 = Get-NInferQualificationEvidenceSha256 $StateSecurityFixture 'windows_state_security_fixture'
    }
    if ($securityFixtureStatus -ceq 'passed' -and
        ($securityFixtureReceipt.evidence_class -cne 'instrumented-function-shim-no-hardware-claim' -or
         $securityFixtureReceipt.hardware_claimed -or
         $securityFixtureReceipt.gpu_power_fixture_calls -lt 1 -or
         $securityFixtureReceipt.null_dacl_rejections -lt 1 -or
         $securityFixtureReceipt.atomic_race_collision_rejections -lt 1)) {
        throw 'windows_state_security_fixture overclaims or omits instrumented controls'
    }
    $securityReceipt = [ordered]@{
        status = $securityStatus
        evidence_scope = [string](Get-NInferQualificationProperty $StateSecurity 'evidence_scope' 'windows_state_security')
        fresh_package_gate_deferred = [bool](Get-NInferQualificationProperty $StateSecurity 'fresh_package_gate_deferred' 'windows_state_security')
        deferred_reason = [string](Get-NInferQualificationProperty $StateSecurity 'deferred_reason' 'windows_state_security')
        root_dacl_protected = [bool](Get-NInferQualificationProperty $StateSecurity 'root_dacl_protected' 'windows_state_security')
        atomic_race_collision_rejections = [int](Get-NInferQualificationProperty $StateSecurity 'atomic_race_collision_rejections' 'windows_state_security')
        raced_state_recursive_deletions = [int](Get-NInferQualificationProperty $StateSecurity 'raced_state_recursive_deletions' 'windows_state_security')
        null_dacl_rejections = [int](Get-NInferQualificationProperty $StateSecurity 'null_dacl_rejections' 'windows_state_security')
        low_privilege_effective_read_denials = [int](Get-NInferQualificationProperty $StateSecurity 'low_privilege_effective_read_denials' 'windows_state_security')
        low_privilege_effective_write_denials = [int](Get-NInferQualificationProperty $StateSecurity 'low_privilege_effective_write_denials' 'windows_state_security')
        precreated_or_unowned_root_rejections = [int](Get-NInferQualificationProperty $StateSecurity 'precreated_or_unowned_root_rejections' 'windows_state_security')
        root_or_child_junction_rejections = [int](Get-NInferQualificationProperty $StateSecurity 'root_or_child_junction_rejections' 'windows_state_security')
        installer_prewrite_root_rejections = [int](Get-NInferQualificationProperty $StateSecurity 'installer_prewrite_root_rejections' 'windows_state_security')
        active_interactive_gpu_rejections = [int](Get-NInferQualificationProperty $StateSecurity 'active_interactive_gpu_rejections' 'windows_state_security')
        request_log_effective_access_denials = [int](Get-NInferQualificationProperty $StateSecurity 'request_log_effective_access_denials' 'windows_state_security')
        managed_release_acl_state_assertions = [int](Get-NInferQualificationProperty $StateSecurity 'managed_release_acl_state_assertions' 'windows_state_security')
        managed_request_log_acl_state_assertions = [int](Get-NInferQualificationProperty $StateSecurity 'managed_request_log_acl_state_assertions' 'windows_state_security')
        managed_rollback_directions = [int](Get-NInferQualificationProperty $StateSecurity 'managed_rollback_directions' 'windows_state_security')
        retained_state_helper_hash_assertions = [int](Get-NInferQualificationProperty $StateSecurity 'retained_state_helper_hash_assertions' 'windows_state_security')
        populated_root_status_milliseconds = [int](Get-NInferQualificationProperty $StateSecurity 'populated_root_status_milliseconds' 'windows_state_security')
        gpu_power_evidence_class = [string](Get-NInferQualificationProperty $StateSecurity 'gpu_power_evidence_class' 'windows_state_security')
        absolute_nvidia_shim_interceptions = [int](Get-NInferQualificationProperty $StateSecurity 'absolute_nvidia_shim_interceptions' 'windows_state_security')
        shipped_test_bypass = [bool](Get-NInferQualificationProperty $StateSecurity 'shipped_test_bypass' 'windows_state_security')
        restored_power_limit_w = [int](Get-NInferQualificationProperty $StateSecurity 'restored_power_limit_w' 'windows_state_security')
        receipt_sha256 = Get-NInferQualificationEvidenceSha256 $StateSecurity 'windows_state_security'
    }
    if ($securityStatus -ceq 'passed' -and
        ($securityReceipt.fresh_package_gate_deferred -or
         -not $securityReceipt.root_dacl_protected -or
         $securityReceipt.atomic_race_collision_rejections -lt 1 -or
         $securityReceipt.raced_state_recursive_deletions -ne 0 -or
         $securityReceipt.null_dacl_rejections -lt 1 -or
         $securityReceipt.low_privilege_effective_read_denials -lt 2 -or
         $securityReceipt.low_privilege_effective_write_denials -lt 2 -or
         $securityReceipt.precreated_or_unowned_root_rejections -lt 2 -or
         $securityReceipt.root_or_child_junction_rejections -lt 2 -or
         $securityReceipt.installer_prewrite_root_rejections -lt 3 -or
         $securityReceipt.active_interactive_gpu_rejections -lt 1 -or
         $securityReceipt.request_log_effective_access_denials -lt 2 -or
         $securityReceipt.managed_release_acl_state_assertions -lt 6 -or
         $securityReceipt.managed_request_log_acl_state_assertions -lt 1 -or
         $securityReceipt.managed_rollback_directions -lt 2 -or
         $securityReceipt.retained_state_helper_hash_assertions -lt 4 -or
         $securityReceipt.populated_root_status_milliseconds -le 0 -or
         $securityReceipt.populated_root_status_milliseconds -gt 5000 -or
         $securityReceipt.gpu_power_evidence_class -cne 'real-trusted-absolute-nvidia-smi' -or
         $securityReceipt.absolute_nvidia_shim_interceptions -ne 0 -or
         $securityReceipt.shipped_test_bypass -or
         $securityReceipt.restored_power_limit_w -ne 370)) {
        throw 'windows_state_security does not prove the complete effective-access contract'
    }
    if ($securityStatus -ceq 'not_run' -and
        (-not $securityReceipt.fresh_package_gate_deferred -or
         $securityReceipt.deferred_reason -cne 'fresh-windows-rtx3090-unavailable-after-user-handoff')) {
        throw 'deferred Windows state-security gate lacks the authorized evidence boundary'
    }

    $ompStatus = Get-NInferQualificationStatus $OmpClient 'omp_windows_client'
    $ompReceipt = [ordered]@{
        status = $ompStatus
        omp_version = [string](Get-NInferQualificationProperty $OmpClient 'omp_version' 'omp_windows_client')
        archive_sha256 = Get-NInferQualificationSha256 $OmpClient 'archive_sha256' 'omp_windows_client'
        binary_sha256 = Get-NInferQualificationSha256 $OmpClient 'binary_sha256' 'omp_windows_client'
        events = [int](Get-NInferQualificationProperty $OmpClient 'events' 'omp_windows_client')
        typed_tool_name = [string](Get-NInferQualificationProperty $OmpClient 'typed_tool_name' 'omp_windows_client')
        tool_results = [int](Get-NInferQualificationProperty $OmpClient 'tool_results' 'omp_windows_client')
        exact_final_answer = [bool](Get-NInferQualificationProperty $OmpClient 'exact_final_answer' 'omp_windows_client')
        receipt_sha256 = Get-NInferQualificationEvidenceSha256 $OmpClient 'omp_windows_client'
    }
    if ($ompStatus -ceq 'passed' -and
        ($ompReceipt.omp_version -cne 'omp/18.0.9' -or $ompReceipt.events -le 0 -or
         $ompReceipt.typed_tool_name -cne 'read' -or $ompReceipt.tool_results -lt 1 -or
         -not $ompReceipt.exact_final_answer)) {
        throw 'omp_windows_client does not bind exact typed-tool/final-answer acceptance'
    }

    $disclosureStatus = [string](Get-NInferQualificationProperty $PublicDisclosure 'status' 'public_disclosure')
    if ($disclosureStatus -cnotin @('passed', 'passed_with_classified_generic_references')) {
        throw 'public_disclosure.status is invalid'
    }
    $disclosureReceipt = [ordered]@{
        status = $disclosureStatus
        policy_version = 1
        forbidden_marker_classes = @(
            'private-fleet-identifiers',
            'private-home-paths',
            'credential-material'
        )
        tracked_source_files_scanned = [int](Get-NInferQualificationProperty $PublicDisclosure 'tracked_source_files_scanned' 'public_disclosure')
        tracked_source_private_marker_or_unclassified_credential_findings = [int](Get-NInferQualificationProperty $PublicDisclosure 'tracked_source_private_marker_or_unclassified_credential_findings' 'public_disclosure')
        release_bytes_scanned = [Int64](Get-NInferQualificationProperty $PublicDisclosure 'release_bytes_scanned' 'public_disclosure')
        release_files_scanned = [int](Get-NInferQualificationProperty $PublicDisclosure 'release_files_scanned' 'public_disclosure')
        binary_package_private_path_findings = [int](Get-NInferQualificationProperty $PublicDisclosure 'binary_package_private_path_findings' 'public_disclosure')
        source_archive_generic_path_references = [int](Get-NInferQualificationProperty $PublicDisclosure 'source_archive_generic_path_references' 'public_disclosure')
        classified_private_fleet_or_credential_findings = [int](Get-NInferQualificationProperty $PublicDisclosure 'classified_private_fleet_or_credential_findings' 'public_disclosure')
        raw_scan_sha256 = Get-NInferQualificationSha256 $PublicDisclosure 'raw_scan_sha256' 'public_disclosure'
        private_fleet_projection_included = $false
        credential_values_recorded = 0
    }
    if ($disclosureReceipt.tracked_source_files_scanned -le 0 -or
        $disclosureReceipt.tracked_source_private_marker_or_unclassified_credential_findings -ne 0 -or
        $disclosureReceipt.release_bytes_scanned -le 0 -or
        $disclosureReceipt.release_files_scanned -le 0 -or
        $disclosureReceipt.binary_package_private_path_findings -ne 0 -or
        $disclosureReceipt.classified_private_fleet_or_credential_findings -ne 0 -or
        ($disclosureReceipt.source_archive_generic_path_references -eq 0 -and
         $disclosureStatus -cne 'passed') -or
        ($disclosureReceipt.source_archive_generic_path_references -gt 0 -and
         $disclosureStatus -cne 'passed_with_classified_generic_references')) {
        throw 'public_disclosure does not truthfully classify the final public bytes'
    }

    $statuses = @(
        $protocolStatus,
        $longContextStatus,
        $checkpointStatus,
        $performanceStatus,
        $instrumentedStatus,
        $shippedStatus,
        $assetStatus,
        $securityFixtureStatus,
        $securityStatus,
        $ompStatus
    )
    $allPassed = @($statuses | Where-Object { $_ -cne 'passed' }).Count -eq 0
    $receipt = [ordered]@{
        artifact_type = 'ninfer_rtx3090_beta_qualification'
        schema_version = 3
        status = $(if ($allPassed) { 'passed' } else { 'incomplete' })
        beta_qualified = $allPassed
        qualified_utc = $qualified.ToUniversalTime().ToString('o')
        candidate = $candidateReceipt
        platform = $platformReceipt
        release_assets = $releaseAssetsReceipt
        qualification_authority = [ordered]@{
            authority = $(if ($allPassed) { 'this-exact-beta-receipt' } else { 'pending-deferred-windows-gate' })
            exact_package_sha256 = [string]$releaseAssetsReceipt.package.sha256
            supersession_performed = $allPassed
            supersedes_package_build_receipt_qualification_status = $(if ($allPassed) { $packageBuildStatus } else { $null })
            supersedes_packaged_release_spec_qualification_status = $(if ($allPassed) { $packagedSpecStatus } else { $null })
            supersedes_source_archive_receipt_status = $(if ($allPassed) { 'pending-requalification' } else { $null })
            historical_build_receipts_mutated = $false
            authority_scope = 'hash-bound-public-beta-only'
        }
        qualification = [ordered]@{
            authenticated_agent_protocol = $protocolReceipt
            long_context_64k = $longContextReceipt
            checkpoint_process_restart = $checkpointReceipt
            performance = $performanceReceipt
            windows_lifecycle_instrumented = $instrumentedReceipt
            windows_lifecycle_shipped = $shippedReceipt
            windows_release_assets = $assetReceipt
            windows_state_security_fixture = $securityFixtureReceipt
            windows_state_security = $securityReceipt
            omp_windows_client = $ompReceipt
        }
        public_disclosure = $disclosureReceipt
        scope = [ordered]@{
            supported_clients = @('OMP 18.0.9 Windows x64')
            supported_api_surfaces = @(
                'OpenAI Responses API',
                'Anthropic Messages API',
                'typed tool calls',
                'checkpoint continuation'
            )
            limitations = @(
                'Beta only; no stable or GA promotion.',
                'Single RTX 3090 at cohort 1 is qualified; multi-GPU and higher-concurrency claims are excluded.',
                'CPU, mixed CPU/GPU, and overnight thermal qualification are excluded.',
                'The 64K retrieval gate qualifies 64,512 prompt tokens; it is not a 128K claim.',
                'Responses DELETE is logical object deletion, not secure erasure; a surviving descendant checkpoint may retain ancestor token/KV context required for continuation.'
            )
        }
        automatic_route_activation_allowed = $false
        stable_promotion_performed = $false
        production_route_activation_performed = $false
    }

    $publicText = $receipt | ConvertTo-Json -Depth 24 -Compress
    if ($publicText -match '(?i)(?:[A-Z]:\\\\Users\\\\|/Users/|/home/)' -or
        $publicText -match '(?i)(?:api[_-]?key|bearer|password)["'']?\s*[:=]\s*["''][^"'']+') {
        throw 'qualification receipt contains a private path or credential value'
    }
    return $receipt
}
