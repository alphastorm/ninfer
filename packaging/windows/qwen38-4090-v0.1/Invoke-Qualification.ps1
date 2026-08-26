[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string]$LongPromptFile,

    [string]$StateRoot = (Join-Path $env:ProgramData 'NInfer\qwen38-4090'),

    [string]$Python = 'python.exe',

    [string]$ReceiptPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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
        return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Label)))) `
            .Replace('-', '').ToLowerInvariant()
    }
    finally { $sha.Dispose() }
}

function Invoke-Chat([string]$Uri, [hashtable]$Headers, [object]$Body) {
    $json = $Body | ConvertTo-Json -Depth 20 -Compress
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

$smokePath = Join-Path ([string]$release.release_root) 'smoke\agent_protocol.py'
$protocolJson = & $Python $smokePath --base-url $baseUrl --model ([string]$config.model_id) `
    --api-key-file ([string]$release.api_key_file) `
    --expect-binary-sha256 ([string]$release.binary_sha256) `
    --expect-model-artifact-sha256 ([string]$release.model_sha256) `
    --expect-config-sha256 ([string]$release.config_sha256) `
    --expect-deployment-profile ([string]$release.deployment_profile)
if ($LASTEXITCODE -ne 0) { throw 'agent protocol smoke failed' }
$protocol = ($protocolJson | Out-String) | ConvertFrom-Json

$longPrompt = Get-Content -LiteralPath (Resolve-Path -LiteralPath $LongPromptFile).Path -Raw -Encoding UTF8
if ([string]::IsNullOrWhiteSpace($longPrompt)) { throw 'long prompt corpus is empty' }
$sessionDigest = Get-Digest 'ninfer-qwen38-4090-v0.1/qualification-session'
$firstRequestDigest = Get-Digest 'ninfer-qwen38-4090-v0.1/qualification-first'
$secondRequestDigest = Get-Digest 'ninfer-qwen38-4090-v0.1/qualification-post-restart'
$chatUri = "$baseUrl/v1/chat/completions"
$firstBody = [ordered]@{
    model = [string]$config.model_id
    messages = @([ordered]@{ role = 'user'; content = $longPrompt })
    max_completion_tokens = 16
    temperature = 0
    stream = $false
    ninfer_session = $sessionDigest
    ninfer_request_id = $firstRequestDigest
}
$firstStarted = [DateTime]::UtcNow
$first = Invoke-Chat $chatUri $headers $firstBody
$firstFinished = [DateTime]::UtcNow
if ([int]$first.usage.prompt_tokens -lt 100000) {
    throw 'long qualification prompt did not reach 100000 input tokens'
}
$assistant = $first.choices[0].message
$assistantTurn = [ordered]@{ role = 'assistant'; content = $assistant.content }
$reasoningProperty = $assistant.PSObject.Properties['reasoning_content']
if ($null -ne $reasoningProperty -and
    -not [string]::IsNullOrEmpty([string]$reasoningProperty.Value)) {
    $assistantTurn.reasoning_content = [string]$reasoningProperty.Value
}

& $controller -Action Restart -StateRoot $StateRoot | Out-Null
$secondBody = [ordered]@{
    model = [string]$config.model_id
    messages = @(
        [ordered]@{ role = 'user'; content = $longPrompt },
        $assistantTurn,
        [ordered]@{ role = 'user'; content = 'Continue with one short sentence.' }
    )
    max_completion_tokens = 16
    temperature = 0
    stream = $false
    ninfer_session = $sessionDigest
    ninfer_request_id = $secondRequestDigest
}
$secondStarted = [DateTime]::UtcNow
$second = Invoke-Chat $chatUri $headers $secondBody
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
        'qualification-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '.json'
    )
}
$receipt = [ordered]@{
    artifact_type = 'ninfer_windows_release_qualification'
    schema_version = 1
    status = 'passed'
    qualified_utc = [DateTime]::UtcNow.ToString('o')
    release_id = [string]$state.active_release
    identity = $status.identity
    configuration = [ordered]@{
        public_model_id = [string]$config.model_id
        max_context = [int]$config.engine.max_context
        kv_dtype = [string]$config.engine.kv_dtype
        speculative_backend = [string]$config.speculative.backend
        reasoning_effort = [string]$config.reasoning.effort
        disk_cache_gib = [int]$config.persistent_cache.quota_gib
    }
    protocol = $protocol
    persistence = [ordered]@{
        first_prompt_tokens = [int]$first.usage.prompt_tokens
        first_completion_tokens = [int]$first.usage.completion_tokens
        first_elapsed_seconds = ($firstFinished - $firstStarted).TotalSeconds
        post_restart_prompt_tokens = [int]$second.usage.prompt_tokens
        post_restart_completion_tokens = [int]$second.usage.completion_tokens
        post_restart_elapsed_seconds = ($secondFinished - $secondStarted).TotalSeconds
        restored_tokens = [int]$matchingRecord.result.prefix_cache_hit_tokens
        reuse_path = [string]$matchingRecord.result.prefix_reuse_path
        post_restart_prepare_seconds = [double]$matchingRecord.timings_seconds.prepare
    }
}
[IO.File]::WriteAllText(
    $ReceiptPath,
    ($receipt | ConvertTo-Json -Depth 20),
    [Text.UTF8Encoding]::new($false)
)
$receipt | ConvertTo-Json -Depth 20
