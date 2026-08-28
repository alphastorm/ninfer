[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$LongPromptFile,

    [Parameter(Mandatory = $true)]
    [ValidateSet('MTP0', 'MTP3')]
    [string]$Profile,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$GoldenReceiptPath,

    [string]$StateRoot = (Join-Path $env:ProgramData 'NInfer\qwen38-4090'),

    [string]$Python = 'python.exe',

    [string]$ReceiptPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'New-QualificationReceipt.ps1')

Add-Type -AssemblyName System.Web.Extensions
$jsonSerializer = [Web.Script.Serialization.JavaScriptSerializer]::new()
$jsonSerializer.MaxJsonLength = [int]::MaxValue

function Read-JsonFile([string]$Path) {
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Get-Release([object]$State) {
    $property = $State.releases.PSObject.Properties[[string]$State.active_release]
    if ($null -eq $property) { throw 'active release record is missing' }
    return $property.Value
}

function Read-OneLineSecret([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $length = $bytes.Length
    while ($length -gt 0 -and ($bytes[$length - 1] -eq 10 -or $bytes[$length - 1] -eq 13)) {
        $length--
    }
    if ($length -eq 0) { throw 'API-key file is empty' }
    for ($index = 0; $index -lt $length; $index++) {
        if ($bytes[$index] -eq 0 -or $bytes[$index] -eq 10 -or $bytes[$index] -eq 13) {
            throw 'API-key file must contain exactly one non-empty line'
        }
    }
    return [Text.UTF8Encoding]::new($false, $true).GetString($bytes, 0, $length)
}

function Get-Digest([string]$Label) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Label)))
        return $digest.Replace('-', '').ToLowerInvariant()
    }
    finally { $sha.Dispose() }
}

