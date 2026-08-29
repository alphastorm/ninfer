[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallerPath,

    [Parameter(Mandatory = $true)]
    [string]$ControllerPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$InstallerPath = (Resolve-Path -LiteralPath $InstallerPath).Path
$ControllerPath = (Resolve-Path -LiteralPath $ControllerPath).Path
$publishedInstallerPath = $InstallerPath
$publishedControllerPath = $ControllerPath
$global:NInferTestTaskExists = $false
$global:NInferTestTaskRunning = $false
$global:NInferTestTaskXml = $null
$global:NInferTestTaskSerial = 0
$global:NInferTestStateRoot = $null
$global:NInferTestDeadReleaseId = $null
$global:NInferTestDeadStartSleptMilliseconds = 0
$global:NInferTestAclFailure = $false
$global:NInferAclObservations = [Collections.Generic.List[object]]::new()
$global:NInferTestAclResetArguments = @()

function global:Get-ScheduledTask {
    param([string]$TaskName, [object]$ErrorAction)
    if (-not $global:NInferTestTaskExists) { return $null }
    return [pscustomobject]@{
        TaskName = $TaskName
        State = if ($global:NInferTestTaskRunning) { 'Running' } else { 'Ready' }
    }
}

function global:Export-ScheduledTask {
    param([string]$TaskName)
    if (-not $global:NInferTestTaskExists) { throw 'mock task is missing' }
    return [string]$global:NInferTestTaskXml
}

function global:New-ScheduledTaskAction {
    param([string]$Execute, [string]$Argument)
    return [pscustomobject]@{ Execute = $Execute; Argument = $Argument }
}

function global:New-ScheduledTaskTrigger {
    param([switch]$AtStartup)
    return [pscustomobject]@{ AtStartup = [bool]$AtStartup }
}

function global:New-ScheduledTaskSettingsSet {
    param(
        [switch]$AllowStartIfOnBatteries,
        [switch]$DontStopIfGoingOnBatteries,
        [TimeSpan]$ExecutionTimeLimit,
        [string]$MultipleInstances,
        [int]$RestartCount,
        [TimeSpan]$RestartInterval,
        [switch]$StartWhenAvailable
    )
    return [pscustomobject]@{ MultipleInstances = $MultipleInstances }
}

function global:New-ScheduledTaskPrincipal {
    param([string]$UserId, [string]$LogonType, [string]$RunLevel)
    return [pscustomobject]@{ UserId = $UserId; LogonType = $LogonType; RunLevel = $RunLevel }
}

function global:Register-ScheduledTask {
    param(
        [string]$TaskName,
        [object]$Action,
        [object]$Trigger,
        [object]$Settings,
        [object]$Principal,
        [string]$Description,
        [string]$Xml,
        [switch]$Force
    )
    if ($PSBoundParameters.ContainsKey('Xml')) {
        $global:NInferTestTaskXml = $Xml
    }
    else {
        $global:NInferTestTaskSerial++
        $global:NInferTestTaskXml = "managed-definition-$($global:NInferTestTaskSerial)"
    }
    $global:NInferTestTaskExists = $true
    $global:NInferTestTaskRunning = $false
    return [pscustomobject]@{ TaskName = $TaskName; State = 'Ready' }
}

function global:Unregister-ScheduledTask {
    param([string]$TaskName, [switch]$Confirm, [object]$ErrorAction)
    $global:NInferTestTaskExists = $false
    $global:NInferTestTaskRunning = $false
    $global:NInferTestTaskXml = $null
}

function global:Start-ScheduledTask {
    param([string]$TaskName)
    if (-not $global:NInferTestTaskExists) { throw 'mock task is missing' }
    $state = Get-Content -LiteralPath (Join-Path $global:NInferTestStateRoot 'state.json') -Raw |
        ConvertFrom-Json
    $global:NInferTestTaskRunning = [string]$state.active_release -cne
        [string]$global:NInferTestDeadReleaseId
}

function global:Stop-ScheduledTask {
    param([string]$TaskName)
    $global:NInferTestTaskRunning = $false
}

function global:Get-NetTCPConnection {
    param([string]$State, [int]$LocalPort, [object]$ErrorAction)
    return $null
}

function global:Invoke-RestMethod {
    param(
        [string]$Method,
        [string]$Uri,
        [hashtable]$Headers,
        [int]$TimeoutSec,
        [switch]$UseBasicParsing
    )
    $state = Get-Content -LiteralPath (Join-Path $global:NInferTestStateRoot 'state.json') -Raw |
        ConvertFrom-Json
    if ([string]$state.active_release -ceq [string]$global:NInferTestDeadReleaseId) {
        throw 'fixture release process exited immediately'
    }
    $release = $state.releases.PSObject.Properties[[string]$state.active_release].Value
    return [pscustomobject]@{
        artifact_type = 'ninfer_server_status'
        schema_version = 1
        status = 'ok'
        identity = [pscustomobject]@{
            patch_stack_sha = [string]$release.patch_stack_sha
            deployment_profile = [string]$release.deployment_profile
            binary_sha256 = [string]$release.binary_sha256
            model_artifact_sha256 = [string]$release.model_artifact_sha256
            config_sha256 = [string]$release.config_sha256
        }
        runtime = [pscustomobject]@{}
        scheduler = [pscustomobject]@{}
        cache = [pscustomobject]@{}
        mtp = [pscustomobject]@{}
    }
}

function global:Start-Sleep {
    [CmdletBinding()]
    param([int]$Seconds, [int]$Milliseconds)
    if (-not [string]::IsNullOrWhiteSpace([string]$global:NInferTestDeadReleaseId)) {
        $global:NInferTestDeadStartSleptMilliseconds += ($Seconds * 1000 + $Milliseconds)
        if ($global:NInferTestDeadStartSleptMilliseconds -gt 31000) {
            throw 'dead-start fixture exceeded the liveness-detection bound'
        }
    }
    if ($Seconds -ne 0) {
        & (Get-Command -Name Start-Sleep -CommandType Cmdlet) -Seconds $Seconds
    }
    elseif ($Milliseconds -ne 0) {
        & (Get-Command -Name Start-Sleep -CommandType Cmdlet) -Milliseconds $Milliseconds
    }
}

function global:icacls.exe {
    $arguments = @($args | ForEach-Object { [string]$_ })
    if ($arguments -contains '/inheritance:r') {
        $global:NInferAclObservations.Add([ordered]@{
                transactions_exist = Test-Path -LiteralPath (Join-Path $global:NInferTestStateRoot 'receipts/install-transactions')
                secrets_exist = Test-Path -LiteralPath (Join-Path $global:NInferTestStateRoot 'secrets')
            })
    }
    if ($arguments -contains '/reset') {
        $global:NInferTestAclResetArguments = $arguments
        if ($global:NInferTestAclFailure) {
            if ($arguments -contains '/C') {
                Set-Variable -Name LASTEXITCODE -Value 0 -Scope 1
                'Successfully processed 1 files; Failed processing 1 files'
                return
            }
            Set-Variable -Name LASTEXITCODE -Value 5 -Scope 1
            'fixture protected child: Access is denied.'
            return
        }
    }
    Set-Variable -Name LASTEXITCODE -Value 0 -Scope 1
}

function global:nvidia-smi.exe {
    param([Parameter(ValueFromRemainingArguments = $true)][object[]]$Arguments)
    '0, GPU-fixture-4090, NVIDIA GeForce RTX 4090'
    Set-Variable -Name LASTEXITCODE -Value 0 -Scope 1
}

function Write-Json([string]$Path, [object]$Value) {
    [IO.File]::WriteAllText(
        $Path,
        ($Value | ConvertTo-Json -Depth 20),
        [Text.UTF8Encoding]::new($false)
    )
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-Equal([object]$Actual, [object]$Expected, [string]$Message) {
    if ([string]$Actual -cne [string]$Expected) { throw $Message }
}

function New-SecretFile([string]$Directory, [string]$Name) {
    $bytes = New-Object byte[] 32
    $random = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $random.GetBytes($bytes) } finally { $random.Dispose() }
    $path = Join-Path $Directory "$Name.key"
    [IO.File]::WriteAllText(
        $path,
        [Convert]::ToBase64String($bytes),
        [Text.UTF8Encoding]::new($false)
    )
    return [pscustomobject]@{ path = $path; sha256 = Get-Sha256 $path }
}

function New-FixturePackage(
    [string]$Directory,
    [string]$InstanceId,
    [string]$ModelPath,
    [int]$Port
) {
    $workspace = Join-Path $Directory "package-$InstanceId"
    $payload = Join-Path $workspace $InstanceId
    New-Item -ItemType Directory -Force -Path (Join-Path $payload 'bin') | Out-Null
    [IO.File]::WriteAllText(
        (Join-Path (Join-Path $payload 'bin') 'ninfer-serve.exe'),
        "fixture-binary-$InstanceId",
        [Text.UTF8Encoding]::new($false)
    )
    Copy-Item -LiteralPath $ControllerPath -Destination (Join-Path $payload 'Control-Release.ps1')
    Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $ControllerPath) 'Protect-StateRoot.ps1') `
        -Destination (Join-Path $payload 'Protect-StateRoot.ps1')

    $config = [ordered]@{
        release_id = "fixture-$InstanceId"
        deployment_profile = "fixture-$InstanceId"
        model_id = 'fixture-model'
        listen = [ordered]@{ host = '127.0.0.1'; port = $Port }
        engine = [ordered]@{
            device = 'cuda:0'
            max_context = 1024
            kv_capacity = 1024
            prefill_chunk = 64
            kv_dtype = 'bf16'
            max_concurrency = 1
            max_pending_requests = 1
            pending_timeout_ms = 1000
        }
        reasoning = [ordered]@{ effort = 'low' }
        response_store = [ordered]@{ max_records = 1; max_mib = 1 }
        session_checkpoint = [ordered]@{ enabled = $true; quota_mib = 2048; staging_mib = 64 }
        persistent_cache = [ordered]@{ quota_gib = 1 }
        telemetry = [ordered]@{ stats_interval_ms = 1000 }
    }
    $configPath = Join-Path $payload 'server-config.json'
    Write-Json $configPath $config

    $binaryPath = Join-Path (Join-Path $payload 'bin') 'ninfer-serve.exe'
    $manifest = [ordered]@{
        artifact_type = 'ninfer_windows_release_manifest'
        schema_version = 1
        release_id = "fixture-$InstanceId"
        release_instance_id = $InstanceId
        deployment_profile = "fixture-$InstanceId"
        source_dirty = $false
        upstream_base_sha = ('1' * 40)
        patch_stack_sha = ('2' * 40)
        binary_sha256 = Get-Sha256 $binaryPath
        config_sha256 = Get-Sha256 $configPath
    }
    Write-Json (Join-Path $payload 'release-manifest.json') $manifest

    $model = Get-Item -LiteralPath $ModelPath
    $spec = [ordered]@{
        release_id = "fixture-$InstanceId"
        source = [ordered]@{ upstream_base_sha = ('1' * 40) }
        model = [ordered]@{ bytes = [Int64]$model.Length; sha256 = Get-Sha256 $ModelPath }
        lifecycle = [ordered]@{
            task_name = 'NInfer-Qwen38-4090-v0.1'
            candidate_root_directories = @('bin', 'config', 'logs', 'receipts')
            model_artifact_ownership = 'external-pinned-read-only'
            cache_ownership = 'state-root-per-release'
            interrupted_install_reentry = 'repair-required'
        }
    }
    Write-Json (Join-Path $payload 'release-spec.json') $spec

    $files = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $payload -Recurse -File)) {
        $relative = $file.FullName.Substring($payload.Length + 1).Replace('\', '/')
        $files += [ordered]@{
            relative_path = $relative
            bytes = [Int64]$file.Length
            sha256 = Get-Sha256 $file.FullName
        }
    }
    Write-Json (Join-Path $payload 'checksums.json') ([ordered]@{
            artifact_type = 'ninfer_package_checksums'
            schema_version = 1
            files = $files
        })

    $zip = Join-Path $Directory "$InstanceId.zip"
    Compress-Archive -Path $payload -DestinationPath $zip -CompressionLevel NoCompression
    return [pscustomobject]@{
        instance_id = $InstanceId
        path = $zip
        sha256 = Get-Sha256 $zip
    }
}

function Invoke-FixtureInstall(
    [object]$Package,
    [object]$Secret,
    [string]$Fault = '',
    [string]$Interrupt = '',
    [switch]$NoStart
) {
    if (-not [string]::IsNullOrEmpty($Fault) -and
        -not [string]::IsNullOrEmpty($Interrupt)) {
        throw 'fixture cannot inject a failure and an interruption together'
    }
    $previousFault = [Environment]::GetEnvironmentVariable('NINFER_TEST_INSTALL_FAILURE_AFTER', 'Process')
    $previousInterrupt = [Environment]::GetEnvironmentVariable('NINFER_TEST_INSTALL_INTERRUPTION_AFTER', 'Process')
    try {
        [Environment]::SetEnvironmentVariable(
            'NINFER_TEST_INSTALL_FAILURE_AFTER',
            $(if ([string]::IsNullOrEmpty($Fault)) { $null } else { $Fault }),
            'Process'
        )
        [Environment]::SetEnvironmentVariable(
            'NINFER_TEST_INSTALL_INTERRUPTION_AFTER',
            $(if ([string]::IsNullOrEmpty($Interrupt)) { $null } else { $Interrupt }),
            'Process'
        )
        $parameters = @{
            PackagePath = $Package.path
            PackageSha256 = $Package.sha256
            ModelArtifactPath = $script:ModelPath
            ApiKeyFile = $Secret.path
            GpuOwnerControllerPath = $script:OwnerControllerPath
            StateRoot = $script:StateRoot
        }
        if ($NoStart) { $parameters.NoStart = $true }
        & $InstallerPath @parameters | Out-Null
    }
    finally {
        [Environment]::SetEnvironmentVariable('NINFER_TEST_INSTALL_FAILURE_AFTER', $previousFault, 'Process')
        [Environment]::SetEnvironmentVariable('NINFER_TEST_INSTALL_INTERRUPTION_AFTER', $previousInterrupt, 'Process')
    }
}

function Invoke-InstallerRepair {
    $output = @(& $InstallerPath -RepairInterruptedInstall -StateRoot $script:StateRoot)
    if ($output.Count -ne 1) { throw 'installer repair did not emit exactly one final receipt' }
    return [pscustomobject]@{
        raw = [string]$output[0]
        receipt = ([string]$output[0] | ConvertFrom-Json)
    }
}

function Get-PendingFixtureTransactions {
    $root = Join-Path (Join-Path $script:StateRoot 'receipts') 'install-transactions'
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { return }
    foreach ($journal in @(Get-ChildItem -LiteralPath $root -File -Filter 'journal.json' -Recurse)) {
        $transaction = Get-Content -LiteralPath $journal.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        if ([string]$transaction.status -cin @('in_progress', 'repair_required')) {
            Write-Output $transaction
        }
    }
}

function Read-State {
    return Get-Content -LiteralPath (Join-Path $script:StateRoot 'state.json') -Raw | ConvertFrom-Json
}

function ConvertTo-Schema1Release([object]$Release) {
    return [ordered]@{
        release_root = [string]$Release.release_root
        server_executable = [string]$Release.server_executable
        model_artifact = [string]$Release.model_artifact
        config_file = [string]$Release.config_file
        api_key_file = [string]$Release.api_key_file
        host = [string]$Release.host
        port = [int]$Release.port
        deployment_profile = [string]$Release.deployment_profile
        upstream_base_sha = [string]$Release.upstream_base_sha
        patch_stack_sha = [string]$Release.patch_stack_sha
        binary_sha256 = [string]$Release.binary_sha256
        model_sha256 = [string]$Release.model_artifact_sha256
        config_sha256 = [string]$Release.config_sha256
        installed_utc = [string]$Release.installed_utc
    }
}

function ConvertTo-Schema1State([object]$State) {
    $releases = [ordered]@{}
    foreach ($property in $State.releases.PSObject.Properties) {
        $releases[$property.Name] = ConvertTo-Schema1Release $property.Value
    }
    return [ordered]@{
        artifact_type = 'ninfer_windows_lifecycle_state'
        schema_version = 1
        task_name = [string]$State.task_name
        active_release = [string]$State.active_release
        previous_release = $State.previous_release
        releases = $releases
    }
}

function Assert-CleanSchema3Release([object]$Release, [string]$ReleaseId) {
    $expectedFields = @(
        'release_root', 'server_executable', 'model_artifact', 'model_reference', 'config_file',
        'cache_root', 'receipts_root', 'api_key_file', 'host', 'port', 'deployment_profile',
        'upstream_base_sha', 'patch_stack_sha', 'binary_sha256', 'model_artifact_sha256',
        'model_bytes', 'model_creation_utc_ticks', 'model_last_write_utc_ticks',
        'config_sha256', 'gpu_index', 'gpu_uuid', 'gpu_name', 'installed_utc'
    )
    $actualFields = @($Release.PSObject.Properties | ForEach-Object { $_.Name })
    Assert-Equal $actualFields.Count $expectedFields.Count "schema-3 release has unexpected fields: $ReleaseId"
    foreach ($field in $expectedFields) {
        Assert-True ($null -ne $Release.PSObject.Properties[$field]) "schema-3 release is missing $($field): $ReleaseId"
    }
    Assert-True ($null -eq $Release.PSObject.Properties['model_sha256']) "schema-3 release retained model_sha256: $ReleaseId"
    $model = Get-Item -LiteralPath ([string]$Release.model_artifact)
    Assert-Equal ([Int64]$Release.model_bytes) ([Int64]$model.Length) "schema-3 model size is stale: $ReleaseId"
    Assert-Equal ([Int64]$Release.model_creation_utc_ticks) ([Int64]$model.CreationTimeUtc.Ticks) "schema-3 model creation identity is stale: $ReleaseId"
    Assert-Equal ([Int64]$Release.model_last_write_utc_ticks) ([Int64]$model.LastWriteTimeUtc.Ticks) "schema-3 model write identity is stale: $ReleaseId"
}

function Assert-CandidateLayout([object]$Release, [string]$ReleaseId) {
    Assert-Equal ([string]$Release.model_reference) 'external-pinned-read-only' "candidate model reference is not external: $ReleaseId"
    $actualModelPath = (Resolve-Path -LiteralPath ([string]$Release.model_artifact)).Path
    $expectedModelPath = (Resolve-Path -LiteralPath $script:ModelPath).Path
    Assert-Equal $actualModelPath $expectedModelPath "candidate does not reference the pinned model: $ReleaseId"
    $statePrefix = [IO.Path]::GetFullPath($script:StateRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    Assert-True (-not $actualModelPath.StartsWith($statePrefix, [StringComparison]::OrdinalIgnoreCase)) "candidate model reference is inside lifecycle state: $ReleaseId"

    $actualReleaseRoot = [IO.Path]::GetFullPath([string]$Release.release_root)
    $expectedReleaseRoot = [IO.Path]::GetFullPath((Join-Path (Join-Path $script:StateRoot 'releases') $ReleaseId))
    Assert-Equal $actualReleaseRoot $expectedReleaseRoot "candidate root identity mismatch: $ReleaseId"
    $expected = @('bin', 'config', 'logs', 'receipts') | Sort-Object
    $actual = @(Get-ChildItem -LiteralPath $actualReleaseRoot -Directory -Force |
        Sort-Object Name | ForEach-Object Name)
    Assert-Equal $actual.Count $expected.Count "candidate root directory count mismatch: $ReleaseId"
    for ($index = 0; $index -lt $expected.Count; $index++) {
        Assert-Equal $actual[$index] $expected[$index] "candidate root directory mismatch: $ReleaseId"
    }
    $rootFiles = @(Get-ChildItem -LiteralPath $actualReleaseRoot -File -Force)
    Assert-Equal $rootFiles.Count 0 "candidate root contains unclassified files: $ReleaseId"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $actualReleaseRoot 'model'))) "candidate root contains a model copy: $ReleaseId"

    Assert-Equal ([IO.Path]::GetFullPath([string]$Release.server_executable)) ([IO.Path]::GetFullPath((Join-Path (Join-Path $actualReleaseRoot 'bin') 'ninfer-serve.exe'))) "candidate server identity mismatch: $ReleaseId"
    Assert-Equal ([IO.Path]::GetFullPath([string]$Release.config_file)) ([IO.Path]::GetFullPath((Join-Path (Join-Path $actualReleaseRoot 'config') 'server-config.json'))) "candidate config identity mismatch: $ReleaseId"
    Assert-Equal ([IO.Path]::GetFullPath([string]$Release.receipts_root)) ([IO.Path]::GetFullPath((Join-Path $actualReleaseRoot 'receipts'))) "candidate receipts root mismatch: $ReleaseId"
    Assert-Equal ([IO.Path]::GetFullPath([string]$Release.cache_root)) ([IO.Path]::GetFullPath((Join-Path (Join-Path $script:StateRoot 'cache') $ReleaseId))) "candidate cache root mismatch: $ReleaseId"
    Assert-Equal ([IO.Path]::GetFullPath([string]$Release.api_key_file)) ([IO.Path]::GetFullPath((Join-Path (Join-Path (Join-Path $script:StateRoot 'secrets') $ReleaseId) 'api-key.txt'))) "candidate secret identity mismatch: $ReleaseId"
}

function Assert-KeyLedger([hashtable]$Ledger) {
    $state = Read-State
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $Ledger.GetEnumerator()) {
        $release = $state.releases.PSObject.Properties[[string]$entry.Key].Value
        Assert-True ($null -ne $release) "release key metadata is missing: $($entry.Key)"
        $path = [string]$release.api_key_file
        Assert-True ($paths.Add($path)) 'release key paths are not unique'
        Assert-True (Test-Path -LiteralPath $path -PathType Leaf) 'release key file is missing'
        Assert-Equal (Get-Sha256 $path) ([string]$entry.Value.sha256) 'release key content changed'
        Assert-Equal $path ([string]$entry.Value.path) 'release key path changed'
    }
}

function Read-OwnerState {
    return Get-Content -LiteralPath $script:OwnerStatePath -Raw | ConvertFrom-Json
}

function Assert-OwnerPaused([bool]$Expected, [string]$Message) {
    Assert-Equal ([bool](Read-OwnerState).paused) $Expected $Message
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('ninfer-release-lifecycle-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$instrumentedRoot = Join-Path $testRoot 'instrumented-installer'
New-Item -ItemType Directory -Path $instrumentedRoot | Out-Null
$publishedInstallerText = [IO.File]::ReadAllText($publishedInstallerPath, [Text.Encoding]::UTF8)
Assert-True (-not $publishedInstallerText.Contains('NINFER_INSTALL_TEST_MODE')) `
    'published installer exposes ambient test-mode activation'
