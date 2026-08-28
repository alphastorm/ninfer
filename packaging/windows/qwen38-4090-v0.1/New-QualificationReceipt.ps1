function New-NInferQualificationReceipt {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$QualifiedUtc,

        [Parameter(Mandatory = $true)]
        [string]$ReleaseId,

        [Parameter(Mandatory = $true)]
        [ValidateSet('MTP0', 'MTP3')]
        [string]$Profile,

        [Parameter(Mandatory = $true)]
        [object]$Identity,

        [Parameter(Mandatory = $true)]
        [object]$Configuration,

        [Parameter(Mandatory = $true)]
        [string]$NonspeculativeConfigSha256,

        [Parameter(Mandatory = $true)]
        [object]$Protocol,

        [Parameter(Mandatory = $true)]
        [object]$LongSession,

        [Parameter(Mandatory = $true)]
        [object]$Persistence,

        [Parameter(Mandatory = $true)]
        [object]$GoldenEquivalent,

        [Parameter(Mandatory = $true)]
        [object]$DeterministicGates
    )

    if ($NonspeculativeConfigSha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw 'nonspeculative configuration identity must be a lower-case SHA-256'
    }

    return [ordered]@{
        artifact_type = 'ninfer_windows_release_qualification'
        schema_version = 1
        status = 'passed'
        qualified_utc = $QualifiedUtc
        release_id = $ReleaseId
        profile = $Profile
        identity = $Identity
        configuration = [ordered]@{
            public_model_id = [string]$Configuration.public_model_id
            max_context = [int]$Configuration.max_context
            kv_capacity = [int]$Configuration.kv_capacity
            kv_dtype = [string]$Configuration.kv_dtype
            prefill_chunk = [int]$Configuration.prefill_chunk
            speculative_backend = [string]$Configuration.speculative_backend
            speculative_draft_tokens = [int]$Configuration.speculative_draft_tokens
            reasoning_effort = [string]$Configuration.reasoning_effort
            disk_cache_gib = [int]$Configuration.disk_cache_gib
            concurrency = [int]$Configuration.concurrency
            nonspeculative_config_sha256 = $NonspeculativeConfigSha256
        }
        protocol = $Protocol
        long_session = $LongSession
        persistence = $Persistence
        golden_equivalent = $GoldenEquivalent
        deterministic_gates = $DeterministicGates
    }
}