function Invoke-Chat([string]$Uri, [hashtable]$Headers, [object]$Body) {
    [string]$json = $jsonSerializer.Serialize($Body)
    return Invoke-RestMethod -Method Post -Uri $Uri -Headers $Headers -ContentType 'application/json' `
        -Body $json -TimeoutSec 1800 -UseBasicParsing
}

$statePath = Join-Path $StateRoot 'state.json'
$state = Read-JsonFile $statePath
$release = Get-Release $state
$config = Read-JsonFile ([string]$release.config_file)
$controller = Join-Path $StateRoot 'Control-Release.ps1'
$key = Read-OneLineSecret ([string]$release.api_key_file)
$headers = @{ Authorization = "Bearer $key" }
$baseUrl = "http://$($release.host):$($release.port)"

& $controller -Action Restart -StateRoot $StateRoot | Out-Null
$status = Invoke-RestMethod -Method Get -Uri "$baseUrl/v1/ninfer/status" -Headers $headers `
    -TimeoutSec 30 -UseBasicParsing
if ($status.artifact_type -cne 'ninfer_server_status' -or $status.status -cne 'ok') {
    throw 'release status endpoint is not ready for qualification'
}
$expectedBackend = if ($Profile -ceq 'MTP0') { 'none' } else { 'mtp' }
$expectedDraftTokens = if ($Profile -ceq 'MTP0') { 0 } else { 3 }
if ([string]$config.speculative.backend -cne $expectedBackend -or
    [int]$config.speculative.draft_tokens -ne $expectedDraftTokens -or
    [string]$status.runtime.speculative_backend -cne $expectedBackend -or
    [int]$status.runtime.speculative_draft_window -ne $expectedDraftTokens -or
    [bool]$status.mtp.enabled -ne ($Profile -ceq 'MTP3')) {
    throw "installed release does not match the requested $Profile arm"
}
$comparableConfig = $jsonSerializer.DeserializeObject($jsonSerializer.Serialize($config))
$comparableConfig['speculative']['backend'] = '<qualification-arm>'
$comparableConfig['speculative']['draft_tokens'] = -1
$nonspeculativeConfigSha256 = Get-Digest ($jsonSerializer.Serialize($comparableConfig))

$golden = Read-JsonFile (Resolve-Path -LiteralPath $GoldenReceiptPath).Path
if ($golden.artifact_type -cne 'ninfer_golden_private_receipt' -or
    [int]$golden.schema_version -ne 1 -or
    $golden.oracle_passed -ne $true -or
    [int]$golden.exit_code -ne 0 -or
    [double]$golden.wall_seconds -le 0 -or
    [string]$golden.binary_sha256 -cne [string]$release.binary_sha256 -or
    [string]$golden.model_artifact_sha256 -cne [string]$release.model_artifact_sha256 -or
    [string]$golden.config_sha256 -cne [string]$release.config_sha256 -or
    $golden.raw_prompt_or_output_included -ne $false) {
    throw 'Golden t01 receipt failed or does not match the installed release identity'
}

$smokePath = Join-Path $PSScriptRoot 'smoke\agent_protocol.py'
$protocolJson = & $Python $smokePath --base-url $baseUrl --model ([string]$config.model_id) `
    --api-key-file ([string]$release.api_key_file) `
    --expect-binary-sha256 ([string]$release.binary_sha256) `
    --expect-model-artifact-sha256 ([string]$release.model_artifact_sha256) `
    --expect-config-sha256 ([string]$release.config_sha256) `
    --expect-deployment-profile ([string]$release.deployment_profile)
if ($LASTEXITCODE -ne 0) { throw 'agent protocol smoke failed' }
$protocol = ($protocolJson | Out-String) | ConvertFrom-Json
if ($protocol.artifact_type -cne 'ninfer_agent_protocol_smoke' -or
    [int]$protocol.schema_version -ne 1 -or $protocol.status -cne 'passed') {
    throw 'agent protocol receipt envelope mismatch'
}

$longPromptPath = (Resolve-Path -LiteralPath $LongPromptFile).Path
$longPrompt = [IO.File]::ReadAllText($longPromptPath, [Text.UTF8Encoding]::new($false, $true))
if ([string]::IsNullOrWhiteSpace($longPrompt)) { throw 'long prompt corpus is empty' }
$digestPrefix = "ninfer-qwen38-4090-v0.1/$($Profile.ToLowerInvariant())"
$sessionDigest = Get-Digest "$digestPrefix/qualification-session"
$firstRequestDigest = Get-Digest "$digestPrefix/qualification-first"
$secondRequestDigest = Get-Digest "$digestPrefix/qualification-post-restart"
$responsesUri = "$baseUrl/v1/responses"
$firstBody = [ordered]@{
    model = [string]$config.model_id
    input = $longPrompt
    max_output_tokens = 16
    temperature = 0
    store = $true
    stream = $false
    ninfer_session = $sessionDigest
    ninfer_request_id = $firstRequestDigest
}
$firstStarted = [DateTime]::UtcNow
$first = Invoke-Chat $responsesUri $headers $firstBody
$firstFinished = [DateTime]::UtcNow
if ([int]$first.usage.input_tokens -lt 100000) {
    throw 'long qualification prompt did not reach 100000 input tokens'
}

& $controller -Action Restart -StateRoot $StateRoot | Out-Null
$secondBody = [ordered]@{
    model = [string]$config.model_id
    previous_response_id = [string]$first.id
    input = 'Continue with one short sentence.'
    max_output_tokens = 16
    temperature = 0
    store = $true
    stream = $false
    ninfer_session = $sessionDigest
    ninfer_request_id = $secondRequestDigest
}
$secondStarted = [DateTime]::UtcNow
$second = Invoke-Chat $responsesUri $headers $secondBody
$secondFinished = [DateTime]::UtcNow

$logPath = Join-Path ([string]$release.release_root) 'logs\requests.jsonl'
$matchingRecord = $null
foreach ($line in @(Get-Content -LiteralPath $logPath -Tail 256 -Encoding UTF8)) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $record = $line | ConvertFrom-Json
    if ($record.event -ceq 'request_done' -and
        [string]$record.request.client_identity.request_sha256 -ceq $secondRequestDigest) {
        $matchingRecord = $record
    }
}
if ($null -eq $matchingRecord) { throw 'post-restart request receipt is missing from JSONL' }
if ($matchingRecord.result.prefix_reuse_path -cne 'restore_disk_checkpoint' -or
    [int]$matchingRecord.result.prefix_cache_hit_tokens -lt 100000) {
    throw 'post-restart request did not restore the persistent checkpoint'
}

if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $receiptDirectory = Join-Path $StateRoot 'receipts'
    New-Item -ItemType Directory -Force -Path $receiptDirectory | Out-Null
    $ReceiptPath = Join-Path $receiptDirectory (
        "qualification-$($Profile.ToLowerInvariant())-" + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '.json'
    )
}
$receiptIdentity = [ordered]@{}
foreach ($property in $status.identity.PSObject.Properties) { $receiptIdentity[$property.Name] = $property.Value }
$receiptIdentity.gpu_index = [int]$release.gpu_index
$receiptIdentity.gpu_uuid = [string]$release.gpu_uuid
$receiptIdentity.gpu_name = [string]$release.gpu_name
$receiptParameters = @{
    QualifiedUtc = [DateTime]::UtcNow.ToString('o')
    ReleaseId = [string]$state.active_release
    Profile = $Profile
    Identity = $receiptIdentity
    Configuration = [ordered]@{
        public_model_id = [string]$config.model_id
        max_context = [int]$config.engine.max_context
        kv_capacity = [int]$config.engine.kv_capacity
        kv_dtype = [string]$config.engine.kv_dtype
        prefill_chunk = [int]$config.engine.prefill_chunk
        speculative_backend = [string]$config.speculative.backend
        speculative_draft_tokens = [int]$config.speculative.draft_tokens
        reasoning_effort = [string]$config.reasoning.effort
        disk_cache_gib = [int]$config.persistent_cache.quota_gib
        concurrency = [int]$config.engine.max_concurrency
    }
    NonspeculativeConfigSha256 = $nonspeculativeConfigSha256
    Protocol = $protocol
    LongSession = [ordered]@{
        first_prompt_tokens = [int]$first.usage.input_tokens
        first_completion_tokens = [int]$first.usage.output_tokens
        first_elapsed_seconds = ($firstFinished - $firstStarted).TotalSeconds
        post_restart_prompt_tokens = [int]$second.usage.input_tokens
        post_restart_completion_tokens = [int]$second.usage.output_tokens
        post_restart_elapsed_seconds = ($secondFinished - $secondStarted).TotalSeconds
    }
    Persistence = [ordered]@{
        restored_tokens = [int]$matchingRecord.result.prefix_cache_hit_tokens
        reuse_path = [string]$matchingRecord.result.prefix_reuse_path
        post_restart_prepare_seconds = [double]$matchingRecord.timings_seconds.prepare
    }
    GoldenT01 = $golden
    DeterministicGates = [ordered]@{
        protocol = 'passed'
        long_session = 'passed'
        persistence = 'passed'
        golden_oracle = 'passed'
    }
}
$receipt = New-NInferQualificationReceipt @receiptParameters
[IO.File]::WriteAllText(
    $ReceiptPath,
    ($receipt | ConvertTo-Json -Depth 20),
    [Text.UTF8Encoding]::new($false)
)
$receipt | ConvertTo-Json -Depth 20
