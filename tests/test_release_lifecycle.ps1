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
$global:NInferTestTaskExists = $false
$global:NInferTestTaskRunning = $false
$global:NInferTestTaskXml = $null
$global:NInferTestTaskSerial = 0
$global:NInferTestStateRoot = $null

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
    $global:NInferTestTaskRunning = $true
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
    $release = $state.releases.PSObject.Properties[[string]$state.active_release].Value
    return [pscustomobject]@{
        artifact_type = 'ninfer_server_status'
        schema_version = 1
        status = 'ok'
        identity = [pscustomobject]@{
            patch_stack_sha = [string]$release.patch_stack_sha
            deployment_profile = [string]$release.deployment_profile
            binary_sha256 = [string]$release.binary_sha256
            model_artifact_sha256 = [string]$release.model_sha256
            config_sha256 = [string]$release.config_sha256
        }
        runtime = [pscustomobject]@{}
        scheduler = [pscustomobject]@{}
        cache = [pscustomobject]@{}
        mtp = [pscustomobject]@{}
    }
}

function global:icacls.exe {
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
        lifecycle = [ordered]@{ task_name = 'NInfer-Qwen38-4090-v0.1' }
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

function Invoke-FixtureInstall([object]$Package, [object]$Secret, [string]$Fault = '') {
    $previous = [Environment]::GetEnvironmentVariable('NINFER_TEST_INSTALL_FAILURE_AFTER', 'Process')
    try {
        if ([string]::IsNullOrEmpty($Fault)) {
            [Environment]::SetEnvironmentVariable('NINFER_TEST_INSTALL_FAILURE_AFTER', $null, 'Process')
        }
        else {
            [Environment]::SetEnvironmentVariable('NINFER_TEST_INSTALL_FAILURE_AFTER', $Fault, 'Process')
        }
        & $InstallerPath -PackagePath $Package.path -PackageSha256 $Package.sha256 `
            -ModelArtifactPath $script:ModelPath -ApiKeyFile $Secret.path `
            -StateRoot $script:StateRoot | Out-Null
    }
    finally {
        [Environment]::SetEnvironmentVariable('NINFER_TEST_INSTALL_FAILURE_AFTER', $previous, 'Process')
    }
}

function Read-State {
    return Get-Content -LiteralPath (Join-Path $script:StateRoot 'state.json') -Raw | ConvertFrom-Json
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

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('ninfer-release-lifecycle-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$script:StateRoot = Join-Path $testRoot 'state'
$script:ModelPath = Join-Path $testRoot 'model.ninfer'
$global:NInferTestStateRoot = $script:StateRoot
$originalFault = [Environment]::GetEnvironmentVariable('NINFER_TEST_INSTALL_FAILURE_AFTER', 'Process')
$originalTestMode = [Environment]::GetEnvironmentVariable('NINFER_INSTALL_TEST_MODE', 'Process')
[Environment]::SetEnvironmentVariable('NINFER_INSTALL_TEST_MODE', 'transaction', 'Process')

try {
    $modelBytes = New-Object byte[] 256
    $modelRandom = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $modelRandom.GetBytes($modelBytes) } finally { $modelRandom.Dispose() }
    [IO.File]::WriteAllBytes($script:ModelPath, $modelBytes)

    $ledger = @{}
    $basePackage = New-FixturePackage $testRoot 'base-release' $script:ModelPath 48100
    $baseSecret = New-SecretFile $testRoot 'base-release'
    Invoke-FixtureInstall $basePackage $baseSecret
    $state = Read-State
    Assert-Equal $state.active_release 'base-release' 'base release did not activate'
    $baseKeyPath = [string]$state.releases.PSObject.Properties['base-release'].Value.api_key_file
    Assert-Equal $baseKeyPath (Join-Path (Join-Path (Join-Path $script:StateRoot 'secrets') 'base-release') 'api-key.txt') 'base key is not release-scoped'
    $ledger['base-release'] = [pscustomobject]@{ path = $baseKeyPath; sha256 = $baseSecret.sha256 }
    Assert-KeyLedger $ledger

    $faultPoints = @(
        'release_move',
        'model_copy',
        'secret_copy',
        'incumbent_stop',
        'state_activation',
        'controller_copy',
        'acl',
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

        Invoke-FixtureInstall $package $secret
        $state = Read-State
        Assert-Equal $state.active_release $instance 'clean retry did not activate candidate'
        $candidateKeyPath = [string]$state.releases.PSObject.Properties[$instance].Value.api_key_file
        Assert-Equal $candidateKeyPath (Join-Path (Join-Path (Join-Path $script:StateRoot 'secrets') $instance) 'api-key.txt') 'candidate key is not release-scoped'
        $ledger[$instance] = [pscustomobject]@{ path = $candidateKeyPath; sha256 = $secret.sha256 }
        Assert-KeyLedger $ledger

        & (Join-Path $script:StateRoot 'Control-Release.ps1') -Action Rollback `
            -StateRoot $script:StateRoot | Out-Null
        Assert-Equal (Read-State).active_release 'base-release' 'rollback did not restore base release'
        Assert-KeyLedger $ledger
    }

    [ordered]@{
        artifact_type = 'ninfer_release_lifecycle_regression'
        schema_version = 1
        status = 'passed'
        injected_failures = $faultPoints.Count
        retries = $faultPoints.Count
        rollback_key_pairs = $faultPoints.Count
    } | ConvertTo-Json -Compress
}
finally {
    [Environment]::SetEnvironmentVariable('NINFER_TEST_INSTALL_FAILURE_AFTER', $originalFault, 'Process')
    [Environment]::SetEnvironmentVariable('NINFER_INSTALL_TEST_MODE', $originalTestMode, 'Process')
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    foreach ($name in @(
            'Get-ScheduledTask', 'Export-ScheduledTask', 'New-ScheduledTaskAction',
            'New-ScheduledTaskTrigger', 'New-ScheduledTaskSettingsSet',
            'New-ScheduledTaskPrincipal', 'Register-ScheduledTask',
            'Unregister-ScheduledTask', 'Start-ScheduledTask', 'Stop-ScheduledTask',
            'Get-NetTCPConnection', 'Invoke-RestMethod', 'icacls.exe'
        )) {
        Remove-Item -LiteralPath "Function:\global:$name" -ErrorAction SilentlyContinue
    }
}
