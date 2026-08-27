[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ComparisonPath,

    [Parameter(Mandatory = $true)]
    [string]$ReceiptConstructorPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ComparisonPath = (Resolve-Path -LiteralPath $ComparisonPath).Path
$ReceiptConstructorPath = (Resolve-Path -LiteralPath $ReceiptConstructorPath).Path
. $ReceiptConstructorPath

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

function New-Receipt([string]$Profile, [double]$WallSeconds) {
    $mtp3 = $Profile -ceq 'MTP3'
    $parameters = @{
        QualifiedUtc = '2026-08-26T00:00:00Z'
        ReleaseId = 'fixture-release'
        Profile = $Profile
        Identity = [ordered]@{
            binary_sha256 = ('1' * 64)
            model_artifact_sha256 = ('2' * 64)
            config_sha256 = if ($mtp3) { ('3' * 64) } else { ('4' * 64) }
        }
        Configuration = [ordered]@{
            public_model_id = 'qwen3.8-27b'
            max_context = 131072
            kv_capacity = 131072
            kv_dtype = 'rk2v4-e8'
            prefill_chunk = 512
            speculative_backend = if ($mtp3) { 'mtp' } else { 'none' }
            speculative_draft_tokens = if ($mtp3) { 3 } else { 0 }
            reasoning_effort = 'xhigh'
            disk_cache_gib = 100
            concurrency = 1
        }
        NonspeculativeConfigSha256 = ('5' * 64)
        Protocol = [ordered]@{ status = 'passed' }
        LongSession = [ordered]@{ fixture = $true }
        Persistence = [ordered]@{
            exact = $true
            reuse_path = 'restore_disk_checkpoint'
            restored_tokens = 105000
        }
        GoldenT01 = [ordered]@{
            oracle_passed = $true
            exit_code = 0
            wall_seconds = $WallSeconds
        }
        DeterministicGates = [ordered]@{
            protocol = 'passed'
            long_session = 'passed'
            persistence = 'passed'
            golden_oracle = 'passed'
            malformed_tool_or_final_output = $false
        }
    }
    return New-NInferQualificationReceipt @parameters
}

$root = Join-Path ([IO.Path]::GetTempPath()) ('ninfer-mtp-decision-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$mtp0Path = Join-Path $root 'mtp0.json'
$mtp3Path = Join-Path $root 'mtp3.json'

try {
    $mtp0 = New-Receipt 'MTP0' 100.0
    $mtp3 = New-Receipt 'MTP3' 89.999
    Assert-True ([string]$mtp0.configuration.nonspeculative_config_sha256 -ceq ('5' * 64)) 'production receipt constructor omitted the canonical non-speculative identity'
    Assert-True (-not $mtp0.Contains('nonspeculative_config_sha256')) 'production receipt constructor emitted a top-level non-speculative identity alias'
    Write-Json $mtp0Path $mtp0
    Write-Json $mtp3Path $mtp3
    $decision = ((& $ComparisonPath -Mtp0ReceiptPath $mtp0Path -Mtp3ReceiptPath $mtp3Path) | Out-String) | ConvertFrom-Json
    Assert-True ([bool]$decision.promote_mtp3) 'MTP3 did not promote above the 10% threshold'
    Assert-True ([string]$decision.selected_profile -ceq 'MTP3') 'promoted profile is not MTP3'

    $mtp3.golden_t01.wall_seconds = 90.001
    Write-Json $mtp3Path $mtp3
    $decision = ((& $ComparisonPath -Mtp0ReceiptPath $mtp0Path -Mtp3ReceiptPath $mtp3Path) | Out-String) | ConvertFrom-Json
    Assert-True (-not [bool]$decision.promote_mtp3) 'MTP3 promoted below the 10% threshold'
    Assert-True ([string]$decision.selected_profile -ceq 'MTP0') 'sub-threshold decision did not retain MTP0'

    $mtp3.golden_t01.wall_seconds = 80.0
    $mtp3.persistence.exact = $false
    Write-Json $mtp3Path $mtp3
    $decision = ((& $ComparisonPath -Mtp0ReceiptPath $mtp0Path -Mtp3ReceiptPath $mtp3Path) | Out-String) | ConvertFrom-Json
    Assert-True (-not [bool]$decision.promote_mtp3) 'inexact persistence promoted MTP3'
    Assert-True (-not [bool]$decision.arms.MTP3.deterministic_gates_passed) 'inexact persistence passed deterministic gates'

    $mtp3.persistence.exact = $true
    $mtp3.configuration.prefill_chunk = 1024
    Write-Json $mtp3Path $mtp3
    $rejected = $false
    try {
        & $ComparisonPath -Mtp0ReceiptPath $mtp0Path -Mtp3ReceiptPath $mtp3Path | Out-Null
    }
    catch {
        $rejected = $_.Exception.Message -like '*differ outside speculation: prefill_chunk*'
    }
    Assert-True $rejected 'non-speculative configuration drift was not rejected'

    $mtp3.configuration.prefill_chunk = 512
    $mtp3.configuration.nonspeculative_config_sha256 = ('6' * 64)
    Write-Json $mtp3Path $mtp3
    $rejected = $false
    try {
        & $ComparisonPath -Mtp0ReceiptPath $mtp0Path -Mtp3ReceiptPath $mtp3Path | Out-Null
    }
    catch {
        $rejected = $_.Exception.Message -like '*differ outside speculation: nonspeculative_config_sha256*'
    }
    Assert-True $rejected 'unreported non-speculative configuration drift was not rejected'

    $invalidRequiredIdentities = @(
        [ordered]@{ group = 'identity'; field = 'binary_sha256'; mode = 'missing' },
        [ordered]@{ group = 'identity'; field = 'binary_sha256'; mode = 'malformed' },
        [ordered]@{ group = 'identity'; field = 'model_artifact_sha256'; mode = 'missing' },
        [ordered]@{ group = 'identity'; field = 'model_artifact_sha256'; mode = 'malformed' },
        [ordered]@{ group = 'configuration'; field = 'nonspeculative_config_sha256'; mode = 'missing' },
        [ordered]@{ group = 'configuration'; field = 'nonspeculative_config_sha256'; mode = 'malformed' }
    )
    foreach ($invalidIdentity in $invalidRequiredIdentities) {
        $invalidMtp0 = New-Receipt 'MTP0' 100.0
        $invalidMtp3 = New-Receipt 'MTP3' 80.0
        foreach ($receipt in @($invalidMtp0, $invalidMtp3)) {
            if ($invalidIdentity.mode -ceq 'missing') {
                $receipt[$invalidIdentity.group].Remove([string]$invalidIdentity.field)
            }
            else {
                $receipt[$invalidIdentity.group][$invalidIdentity.field] = ('A' * 64)
            }
        }
        Write-Json $mtp0Path $invalidMtp0
        Write-Json $mtp3Path $invalidMtp3
        $requiredIdentityRejected = $false
        $expectedIdentityField = "$($invalidIdentity.group).$($invalidIdentity.field)"
        try {
            & $ComparisonPath -Mtp0ReceiptPath $mtp0Path -Mtp3ReceiptPath $mtp3Path | Out-Null
        }
        catch {
            $requiredIdentityRejected = $_.Exception.Message -like "*$expectedIdentityField must be a lower-case SHA-256*"
        }
        Assert-True $requiredIdentityRejected "equal $($invalidIdentity.mode) required identity was accepted: $expectedIdentityField"
    }

    [ordered]@{
        artifact_type = 'ninfer_mtp_decision_regression'
        schema_version = 1
        status = 'passed'
        cases = 12
    } | ConvertTo-Json -Compress
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