$productionTestMode = '$script:InstallTestMode = $false'
$productionTestModeIndex = $publishedInstallerText.IndexOf($productionTestMode, [StringComparison]::Ordinal)
Assert-True ($productionTestModeIndex -ge 0 -and
    $publishedInstallerText.IndexOf(
        $productionTestMode,
        $productionTestModeIndex + $productionTestMode.Length,
        [StringComparison]::Ordinal
    ) -lt 0) `
    'published installer test mode is not one hardcoded false assignment'
$instrumentedInstallerText = $publishedInstallerText.Replace(
    $productionTestMode,
    '$script:InstallTestMode = $true'
)
$InstallerPath = Join-Path $instrumentedRoot 'Install-Release.ps1'
[IO.File]::WriteAllText($InstallerPath, $instrumentedInstallerText, [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $publishedInstallerPath) 'Control-GpuOwner.ps1') `
    -Destination (Join-Path $instrumentedRoot 'Control-GpuOwner.ps1')
$ControllerPath = Join-Path $instrumentedRoot 'Control-Release.ps1'
Copy-Item -LiteralPath $publishedControllerPath -Destination $ControllerPath
$instrumentedProtection = @'
function Initialize-NInferProtectedStateRoot([string]$Path) {
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
    return (Resolve-Path -LiteralPath $Path).Path
}
function Assert-NInferProtectedStateTree([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw 'instrumented protected state root is missing'
    }
}
'@
[IO.File]::WriteAllText(
    (Join-Path $instrumentedRoot 'Protect-StateRoot.ps1'),
    $instrumentedProtection,
    [Text.UTF8Encoding]::new($false)
)
$script:StateRoot = Join-Path $testRoot 'state'
$script:ModelPath = Join-Path $testRoot 'model.ninfer'
$script:OwnerStatePath = Join-Path $testRoot 'gpu-owner-state.json'
$script:OwnerControllerPath = Join-Path $testRoot 'Control-GpuOwner.ps1'
Write-Json $script:OwnerStatePath ([ordered]@{ paused = $false; stop_calls = 0 })
$ownerScript = @'
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('status', 'stop', 'start')]
    [string]$Action
)
$ErrorActionPreference = 'Stop'
$statePath = '__OWNER_STATE__'
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
if ($Action -ceq 'stop') { $state.paused = $true; $state.stop_calls = [int]$state.stop_calls + 1 }
if ($Action -ceq 'start') { $state.paused = $false }
[IO.File]::WriteAllText($statePath, ($state | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
$state | ConvertTo-Json -Compress
'@
$ownerScript = $ownerScript.Replace('__OWNER_STATE__', $script:OwnerStatePath.Replace("'", "''"))
[IO.File]::WriteAllText($script:OwnerControllerPath, $ownerScript, [Text.UTF8Encoding]::new($false))
$global:NInferTestStateRoot = $script:StateRoot
$originalFault = [Environment]::GetEnvironmentVariable('NINFER_TEST_INSTALL_FAILURE_AFTER', 'Process')
$originalInterrupt = [Environment]::GetEnvironmentVariable('NINFER_TEST_INSTALL_INTERRUPTION_AFTER', 'Process')

try {
    $modelBytes = New-Object byte[] 256
    $modelRandom = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $modelRandom.GetBytes($modelBytes) } finally { $modelRandom.Dispose() }
    [IO.File]::WriteAllBytes($script:ModelPath, $modelBytes)

    $ledger = @{}
    $basePackage = New-FixturePackage $testRoot 'base-release' $script:ModelPath 48100
    $baseSecret = New-SecretFile $testRoot 'base-release'
    $publishedOwnerController = Join-Path (Split-Path -Parent $InstallerPath) 'Control-GpuOwner.ps1'
    Assert-True (Test-Path -LiteralPath $publishedOwnerController -PathType Leaf) `
        'first-install default GPU-owner controller is missing beside the installer'
    Invoke-FixtureInstall $basePackage $baseSecret
    $earlyAclObservations = @($global:NInferAclObservations | Where-Object { -not $_.transactions_exist -and -not $_.secrets_exist })
    Assert-Equal $earlyAclObservations.Count 1 'first-install ACL hardening did not precede transaction and secret children'
    Assert-True (-not [bool]$global:NInferAclObservations[0].transactions_exist -and -not [bool]$global:NInferAclObservations[0].secrets_exist) 'first ACL hardening invocation was late'
    $state = Read-State
    Assert-Equal $state.active_release 'base-release' 'base release did not activate'
    Assert-Equal ([int]$state.schema_version) 3 'base release did not publish schema 3 state'
    $baseInstalledRelease = $state.releases.PSObject.Properties['base-release'].Value
    Assert-CleanSchema3Release $baseInstalledRelease 'base-release'
    Assert-CandidateLayout $baseInstalledRelease 'base-release'
    $baseKeyPath = [string]$state.releases.PSObject.Properties['base-release'].Value.api_key_file
    Assert-Equal $baseKeyPath (Join-Path (Join-Path (Join-Path $script:StateRoot 'secrets') 'base-release') 'api-key.txt') 'base key is not release-scoped'
    $ledger['base-release'] = [pscustomobject]@{ path = $baseKeyPath; sha256 = $baseSecret.sha256 }
    Assert-KeyLedger $ledger
    Assert-OwnerPaused $true 'base start did not release the prior GPU owner'
    Assert-True (Test-Path -LiteralPath (Join-Path $script:StateRoot 'gpu-owner-lease.json')) 'base GPU-owner lease is missing'


    $managedController = Join-Path $script:StateRoot 'Control-Release.ps1'
    & $managedController -Action Stop -StateRoot $script:StateRoot | Out-Null
    Assert-OwnerPaused $false 'schema-1 fixture setup did not stop the base release cleanly'

    $schema3BaseState = Read-State
    $baseReleaseBeforeMigration = $schema3BaseState.releases.PSObject.Properties['base-release'].Value
    $baseVerifiedModelDigest = [string]$baseReleaseBeforeMigration.model_artifact_sha256
    $schema3ModelPath = [string]$baseReleaseBeforeMigration.model_artifact
    $schema3VerifiedWriteTime = [DateTime]::new(
        [Int64]$baseReleaseBeforeMigration.model_last_write_utc_ticks,
        [DateTimeKind]::Utc
    )
    $schema3MismatchPackage = New-FixturePackage $testRoot 'schema3-mismatch' $script:ModelPath 48148
    $schema3MismatchSecret = New-SecretFile $testRoot 'schema3-mismatch'
    $schema3StateShaBefore = Get-Sha256 (Join-Path $script:StateRoot 'state.json')
    $schema3ControllerShaBefore = Get-Sha256 $managedController
    $schema3TaskXmlBefore = [string]$global:NInferTestTaskXml
    $schema3TaskRunningBefore = $global:NInferTestTaskRunning
    $schema3MismatchFailure = $null
    try {
        [IO.File]::SetLastWriteTimeUtc($schema3ModelPath, $schema3VerifiedWriteTime.AddSeconds(1))
        Invoke-FixtureInstall $schema3MismatchPackage $schema3MismatchSecret -NoStart
    }
    catch {
        $schema3MismatchFailure = $_
    }
    finally {
        [IO.File]::SetLastWriteTimeUtc($schema3ModelPath, $schema3VerifiedWriteTime)
    }
    Assert-True ($null -ne $schema3MismatchFailure) 'install re-baselined changed schema-3 model metadata'
    Assert-True ($schema3MismatchFailure.Exception.Message -like "*cannot migrate installed release 'base-release': model artifact changed after install*") 'changed schema-3 model returned the wrong failure'
    Assert-Equal (Get-Sha256 (Join-Path $script:StateRoot 'state.json')) $schema3StateShaBefore 'schema-3 identity rejection published lifecycle state'
    Assert-Equal (Get-Sha256 $managedController) $schema3ControllerShaBefore 'schema-3 identity rejection changed the managed controller'
    Assert-Equal ([string]$global:NInferTestTaskXml) $schema3TaskXmlBefore 'schema-3 identity rejection changed the scheduled task'
    Assert-Equal $global:NInferTestTaskRunning $schema3TaskRunningBefore 'schema-3 identity rejection changed task liveness'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path (Join-Path $script:StateRoot 'releases') 'schema3-mismatch'))) 'schema-3 identity rejection left a candidate release'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path (Join-Path $script:StateRoot 'secrets') 'schema3-mismatch'))) 'schema-3 identity rejection left a candidate secret'
    $unmigratableState = ConvertTo-Schema1State $schema3BaseState
    $unmigratableRelease = ConvertTo-Schema1Release $baseReleaseBeforeMigration
    $unmigratableRelease['model_artifact'] = Join-Path $script:StateRoot 'missing-model.ninfer'
    $unmigratableState.releases['unmigratable-release'] = $unmigratableRelease
    Write-Json (Join-Path $script:StateRoot 'state.json') $unmigratableState

    $migrationFailurePackage = New-FixturePackage $testRoot 'migration-failure' $script:ModelPath 48149
    $migrationFailureSecret = New-SecretFile $testRoot 'migration-failure'
    $legacyStateSha = Get-Sha256 (Join-Path $script:StateRoot 'state.json')
    $migrationFailure = $null
    try {
        Invoke-FixtureInstall $migrationFailurePackage $migrationFailureSecret -NoStart
    }
    catch {
        $migrationFailure = $_
    }
    Assert-True ($null -ne $migrationFailure) 'install accepted an unmigratable prior release record'
    Assert-True ($migrationFailure.Exception.Message -like "*cannot migrate installed release 'unmigratable-release': model artifact is missing*") 'unmigratable release returned the wrong failure'
    Assert-Equal (Get-Sha256 (Join-Path $script:StateRoot 'state.json')) $legacyStateSha 'failed schema migration published lifecycle state'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path (Join-Path $script:StateRoot 'releases') 'migration-failure'))) 'failed schema migration left a candidate release'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path (Join-Path $script:StateRoot 'secrets') 'migration-failure'))) 'failed schema migration left a candidate secret'

    $legacyState = ConvertTo-Schema1State $schema3BaseState
    Write-Json (Join-Path $script:StateRoot 'state.json') $legacyState
    $migrationPackage = New-FixturePackage $testRoot 'migration-release' $script:ModelPath 48150
    $migrationSecret = New-SecretFile $testRoot 'migration-release'
    Invoke-FixtureInstall $migrationPackage $migrationSecret
    $migratedState = Read-State
    Assert-Equal ([int]$migratedState.schema_version) 3 'schema-1 lifecycle state was not migrated to schema 3'
    Assert-Equal $migratedState.active_release 'migration-release' 'migration candidate did not activate'
    foreach ($property in $migratedState.releases.PSObject.Properties) {
        Assert-CleanSchema3Release $property.Value $property.Name
        Assert-CandidateLayout $property.Value $property.Name
    }
    $migratedBase = $migratedState.releases.PSObject.Properties['base-release'].Value
    Assert-Equal ([string]$migratedBase.model_artifact_sha256) $baseVerifiedModelDigest 'schema migration did not preserve the verified model digest'
    $migrationKeyPath = [string]$migratedState.releases.PSObject.Properties['migration-release'].Value.api_key_file
    $ledger['migration-release'] = [pscustomobject]@{ path = $migrationKeyPath; sha256 = $migrationSecret.sha256 }
    Assert-KeyLedger $ledger
    Assert-OwnerPaused $true 'migrated release start did not release the GPU owner'

    & $managedController -Action Rollback -StateRoot $script:StateRoot | Out-Null
    Assert-Equal (Read-State).active_release 'base-release' 'rollback could not start the migrated schema-1 release'
    & $managedController -Action Restart -StateRoot $script:StateRoot | Out-Null
    Assert-Equal (Read-State).active_release 'base-release' 'restart changed the migrated active release'
    Assert-True $global:NInferTestTaskRunning 'migrated release restart did not remain live'

    $aclPackage = New-FixturePackage $testRoot 'acl-incomplete' $script:ModelPath 48151
    $aclSecret = New-SecretFile $testRoot 'acl-incomplete'
    $aclStateShaBefore = Get-Sha256 (Join-Path $script:StateRoot 'state.json')
    $aclControllerShaBefore = Get-Sha256 $managedController
    $aclTaskXmlBefore = [string]$global:NInferTestTaskXml
    $global:NInferTestAclResetArguments = @()
    $global:NInferTestAclFailure = $true
    $aclFailure = $null
    try {
        Invoke-FixtureInstall $aclPackage $aclSecret -NoStart
    }
    catch {
        $aclFailure = $_
    }
    finally {
        $global:NInferTestAclFailure = $false
    }
    Assert-True ($null -ne $aclFailure) 'install accepted a failed recursive ACL ordering probe'
    Assert-True ($aclFailure.Exception.Message -like '*failed test-mode recursive ACL ordering probe*') "ACL ordering probe returned the wrong failure: $($aclFailure.Exception.Message)"
    Assert-True ($global:NInferTestAclResetArguments -contains '/reset') 'ACL ordering fixture was not executed'
    Assert-True ($global:NInferTestAclResetArguments -notcontains '/C') 'ACL ordering fixture retained continue-on-error'
    Assert-Equal (Get-Sha256 (Join-Path $script:StateRoot 'state.json')) $aclStateShaBefore 'ACL failure published lifecycle state'
    Assert-Equal (Get-Sha256 $managedController) $aclControllerShaBefore 'ACL failure changed the managed controller'
    Assert-Equal ([string]$global:NInferTestTaskXml) $aclTaskXmlBefore 'ACL failure changed the scheduled task'
    Assert-True $global:NInferTestTaskRunning 'ACL failure did not restart the incumbent release'
    Assert-Equal (Read-State).active_release 'base-release' 'ACL failure left the candidate active'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path (Join-Path $script:StateRoot 'releases') 'acl-incomplete'))) 'ACL failure left a candidate release'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path (Join-Path $script:StateRoot 'secrets') 'acl-incomplete'))) 'ACL failure left a candidate secret'
    Assert-OwnerPaused $true 'ACL failure changed GPU-owner state'
    $faultPoints = @(
        'model_reference',
        'candidate_layout',
        'secret_copy',
        'incumbent_stop',
        'owner_controller_copy',
        'acl',
        'controller_copy',
        'state_activation',
        'task_registration',
        'candidate_start'
    )
    $index = 0
    foreach ($fault in $faultPoints) {
        $index++
        $instance = "candidate-$index"
        $package = New-FixturePackage $testRoot $instance $script:ModelPath (48100 + $index)
        $secret = New-SecretFile $testRoot $instance
        $stateShaBefore = Get-Sha256 (Join-Path $script:StateRoot 'state.json')
        $controllerShaBefore = Get-Sha256 (Join-Path $script:StateRoot 'Control-Release.ps1')
        $taskXmlBefore = [string]$global:NInferTestTaskXml
        $taskRunningBefore = $global:NInferTestTaskRunning

        $failed = $false
        try { Invoke-FixtureInstall $package $secret $fault }
        catch {
            $failed = $true
            Assert-True ($_.Exception.Message -like "*injected install failure after $fault*") `
                'fault injection returned the wrong failure'
        }
        Assert-True $failed "fault point did not fail: $fault"
        Assert-Equal (Get-Sha256 (Join-Path $script:StateRoot 'state.json')) $stateShaBefore `
            'state bytes changed after failed install'
        Assert-Equal (Get-Sha256 (Join-Path $script:StateRoot 'Control-Release.ps1')) `
            $controllerShaBefore 'controller bytes changed after failed install'
        Assert-Equal $global:NInferTestTaskXml $taskXmlBefore 'task definition changed after failed install'
        Assert-Equal $global:NInferTestTaskRunning $taskRunningBefore 'incumbent running state changed after failed install'
        Assert-Equal (Read-State).active_release 'base-release' 'failed candidate remained active'
        $candidateReleaseRoot = Join-Path (Join-Path $script:StateRoot 'releases') $instance
        $candidateSecretRoot = Join-Path (Join-Path $script:StateRoot 'secrets') $instance
        Assert-True (-not (Test-Path -LiteralPath $candidateReleaseRoot)) 'failed candidate payload remains'
        Assert-True (-not (Test-Path -LiteralPath $candidateSecretRoot)) 'failed candidate secret remains'
        Assert-Equal (@(Get-ChildItem -LiteralPath $script:StateRoot -Directory -Filter 'staging-*').Count) 0 'failed candidate staging directory remains'
        Assert-KeyLedger $ledger
        Assert-OwnerPaused $true "prior GPU owner changed after failed install: $fault"
        Assert-True (Test-Path -LiteralPath (Join-Path $script:StateRoot 'gpu-owner-lease.json')) 'incumbent GPU-owner lease was not restored'

        Invoke-FixtureInstall $package $secret
        $state = Read-State
        Assert-Equal $state.active_release $instance 'clean retry did not activate candidate'
        $candidateRelease = $state.releases.PSObject.Properties[$instance].Value
        Assert-CandidateLayout $candidateRelease $instance
        $candidateKeyPath = [string]$state.releases.PSObject.Properties[$instance].Value.api_key_file
        Assert-Equal $candidateKeyPath (Join-Path (Join-Path (Join-Path $script:StateRoot 'secrets') $instance) 'api-key.txt') 'candidate key is not release-scoped'
        $ledger[$instance] = [pscustomobject]@{ path = $candidateKeyPath; sha256 = $secret.sha256 }
        Assert-KeyLedger $ledger
        Assert-OwnerPaused $true 'candidate start did not release the prior GPU owner'

        & (Join-Path $script:StateRoot 'Control-Release.ps1') -Action Rollback `
            -StateRoot $script:StateRoot | Out-Null
        Assert-Equal (Read-State).active_release 'base-release' 'rollback did not restore base release'
        Assert-KeyLedger $ledger
        Assert-OwnerPaused $true 'rollback did not preserve the incumbent GPU-owner lease'
    }

    $interruptionStateSha = Get-Sha256 (Join-Path $script:StateRoot 'state.json')
    $interruptionControllerSha = Get-Sha256 $managedController
    $interruptionTaskXml = [string]$global:NInferTestTaskXml
    $interruptionTaskRunning = $global:NInferTestTaskRunning
    $interruptionOwnerPaused = [bool](Read-OwnerState).paused
    $pinnedModelSha = Get-Sha256 $script:ModelPath

    $scalarPackage = New-FixturePackage $testRoot 'interrupted-scalar' $script:ModelPath 48190
    $scalarSecret = New-SecretFile $testRoot 'interrupted-scalar'
    $scalarProgressWriter = [IO.StringWriter]::new()
    $previousConsoleOut = [Console]::Out
    $scalarInterruption = $null
    [Console]::SetOut($scalarProgressWriter)
    try {
        Invoke-FixtureInstall -Package $scalarPackage -Secret $scalarSecret -Interrupt 'stage_create' -NoStart
    }
    catch {
        $scalarInterruption = $_
    }
    finally {
        [Console]::SetOut($previousConsoleOut)
    }
    Assert-True ($null -ne $scalarInterruption) 'stage-only interruption did not stop the installer'
    Assert-True ($scalarInterruption.Exception.Message -like '*simulated interrupted install after stage_create*') 'stage-only interruption returned the wrong failure'
    $scalarProgressLines = @($scalarProgressWriter.ToString().Split([string[]]@([Environment]::NewLine), [StringSplitOptions]::RemoveEmptyEntries) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $scalarProgressWriter.Dispose()
    $scalarProgress = @($scalarProgressLines | ForEach-Object { $_ | ConvertFrom-Json })
    Assert-True ($scalarProgress.Count -ge 3) 'installer did not stream stage-only progress before interruption'
    Assert-True (@($scalarProgress | Where-Object phase -ceq 'install_started').Count -eq 1) 'installer did not stream its durable transaction start'
    Assert-True (@($scalarProgress | Where-Object artifact_type -cne 'ninfer_windows_install_progress').Count -eq 0) 'installer progress stream contains a non-progress record'

    $scalarPending = @(Get-PendingFixtureTransactions)
    Assert-Equal $scalarPending.Count 1 'single interrupted transaction was not preserved as a scalar-safe collection'
    $scalarReentryError = $null
    try {
        Invoke-FixtureInstall -Package $scalarPackage -Secret $scalarSecret -NoStart
    }
    catch {
        $scalarReentryError = $_
    }
    Assert-True ($null -ne $scalarReentryError) 'interrupted installer transaction allowed qualification re-entry'
    Assert-True ($scalarReentryError.Exception.Message -like '*requires installer repair before another install*') 'interrupted installer re-entry did not route to repair'

    $scalarRepair = Invoke-InstallerRepair
    Assert-Equal ([string]$scalarRepair.receipt.artifact_type) 'ninfer_windows_installer_repair_receipt' 'scalar cleanup emitted the wrong receipt'
    Assert-Equal ([int]$scalarRepair.receipt.transaction_count) 1 'scalar cleanup receipt lost its transaction array'
    Assert-Equal ([int]$scalarRepair.receipt.cleanup_action_count) 1 'scalar cleanup receipt lost its sole cleanup action'
    Assert-True ($scalarRepair.raw.Contains('"transactions":[')) 'scalar transaction serialized as an object instead of an array'
    Assert-True ($scalarRepair.raw.Contains('"cleanup_actions":[')) 'scalar cleanup action serialized as an object instead of an array'
    Assert-Equal (@(Get-PendingFixtureTransactions).Count) 0 'scalar repair left a pending transaction'
    foreach ($action in @($scalarRepair.receipt.transactions[0].cleanup_actions)) {
        Assert-True (-not (Test-Path -LiteralPath ([string]$action.path))) 'scalar repair receipt names a cleanup path that still exists'
    }
    Assert-Equal (@(Get-ChildItem -LiteralPath $script:StateRoot -Directory -Filter 'staging-*').Count) 0 'scalar repair left a staging directory'
    Assert-Equal (Get-Sha256 (Join-Path $script:StateRoot 'state.json')) $interruptionStateSha 'scalar repair changed lifecycle state'
    Assert-Equal (Get-Sha256 $managedController) $interruptionControllerSha 'scalar repair changed the controller'
    Assert-Equal ([string]$global:NInferTestTaskXml) $interruptionTaskXml 'scalar repair changed the scheduled task'
    Assert-Equal $global:NInferTestTaskRunning $interruptionTaskRunning 'scalar repair changed task liveness'
    Assert-Equal ([bool](Read-OwnerState).paused) $interruptionOwnerPaused 'scalar repair changed GPU-owner state'

    $arrayPackage = New-FixturePackage $testRoot 'interrupted-array' $script:ModelPath 48191
    $arraySecret = New-SecretFile $testRoot 'interrupted-array'
    $arrayProgressWriter = [IO.StringWriter]::new()
    $previousConsoleOut = [Console]::Out
    $arrayInterruption = $null
    [Console]::SetOut($arrayProgressWriter)
    try {
        Invoke-FixtureInstall -Package $arrayPackage -Secret $arraySecret -Interrupt 'secret_copy' -NoStart
    }
    catch {
        $arrayInterruption = $_
    }
    finally {
        [Console]::SetOut($previousConsoleOut)
    }
    Assert-True ($null -ne $arrayInterruption) 'candidate-layout interruption did not stop the installer'
    Assert-True ($arrayInterruption.Exception.Message -like '*simulated interrupted install after secret_copy*') 'candidate-layout interruption returned the wrong failure'
    $arrayInstallProgress = @($arrayProgressWriter.ToString().Split([string[]]@([Environment]::NewLine), [StringSplitOptions]::RemoveEmptyEntries) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | ForEach-Object { $_ | ConvertFrom-Json })
    $arrayProgressWriter.Dispose()
    Assert-True (@($arrayInstallProgress | Where-Object phase -ceq 'model_identity_started').Count -eq 1) 'installer did not stream model identity verification'
    Assert-True (@($arrayInstallProgress | Where-Object phase -ceq 'model_identity_completed').Count -eq 1) 'installer did not stream model identity completion'
    Assert-True (@($arrayInstallProgress | Where-Object phase -ceq 'install_interrupted').Count -eq 1) 'installer did not stream repair routing after interruption'

    $arrayPending = @(Get-PendingFixtureTransactions)
    Assert-Equal $arrayPending.Count 1 'candidate interruption was not preserved as one pending transaction'
    $interruptedCandidateRoot = [string]$arrayPending[0].release_root
    Assert-True (Test-Path -LiteralPath $interruptedCandidateRoot -PathType Container) 'candidate interruption did not preserve repairable candidate state'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $interruptedCandidateRoot 'model'))) 'interrupted candidate contains a model copy'
    $arrayReentryError = $null
    try {
        Invoke-FixtureInstall -Package $arrayPackage -Secret $arraySecret -NoStart
    }
    catch {
        $arrayReentryError = $_
    }
    Assert-True ($null -ne $arrayReentryError) 'candidate interruption allowed qualification re-entry'
    Assert-True ($arrayReentryError.Exception.Message -like '*requires installer repair before another install*') 'candidate interruption did not route re-entry to repair'

    $cleanupProgressWriter = [IO.StringWriter]::new()
    $previousConsoleOut = [Console]::Out
    [Console]::SetOut($cleanupProgressWriter)
    try {
        $arrayRepair = Invoke-InstallerRepair
    }
    finally {
        [Console]::SetOut($previousConsoleOut)
    }
    $cleanupProgress = @($cleanupProgressWriter.ToString().Split([string[]]@([Environment]::NewLine), [StringSplitOptions]::RemoveEmptyEntries) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | ForEach-Object { $_ | ConvertFrom-Json })
    $cleanupProgressWriter.Dispose()
    Assert-True (@($cleanupProgress | Where-Object phase -ceq 'cleanup_inventory').Count -eq 1) 'repair did not stream its cleanup inventory'
    Assert-True (@($cleanupProgress | Where-Object phase -ceq 'cleanup_target_started').Count -eq 4) 'repair did not stream every deletion start'
    Assert-True (@($cleanupProgress | Where-Object phase -ceq 'cleanup_target_completed').Count -eq 4) 'repair did not stream every deletion completion'
    Assert-Equal ([int]$arrayRepair.receipt.transaction_count) 1 'array cleanup receipt lost its transaction array'
    Assert-Equal ([int]$arrayRepair.receipt.cleanup_action_count) 4 'array cleanup receipt lost cleanup actions'
    Assert-True ($arrayRepair.raw.Contains('"transactions":[')) 'array transaction serialized as an object'
    Assert-True ($arrayRepair.raw.Contains('"cleanup_actions":[')) 'multiple cleanup actions did not serialize as an array'
    Assert-Equal (@($arrayRepair.receipt.transactions[0].cleanup_actions).Count) 4 'array cleanup actions did not deserialize as four entries'
    foreach ($action in @($arrayRepair.receipt.transactions[0].cleanup_actions)) {
        Assert-True (-not (Test-Path -LiteralPath ([string]$action.path))) 'array repair receipt names a cleanup path that still exists'
    }
    Assert-Equal (@(Get-PendingFixtureTransactions).Count) 0 'array repair left a pending transaction'
    Assert-True (-not (Test-Path -LiteralPath $interruptedCandidateRoot)) 'array repair left candidate state'
    Assert-Equal (Get-Sha256 (Join-Path $script:StateRoot 'state.json')) $interruptionStateSha 'array repair changed lifecycle state'
    Assert-Equal (Get-Sha256 $managedController) $interruptionControllerSha 'array repair changed the controller'
    Assert-Equal ([string]$global:NInferTestTaskXml) $interruptionTaskXml 'array repair changed the scheduled task'
    Assert-Equal $global:NInferTestTaskRunning $interruptionTaskRunning 'array repair changed task liveness'
    Assert-Equal ([bool](Read-OwnerState).paused) $interruptionOwnerPaused 'array repair changed GPU-owner state'
    Assert-Equal (Get-Sha256 $script:ModelPath) $pinnedModelSha 'interrupted install or repair changed the pinned model'

    $deadRollbackPackage = New-FixturePackage $testRoot 'dead-rollback-current' $script:ModelPath 48200
    $deadRollbackSecret = New-SecretFile $testRoot 'dead-rollback-current'
    Invoke-FixtureInstall $deadRollbackPackage $deadRollbackSecret
    $deadRollbackState = Read-State
    $deadRollbackKeyPath = [string]$deadRollbackState.releases.PSObject.Properties['dead-rollback-current'].Value.api_key_file
    $ledger['dead-rollback-current'] = [pscustomobject]@{ path = $deadRollbackKeyPath; sha256 = $deadRollbackSecret.sha256 }
    Assert-KeyLedger $ledger

    $global:NInferTestDeadReleaseId = 'base-release'
    $global:NInferTestDeadStartSleptMilliseconds = 0
    $deadStartWatch = [Diagnostics.Stopwatch]::StartNew()
    $deadRollbackFailure = $null
    try {
        & $managedController -Action Rollback -StateRoot $script:StateRoot | Out-Null
    }
    catch {
        $deadRollbackFailure = $_
    }
    finally {
        $deadStartWatch.Stop()
        $global:NInferTestDeadReleaseId = $null
    }
    Assert-True ($null -ne $deadRollbackFailure) 'rollback accepted a release whose scheduled task exited immediately'
    Assert-True ($deadRollbackFailure.Exception.Message -like '*managed scheduled task exited before release became ready: Ready*') 'dead rollback returned the wrong liveness failure'
    Assert-True ($global:NInferTestDeadStartSleptMilliseconds -le 31000) 'dead rollback exceeded the startup-grace detection bound'
    Assert-True ($deadStartWatch.Elapsed.TotalSeconds -lt 35) 'dead rollback was not detected promptly'
    $restoredAfterDeadRollback = Read-State
    Assert-Equal $restoredAfterDeadRollback.active_release 'dead-rollback-current' 'failed rollback did not restore the current release'
    Assert-Equal $restoredAfterDeadRollback.previous_release 'base-release' 'failed rollback changed the prior release identity'
    Assert-True $global:NInferTestTaskRunning 'failed rollback did not restart the current release'
    Assert-OwnerPaused $true 'failed rollback did not preserve GPU-owner isolation'

    & $managedController -Action Rollback -StateRoot $script:StateRoot | Out-Null
    Assert-Equal (Read-State).active_release 'base-release' 'healthy rollback after dead-start recovery failed'
    Assert-KeyLedger $ledger

    & $managedController -Action Stop -StateRoot $script:StateRoot | Out-Null
    Assert-OwnerPaused $false 'stop did not restore a previously running GPU owner'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $script:StateRoot 'gpu-owner-lease.json'))) 'stop left a GPU-owner lease behind'

    & $script:OwnerControllerPath -Action stop | Out-Null
    Assert-OwnerPaused $true 'gaming-mode fixture did not enter paused state'
    & $managedController -Action Start -StateRoot $script:StateRoot | Out-Null
    Assert-OwnerPaused $true 'start changed a previously paused GPU owner'
    $lease = Get-Content -LiteralPath (Join-Path $script:StateRoot 'gpu-owner-lease.json') -Raw | ConvertFrom-Json
    Assert-Equal ([bool]$lease.prior_paused) $true 'lease did not capture paused gaming state'
    & $managedController -Action Restart -StateRoot $script:StateRoot | Out-Null
    Assert-OwnerPaused $true 'restart changed prior gaming state'
    & $managedController -Action Stop -StateRoot $script:StateRoot | Out-Null
    Assert-OwnerPaused $true 'stop did not restore prior gaming state'
    & $script:OwnerControllerPath -Action start | Out-Null
    Assert-OwnerPaused $false 'normal-host state was not restored for cleanup'
    $activeState = Read-State
    $activeRelease = $activeState.releases.PSObject.Properties[[string]$activeState.active_release].Value
    Assert-Equal ([Int64]$activeRelease.model_bytes) ([Int64](Get-Item -LiteralPath ([string]$activeRelease.model_artifact)).Length) 'installed model byte identity was not recorded'
    Assert-True ([Int64]$activeRelease.model_creation_utc_ticks -gt 0) 'installed model creation identity was not recorded'
    Assert-True ([Int64]$activeRelease.model_last_write_utc_ticks -gt 0) 'installed model write identity was not recorded'
    $verifiedWriteTime = [DateTime]::new(
        [Int64]$activeRelease.model_last_write_utc_ticks,
        [DateTimeKind]::Utc
    )
    [IO.File]::SetLastWriteTimeUtc(
        [string]$activeRelease.model_artifact,
        $verifiedWriteTime.AddSeconds(1)
    )
    $metadataError = $null
    try {
        & $managedController -Action Run -StateRoot $script:StateRoot | Out-Null
    }
    catch {
        $metadataError = $_
    }
    finally {
        [IO.File]::SetLastWriteTimeUtc([string]$activeRelease.model_artifact, $verifiedWriteTime)
    }
    Assert-True ($null -ne $metadataError) 'changed model metadata was accepted'
    Assert-Equal $metadataError.Exception.Message 'model artifact changed after install; reinstall the release' 'changed model metadata returned the wrong failure'
    Assert-OwnerPaused $false 'model metadata rejection did not restore the GPU owner'

    $global:NInferBlockedModelPath = (Resolve-Path -LiteralPath ([string]$activeRelease.model_artifact)).Path
    $global:NInferExpectedBinaryPath = (Resolve-Path -LiteralPath ([string]$activeRelease.server_executable)).Path
    $global:NInferExpectedConfigPath = (Resolve-Path -LiteralPath ([string]$activeRelease.config_file)).Path
    $global:NInferBlockedModelHashCalls = 0
    $global:NInferBinaryHashObserved = $false
    $global:NInferConfigHashObserved = $false
    function global:Get-FileHash {
        [CmdletBinding()]
        param(
            [Parameter(Mandatory = $true)][string]$LiteralPath,
            [string]$Algorithm = 'SHA256'
        )
        $resolved = (Resolve-Path -LiteralPath $LiteralPath).Path
        if ($resolved -eq $global:NInferBlockedModelPath) {
            $global:NInferBlockedModelHashCalls++
            throw 'restart attempted to rehash the installed model'
        }
        if ($resolved -eq $global:NInferExpectedBinaryPath) { $global:NInferBinaryHashObserved = $true }
        if ($resolved -eq $global:NInferExpectedConfigPath) { $global:NInferConfigHashObserved = $true }
        return Microsoft.PowerShell.Utility\Get-FileHash -Algorithm $Algorithm -LiteralPath $LiteralPath
    }
    $runError = $null
    try {
        & $managedController -Action Run -StateRoot $script:StateRoot | Out-Null
    }
    catch {
        $runError = $_
    }
    finally {
        Remove-Item -LiteralPath Function:\global:Get-FileHash -Force
    }
    Assert-True ($null -ne $runError) 'invalid fixture server unexpectedly ran'
    Assert-True ($runError.Exception.Message -cne 'restart attempted to rehash the installed model') 'restart rehashed the installed model'
    Assert-Equal $global:NInferBlockedModelHashCalls 0 'restart read the full installed model'
    Assert-True $global:NInferBinaryHashObserved 'restart did not validate the server executable'
    Assert-True $global:NInferConfigHashObserved 'restart did not validate the server config'
    Assert-OwnerPaused $false 'failed runtime validation did not restore the GPU owner'

    $active = Read-State
    $capturedLeasePath = Join-Path $script:StateRoot 'gpu-owner-lease.json'
    Write-Json $capturedLeasePath ([ordered]@{
            artifact_type = 'ninfer_gpu_owner_lease'; schema_version = 1
            release_id = [string]$active.active_release
            controller_sha256 = [string]$active.gpu_owner.controller_sha256
            prior_paused = $false; phase = 'captured'; acquired_utc = [DateTime]::UtcNow.ToString('o')
        })
    & $script:OwnerControllerPath -Action start | Out-Null
    $stopCallsBefore = [int](Read-OwnerState).stop_calls
    & $managedController -Action Start -StateRoot $script:StateRoot | Out-Null
    Assert-True ([int](Read-OwnerState).stop_calls -gt $stopCallsBefore) 'captured GPU-owner lease did not re-drive owner stop'
    & $managedController -Action Stop -StateRoot $script:StateRoot | Out-Null
    Assert-OwnerPaused $false 'captured lease recovery did not restore prior GPU owner state on stop'

    [ordered]@{
        artifact_type = 'ninfer_release_lifecycle_regression'
        schema_version = 1
        status = 'passed'
        injected_failures = $faultPoints.Count
        published_default_gpu_owner_controllers = 1
        retries = $faultPoints.Count
        rollback_key_pairs = $faultPoints.Count
        restart_model_rehash_calls = $global:NInferBlockedModelHashCalls
        model_metadata_tamper_rejections = 1
        schema1_release_records_migrated = 1
        schema3_identity_mismatch_rejections = 1
        unmigratable_release_rejections = 1
        stub_acl_propagation_rejections = 1
        interrupted_reentry_rejections = 2
        installer_repairs = 2
        scalar_cleanup_actions = 1
        array_cleanup_actions = 4
        candidate_model_copies = 0
        dead_start_rejections = 1
        captured_gpu_lease_reentries = 1
        stub_acl_ordering_observations = $earlyAclObservations.Count
        selected_gpu_identity_bindings = 1
        dead_start_sleep_milliseconds = $global:NInferTestDeadStartSleptMilliseconds
    } | ConvertTo-Json -Compress
}
finally {
    [Environment]::SetEnvironmentVariable('NINFER_TEST_INSTALL_FAILURE_AFTER', $originalFault, 'Process')
    [Environment]::SetEnvironmentVariable('NINFER_TEST_INSTALL_INTERRUPTION_AFTER', $originalInterrupt, 'Process')
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    foreach ($name in @(
            'Get-ScheduledTask', 'Export-ScheduledTask', 'New-ScheduledTaskAction',
            'New-ScheduledTaskTrigger', 'New-ScheduledTaskSettingsSet',
            'New-ScheduledTaskPrincipal', 'Register-ScheduledTask',
            'Unregister-ScheduledTask', 'Start-ScheduledTask', 'Stop-ScheduledTask',
            'Get-NetTCPConnection', 'Invoke-RestMethod', 'Start-Sleep', 'icacls.exe', 'nvidia-smi.exe'
        )) {
        Remove-Item -LiteralPath "Function:\global:$name" -ErrorAction SilentlyContinue
    }
}
