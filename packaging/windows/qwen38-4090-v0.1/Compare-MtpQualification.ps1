[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$Mtp0ReceiptPath,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$Mtp3ReceiptPath,

    [string]$DecisionPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-Receipt([string]$Path, [string]$ExpectedProfile) {
    $receipt = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($receipt.artifact_type -cne 'ninfer_windows_release_qualification' -or
        [int]$receipt.schema_version -ne 1 -or
        [string]$receipt.profile -cne $ExpectedProfile) {
        throw "$ExpectedProfile qualification receipt envelope mismatch"
    }
    return $receipt
}


function Get-RequiredLowerSha256(
    [object]$Receipt,
    [string]$ExpectedProfile,
    [string]$Group,
    [string]$Field
) {
    $groupProperty = $Receipt.PSObject.Properties[$Group]
    $fieldProperty = if ($null -eq $groupProperty -or $null -eq $groupProperty.Value) {
        $null
    }
    else {
        $groupProperty.Value.PSObject.Properties[$Field]
    }
    $value = if ($null -eq $fieldProperty) { '' } else { [string]$fieldProperty.Value }
    if ($value -cnotmatch '^[0-9a-f]{64}$') {
        throw "$ExpectedProfile qualification receipt $Group.$Field must be a lower-case SHA-256"
    }
    return $value
}
function Test-DeterministicGates([object]$Receipt) {
    try {
        return (
            [string]$Receipt.status -ceq 'passed' -and
            [string]$Receipt.protocol.status -ceq 'passed' -and
            [string]$Receipt.deterministic_gates.protocol -ceq 'passed' -and
            [string]$Receipt.deterministic_gates.long_session -ceq 'passed' -and
            [string]$Receipt.deterministic_gates.persistence -ceq 'passed' -and
            [string]$Receipt.deterministic_gates.golden_equivalent -ceq 'passed' -and
            ([string]$Receipt.persistence.reuse_path -cin @('restore_disk_checkpoint', 'append_frontier')) -and
            [bool]$Receipt.persistence.server_instance_replaced -and
            [int]$Receipt.persistence.restored_tokens -ge 100000 -and
            [string]$Receipt.golden_equivalent.status -ceq 'passed' -and
            [double]$Receipt.golden_equivalent.omp.wall_seconds -gt 0 -and
            $Receipt.golden_equivalent.contract.historical_private_corpus_reused -eq $false
        )
    }
    catch {
        return $false
    }
}

$mtp0 = Read-Receipt (Resolve-Path -LiteralPath $Mtp0ReceiptPath).Path 'MTP0'
$mtp3 = Read-Receipt (Resolve-Path -LiteralPath $Mtp3ReceiptPath).Path 'MTP3'

foreach ($field in @('binary_sha256', 'model_artifact_sha256')) {
    $mtp0Value = Get-RequiredLowerSha256 $mtp0 'MTP0' 'identity' $field
    $mtp3Value = Get-RequiredLowerSha256 $mtp3 'MTP3' 'identity' $field
    if ($mtp0Value -cne $mtp3Value) {
        throw "qualification arms use different identity field: $field"
    }
}
$mtp0NonspeculativeConfigSha256 = Get-RequiredLowerSha256 $mtp0 'MTP0' 'configuration' 'nonspeculative_config_sha256'
$mtp3NonspeculativeConfigSha256 = Get-RequiredLowerSha256 $mtp3 'MTP3' 'configuration' 'nonspeculative_config_sha256'
if ($mtp0NonspeculativeConfigSha256 -cne $mtp3NonspeculativeConfigSha256) {
    throw 'qualification arms differ outside speculation: nonspeculative_config_sha256'
}
foreach ($field in @(
        'public_model_id', 'max_context', 'kv_capacity', 'kv_dtype', 'prefill_chunk',
        'reasoning_effort', 'disk_cache_gib', 'concurrency'
    )) {
    if ([string]$mtp0.configuration.$field -cne [string]$mtp3.configuration.$field) {
        throw "qualification arms differ outside speculation: $field"
    }
}
if ([string]$mtp0.configuration.speculative_backend -cne 'none' -or
    [int]$mtp0.configuration.speculative_draft_tokens -ne 0 -or
    [string]$mtp3.configuration.speculative_backend -cne 'mtp' -or
    [int]$mtp3.configuration.speculative_draft_tokens -ne 3) {
    throw 'qualification arms are not the required MTP0 versus MTP3 pair'
}

$mtp0Passed = Test-DeterministicGates $mtp0
$mtp3Passed = Test-DeterministicGates $mtp3
$mtp0Wall = if ($mtp0Passed) { [double]$mtp0.golden_equivalent.omp.wall_seconds } else { $null }
$mtp3Wall = if ($mtp3Passed) { [double]$mtp3.golden_equivalent.omp.wall_seconds } else { $null }
$improvement = $null
if ($null -ne $mtp0Wall -and $null -ne $mtp3Wall) {
    $improvement = (($mtp0Wall - $mtp3Wall) / $mtp0Wall) * 100.0
}
$promote = $mtp0Passed -and $mtp3Passed -and $null -ne $improvement -and $improvement -ge 10.0

$reason = if ($promote) {
    'MTP3 passed every deterministic, output, persistence, and Golden-equivalent gate and improved complete Golden-equivalent wall time by at least 10%.'
}
elseif (-not $mtp0Passed) {
    'MTP0 baseline gates did not all pass; no speculative promotion is permitted.'
}
elseif (-not $mtp3Passed) {
    'MTP3 did not pass every deterministic, output, persistence, and Golden-equivalent gate.'
}
else {
    'MTP3 complete Golden-equivalent wall-time improvement was below 10%.'
}

$decision = [ordered]@{
    artifact_type = 'ninfer_mtp_requalification_decision'
    schema_version = 1
    status = 'closed'
    decided_utc = [DateTime]::UtcNow.ToString('o')
    selected_profile = if ($promote) { 'MTP3' } else { 'MTP0' }
    promote_mtp3 = $promote
    release_eligible = $mtp0Passed
    threshold_complete_golden_equivalent_wall_time_percent = 10.0
    arms = [ordered]@{
        MTP0 = [ordered]@{
            deterministic_gates_passed = $mtp0Passed
            complete_golden_equivalent_wall_seconds = $mtp0Wall
        }
        MTP3 = [ordered]@{
            deterministic_gates_passed = $mtp3Passed
            complete_golden_equivalent_wall_seconds = $mtp3Wall
        }
    }
    complete_golden_equivalent_wall_time_improvement_percent = if ($null -eq $improvement) {
        $null
    }
    else {
        [Math]::Round($improvement, 6)
    }
    reason = $reason
}

if (-not [string]::IsNullOrWhiteSpace($DecisionPath)) {
    $temporary = "$DecisionPath.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText(
            $temporary,
            ($decision | ConvertTo-Json -Depth 16),
            [Text.UTF8Encoding]::new($false)
        )
        Move-Item -LiteralPath $temporary -Destination $DecisionPath -Force
    }
    finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}
$decision | ConvertTo-Json -Depth 16
