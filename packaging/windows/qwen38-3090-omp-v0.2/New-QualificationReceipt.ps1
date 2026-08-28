function Get-NInferQualificationGate {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][object]$Gate
    )

    $status = [string]$Gate.status
    if ($status -cnotin @('passed', 'failed', 'not_run')) {
        throw "$Name gate status must be passed, failed, or not_run"
    }
    $evidence = [string]$Gate.evidence_sha256
    if ($status -ceq 'passed' -and $evidence -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Name gate evidence must be a lower-case SHA-256 when passed"
    }
    if ($status -cne 'passed' -and -not [string]::IsNullOrEmpty($evidence) -and
        $evidence -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Name gate evidence must be empty or a lower-case SHA-256"
    }
    return [ordered]@{
        status = $status
        evidence_sha256 = if ([string]::IsNullOrEmpty($evidence)) { $null } else { $evidence }
    }
}

function Get-NInferQualificationSha {
    param([Parameter(Mandatory = $true)][object]$Object, [Parameter(Mandatory = $true)][string]$Name)
    $value = [string]$Object.$Name
    if ($value -cnotmatch '^[0-9a-f]{64}$') {
        throw "identity.$Name must be a lower-case SHA-256"
    }
    return $value
}

function New-NInferQualificationReceipt {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$QualifiedUtc,
        [Parameter(Mandatory = $true)][string]$ReleaseId,
        [Parameter(Mandatory = $true)][object]$Identity,
        [Parameter(Mandatory = $true)][object]$Configuration,
        [Parameter(Mandatory = $true)][object]$Hardware,
        [Parameter(Mandatory = $true)][object]$GpuThermal,
        [Parameter(Mandatory = $true)][object]$Protocol,
        [Parameter(Mandatory = $true)][object]$LongContext,
        [Parameter(Mandatory = $true)][object]$CheckpointRestart,
        [Parameter(Mandatory = $true)][object]$RoleCorpus,
        [Parameter(Mandatory = $true)][object]$Performance,
        [Parameter(Mandatory = $true)][object]$Lifecycle
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
    foreach ($name in @('release_head_sha', 'upstream_base_sha', 'lineage_base_sha')) {
        $value = [string]$Identity.$name
        if ($value -cnotmatch '^[0-9a-f]{40}$') {
            throw "identity.$name must be a full lower-case Git SHA"
        }
    }
    if ([string]$Identity.build_profile -cne 'omp-v0.2.0-rtx3090' -or
        [string]$Identity.cuda_architecture -cne 'sm_86') {
        throw 'qualification identity does not match the immutable RTX 3090 build profile'
    }
    $identityReceipt = [ordered]@{
        release_head_sha = [string]$Identity.release_head_sha
        upstream_base_sha = [string]$Identity.upstream_base_sha
        lineage_base_sha = [string]$Identity.lineage_base_sha
        build_profile = [string]$Identity.build_profile
        cuda_architecture = [string]$Identity.cuda_architecture
        ninfer_sha256 = Get-NInferQualificationSha $Identity 'ninfer_sha256'
        server_sha256 = Get-NInferQualificationSha $Identity 'server_sha256'
        benchmark_sha256 = Get-NInferQualificationSha $Identity 'benchmark_sha256'
        config_sha256 = Get-NInferQualificationSha $Identity 'config_sha256'
        model_sha256 = Get-NInferQualificationSha $Identity 'model_sha256'
        package_sha256 = Get-NInferQualificationSha $Identity 'package_sha256'
        checksums_sha256 = Get-NInferQualificationSha $Identity 'checksums_sha256'
    }
    if ([string]$Configuration.deployment_profile -cne 'qwen38-3090-omp-v0.2.0-c1' -or
        [int]$Configuration.max_context -ne 65536 -or
        [string]$Configuration.kv_capacity -cne 'auto' -or
        [string]$Configuration.kv_dtype -cne 'int8' -or
        [int]$Configuration.prefill_chunk -ne 1024 -or
        [int]$Configuration.concurrency -ne 1 -or
        [string]$Configuration.speculative_backend -cne 'mtp' -or
        [int]$Configuration.speculative_draft_tokens -ne 3 -or
        [string]$Configuration.reasoning_effort -cne 'xhigh' -or
        [int]$Configuration.checkpoint_quota_mib -ne 65536 -or
        [int]$Configuration.checkpoint_staging_mib -ne 256) {
        throw 'qualification configuration does not match the immutable C1 profile'
    }
    $hardwareReceipt = [ordered]@{
        gpu_name = [string]$Hardware.gpu_name
        gpu_uuid = [string]$Hardware.gpu_uuid
        driver_version = [string]$Hardware.driver_version
        compute_capability = [string]$Hardware.compute_capability
    }
    if ($hardwareReceipt.gpu_name -cne 'NVIDIA GeForce RTX 3090' -or
        $hardwareReceipt.compute_capability -cne '8.6' -or
        [string]::IsNullOrWhiteSpace($hardwareReceipt.gpu_uuid) -or
        [string]::IsNullOrWhiteSpace($hardwareReceipt.driver_version)) {
        throw 'qualification hardware does not identify the bound RTX 3090'
    }

    $gates = [ordered]@{
        gpu_only_thermal_envelope = Get-NInferQualificationGate 'GPU-only thermal envelope' $GpuThermal
        sm86_driver_hardware_identity = Get-NInferQualificationGate 'sm86 driver hardware identity' $Hardware
        authenticated_real_client_protocol = Get-NInferQualificationGate 'authenticated real-client protocol' $Protocol
        advertised_context_retrieval = Get-NInferQualificationGate 'advertised context retrieval' $LongContext
        process_restart_session_continuation = Get-NInferQualificationGate 'process-restart session continuation' $CheckpointRestart
        frozen_role_corpus = Get-NInferQualificationGate 'frozen role corpus' $RoleCorpus
        bounded_gpu_performance_at_qualified_cap = Get-NInferQualificationGate 'bounded GPU performance at qualified cap' $Performance
        gaming_drain_restart_rollback = Get-NInferQualificationGate 'gaming drain/restart/rollback' $Lifecycle
    }
    $allPassed = $true
    foreach ($gate in $gates.Values) {
        if ([string]$gate.status -cne 'passed') { $allPassed = $false }
    }

    return [ordered]@{
        artifact_type = 'ninfer_windows_release_qualification'
        schema_version = 2
        status = if ($allPassed) { 'passed' } else { 'incomplete' }
        qualified_utc = $qualified.ToUniversalTime().ToString('o')
        release_id = $ReleaseId
        identity = $identityReceipt
        configuration = [ordered]@{
            deployment_profile = [string]$Configuration.deployment_profile
            max_context = [int]$Configuration.max_context
            kv_capacity = [string]$Configuration.kv_capacity
            kv_dtype = [string]$Configuration.kv_dtype
            prefill_chunk = [int]$Configuration.prefill_chunk
            concurrency = [int]$Configuration.concurrency
            speculative_backend = [string]$Configuration.speculative_backend
            speculative_draft_tokens = [int]$Configuration.speculative_draft_tokens
            reasoning_effort = [string]$Configuration.reasoning_effort
            checkpoint_quota_mib = [int]$Configuration.checkpoint_quota_mib
            checkpoint_staging_mib = [int]$Configuration.checkpoint_staging_mib
        }
        hardware = $hardwareReceipt
        gates = $gates
    }
}
