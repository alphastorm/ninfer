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
$global:NInferTestDeadReleaseId = $null
$global:NInferTestDeadStartSleptMilliseconds = 0
$global:NInferTestAclFailure = $false
$global:NInferAclObservations = [Collections.Generic.List[object]]::new()
$global:NInferTestAclResetArguments = @()
$global:NInferAclCalls = [Collections.Generic.List[object]]::new()

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
        if ($global:NInferTestDeadStartSleptMilliseconds -gt 8000) {
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
    $global:NInferAclCalls.Add(@($arguments))
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
    '0, GPU-fixture-3090, NVIDIA GeForce RTX 3090'
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
    [string]$ReleaseName,
    [string]$ModelPath,
    [int]$Port
) {
    $workspace = Join-Path $Directory "package-$ReleaseName"
    $rootName = "payload-$ReleaseName"
    $payload = Join-Path $workspace $rootName
    $bin = Join-Path $payload 'bin'
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
    foreach ($name in @('ninfer.exe', 'ninfer-serve.exe', 'ninfer_bench.exe')) {
        [IO.File]::WriteAllText(
            (Join-Path $bin $name),
            "fixture-$name-$ReleaseName",
            [Text.UTF8Encoding]::new($false)
        )
    }
    [IO.File]::WriteAllText(
        (Join-Path $bin 'runtime-fixture.dll'),
        "runtime-$ReleaseName",
        [Text.UTF8Encoding]::new($false)
    )
    Copy-Item -LiteralPath $ControllerPath -Destination (Join-Path $payload 'Control-Release.ps1')
    Copy-Item -LiteralPath $InstallerPath -Destination (Join-Path $payload 'Install-Release.ps1')
    Copy-Item -LiteralPath $script:StateHelperPath -Destination (Join-Path $payload 'Protect-StateRoot.ps1')
    [IO.File]::WriteAllText(
        (Join-Path $payload 'New-QualificationReceipt.ps1'),
        '# qualification receipt fixture',
        [Text.UTF8Encoding]::new($false)
    )

    $config = [ordered]@{
        artifact_type = 'ninfer_windows_server_config'
        schema_version = 2
        release_id = $ReleaseName
        deployment_profile = 'qwen38-3090-omp-v0.2.0-c1'
        listen = [ordered]@{
            host = '127.0.0.1'
            port = $Port
            allowed_host_classes = @('loopback', 'tailscale-ipv4')
        }
        authentication = [ordered]@{ mode = 'required-api-key-file' }
        model_id = 'q38-ninfer'
        engine = [ordered]@{
            device = 0
            max_context = 65536
            kv_capacity = 'auto'
            prefill_chunk = 1024
            kv_dtype = 'int8'
            max_concurrency = 1
            max_pending_requests = 16
            pending_timeout_ms = 30000
            cuda_graph = $true
            prefix_reuse = $true
            vision = $false
        }
        speculative = [ordered]@{ backend = 'mtp'; draft_tokens = 3 }
        reasoning = [ordered]@{ effort = 'xhigh'; preserve_thinking = $true }
        response_store = [ordered]@{ max_records = 16; max_mib = 8 }
        session_checkpoint = [ordered]@{ enabled = $true; quota_mib = 65536; staging_mib = 256 }
        telemetry = [ordered]@{ stats_interval_ms = 1000 }
    }
    $configPath = Join-Path $payload 'server-config.json'
    Write-Json $configPath $config

    $model = Get-Item -LiteralPath $ModelPath
    $spec = [ordered]@{
        artifact_type = 'ninfer_windows_release_spec'
        schema_version = 2
        release_id = $ReleaseName
        release_version = '0.2.0'
        deployment_profile = 'qwen38-3090-omp-v0.2.0-c1'
        build_profile = 'omp-v0.2.0-rtx3090'
        platform = 'windows-x86_64-cuda12.8-rtx3090'
        source = [ordered]@{
            upstream_base_sha = ('1' * 40)
            lineage_base_sha = ('2' * 40)
        }
        gpu = [ordered]@{
            name = 'NVIDIA GeForce RTX 3090'
            cuda_architecture = 'sm_86'
            cmake_cuda_architecture = '86'
            compute_capability = '8.6'
            minimum_driver_major = 570
        }
        model = [ordered]@{
            bytes = [Int64]$model.Length
            sha256 = Get-Sha256 $ModelPath
        }
        network = [ordered]@{
            allowed_listen_host_classes = @('loopback', 'tailscale-ipv4')
            authentication = 'required-api-key-file'
        }
        lifecycle = [ordered]@{
            task_name = 'NInfer-Qwen38-3090-OMP-v0.2'
            candidate_root_directories = @('bin', 'config', 'logs', 'receipts')
            model_artifact_ownership = 'external-pinned-read-only'
            cache_ownership = 'state-root-per-release'
            interrupted_install_reentry = 'repair-required'
            state_pointers = @('prepared_release', 'active_release', 'previous_release')
        }
    }
    Write-Json (Join-Path $payload 'release-spec.json') $spec

    $patchSha = Get-Sha256 (Join-Path $bin 'ninfer-serve.exe')
    $patchSha = $patchSha.Substring(0, 40)
    $instanceId = "$ReleaseName-$($patchSha.Substring(0, 12))"
    $identity = [ordered]@{
        artifact_type = 'ninfer_release_build_identity'
        schema_version = 2
        release_version = 'v0.2.0'
        platform = 'windows-x86_64-cuda12.8-rtx3090'
        upstream_base_sha = ('1' * 40)
        lineage_base_sha = ('2' * 40)
        patch_stack_sha = $patchSha
        build_profile = 'omp-v0.2.0-rtx3090'
        build_type = 'Release'
        cxx_compiler = 'fixture'
        cuda_compiler = 'fixture'
        cuda_toolkit = '12.8'
        cuda_architecture = '86'
        source_dirty = $false
        source_archive_sha256 = ('3' * 64)
        configuration_sha256 = Get-Sha256 $configPath
        binaries = [ordered]@{
            ninfer = Get-Sha256 (Join-Path $bin 'ninfer.exe')
            'ninfer-serve' = Get-Sha256 (Join-Path $bin 'ninfer-serve.exe')
            ninfer_bench = Get-Sha256 (Join-Path $bin 'ninfer_bench.exe')
        }
        runtime_dependencies = [ordered]@{
            'runtime-fixture.dll' = Get-Sha256 (Join-Path $bin 'runtime-fixture.dll')
        }
    }
    Write-Json (Join-Path $payload 'build-identity.json') $identity

    $checksumLines = @(
        Get-ChildItem -LiteralPath $payload -File -Recurse -Force |
            Sort-Object { $_.FullName.Substring($payload.Length + 1).Replace('\', '/') } |
            ForEach-Object {
                $relative = $_.FullName.Substring($payload.Length + 1).Replace('\', '/')
                "$(Get-Sha256 $_.FullName)  $relative"
            }
    )
    [IO.File]::WriteAllText(
        (Join-Path $payload 'SHA256SUMS.txt'),
        ([string]::Join([Environment]::NewLine, $checksumLines) + [Environment]::NewLine),
        [Text.Encoding]::ASCII
    )

    $archive = Join-Path $Directory "$ReleaseName.tar.gz"
    $tar = (Get-Command tar -ErrorAction Stop).Source
    Push-Location $workspace
    try {
        & $tar '-czf' $archive $rootName
        if ($LASTEXITCODE -ne 0) { throw 'fixture package tar creation failed' }
    }
    finally { Pop-Location }
    return [pscustomobject]@{
        release_name = $ReleaseName
        instance_id = $instanceId
        path = $archive
        sha256 = Get-Sha256 $archive
    }
}

function Invoke-FixtureInstall(
    [object]$Package,
    [object]$Secret,
    [string]$Fault = '',
    [string]$Interrupt = '',
    [switch]$NoStart
) {
    if (-not [string]::IsNullOrEmpty($Fault) -and -not [string]::IsNullOrEmpty($Interrupt)) {
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
        $output = @(& $InstallerPath @parameters)
        if ($output.Count -eq 0) { throw 'installer fixture returned no receipt' }
        return ([string]$output[-1] | ConvertFrom-Json)
    }
    finally {
        [Environment]::SetEnvironmentVariable('NINFER_TEST_INSTALL_FAILURE_AFTER', $previousFault, 'Process')
        [Environment]::SetEnvironmentVariable('NINFER_TEST_INSTALL_INTERRUPTION_AFTER', $previousInterrupt, 'Process')
    }
}

function Invoke-InstallerRepair {
    $output = @(& $InstallerPath -RepairInterruptedInstall -StateRoot $script:StateRoot)
    if ($output.Count -eq 0) { throw 'installer repair returned no receipt' }
    return ([string]$output[-1] | ConvertFrom-Json)
}

function Get-PendingFixtureTransactions {
    $root = Join-Path $script:StateRoot 'receipts/install-transactions'
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { return @() }
    return @(
        Get-ChildItem -LiteralPath $root -Directory |
            ForEach-Object {
                $journal = Get-Content -LiteralPath (Join-Path $_.FullName 'journal.json') -Raw | ConvertFrom-Json
                if ([string]$journal.status -cin @('in_progress', 'repair_required')) { $journal }
            }
    )
}

function Read-State {
    return Get-Content -LiteralPath (Join-Path $script:StateRoot 'state.json') -Raw | ConvertFrom-Json
}

function Assert-CandidateLayout([object]$Release, [string]$ReleaseId) {
    Assert-Equal ([string]$Release.model_reference) 'external-pinned-read-only' 'candidate model is not external'
    $modelPath = [IO.Path]::GetFullPath([string]$Release.model_artifact)
    $statePrefix = [IO.Path]::GetFullPath($script:StateRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    Assert-True (-not $modelPath.StartsWith($statePrefix, [StringComparison]::OrdinalIgnoreCase)) 'candidate model is inside lifecycle state'
    $releaseRoot = [IO.Path]::GetFullPath([string]$Release.release_root)
    Assert-Equal $releaseRoot ([IO.Path]::GetFullPath((Join-Path (Join-Path $script:StateRoot 'releases') $ReleaseId))) 'candidate root identity mismatch'
    $directories = @(Get-ChildItem -LiteralPath $releaseRoot -Directory -Force | Sort-Object Name | ForEach-Object Name)
    Assert-Equal ([string]::Join(',', $directories)) 'bin,config,logs,receipts' 'candidate root directories changed'
    Assert-Equal @(Get-ChildItem -LiteralPath $releaseRoot -File -Force).Count 0 'candidate root has unclassified files'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $releaseRoot 'model'))) 'candidate contains a model copy'
    Assert-True (Test-Path -LiteralPath (Join-Path (Join-Path $releaseRoot 'bin') 'runtime-fixture.dll') -PathType Leaf) 'candidate omitted app-local DLL'
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('ninfer-release-lifecycle-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$stateHelperSource = Join-Path (Split-Path -Parent $InstallerPath) 'Protect-StateRoot.ps1'
if (-not (Test-Path -LiteralPath $stateHelperSource -PathType Leaf)) {
    throw 'production installer omitted the protected-state helper'
}
$script:StateHelperPath = Join-Path $testRoot 'Protect-StateRoot.ps1'
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
    $script:StateHelperPath,
    $instrumentedProtection,
    [Text.UTF8Encoding]::new($false)
)
$installerSource = Get-Content -LiteralPath $InstallerPath -Raw -Encoding UTF8
$testModeNeedle = '$script:InstallTestMode = $false'
if ([regex]::Matches($installerSource, [regex]::Escape($testModeNeedle)).Count -ne 1) {
    throw 'production installer test-mode invariant changed'
}
$installerSource = $installerSource.Replace($testModeNeedle, '$script:InstallTestMode = $true')
$InstallerPath = Join-Path $testRoot 'Install-Release.test.ps1'
[IO.File]::WriteAllText($InstallerPath, $installerSource, [Text.UTF8Encoding]::new($false))
$script:StateRoot = Join-Path $testRoot 'state'
$script:ModelPath = Join-Path $testRoot 'external-model.ninfer'
$script:OwnerStatePath = Join-Path $testRoot 'owner-state.json'
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
    $modelBytes = New-Object byte[] 1024
    $random = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $random.GetBytes($modelBytes) } finally { $random.Dispose() }
    [IO.File]::WriteAllBytes($script:ModelPath, $modelBytes)
    $modelShaBefore = Get-Sha256 $script:ModelPath

    $basePackage = New-FixturePackage $testRoot 'base-release' $script:ModelPath 48100
    $baseSecret = New-SecretFile $testRoot 'base-release'
    $multilineSecret = Join-Path $testRoot 'multiline.key'
    $multilineText = [string]::Join([Environment]::NewLine, @('first', 'second')) + [Environment]::NewLine
    [IO.File]::WriteAllText($multilineSecret, $multilineText, [Text.UTF8Encoding]::new($false))
    $multilineRejected = $false
    try {
        & $InstallerPath -PackagePath $basePackage.path -PackageSha256 $basePackage.sha256 -ModelArtifactPath $script:ModelPath -ApiKeyFile $multilineSecret -GpuOwnerControllerPath $script:OwnerControllerPath -StateRoot $script:StateRoot -NoStart | Out-Null
    }
    catch { $multilineRejected = $_.Exception.Message -like '*exactly one non-empty line*' }
    Assert-True $multilineRejected 'installer accepted a multiline secret file'

    $baseReceipt = Invoke-FixtureInstall $basePackage $baseSecret
    Assert-Equal ([string]$baseReceipt.status) 'passed' 'clean install did not pass'
    Assert-Equal ([int]$baseReceipt.schema_version) 2 'install receipt schema changed'
    Assert-Equal ([int]$baseReceipt.secret_values_recorded) 0 'install receipt recorded a secret value'
    $baseReceiptText = $baseReceipt | ConvertTo-Json -Depth 20 -Compress
    $baseSecretValue = [IO.File]::ReadAllText($baseSecret.path)
    Assert-True (-not $baseReceiptText.Contains($baseSecretValue)) 'install receipt contains the API key'
    Assert-Equal ([string]$baseReceipt.identity.build_profile) 'omp-v0.2.0-rtx3090' 'install receipt lost build profile'
    Assert-Equal ([string]$baseReceipt.identity.cuda_architecture) 'sm_86' 'install receipt lost CUDA architecture'

    $baseId = [string]$basePackage.instance_id
    $state = Read-State
    Assert-Equal ([int]$state.schema_version) 4 'clean install did not publish schema 4 state'
    Assert-True ([string]::IsNullOrWhiteSpace([string]$state.prepared_release)) 'clean install left a prepared pointer'
    Assert-Equal ([string]$state.active_release) $baseId 'clean install active pointer mismatch'
    Assert-True ([string]::IsNullOrWhiteSpace([string]$state.previous_release)) 'clean install created a previous pointer'
    $baseRelease = $state.releases.PSObject.Properties[$baseId].Value
    Assert-CandidateLayout $baseRelease $baseId
    foreach ($field in @('ninfer_sha256', 'binary_sha256', 'benchmark_sha256', 'config_sha256', 'model_artifact_sha256', 'package_sha256', 'inner_checksums_sha256')) {
        Assert-True ([string]$baseRelease.$field -cmatch '^[0-9a-f]{64}$') "release identity field is invalid: $field"
    }
    $baseKeyPath = [string]$baseRelease.api_key_file
    Assert-Equal (Get-Sha256 $baseKeyPath) ([string]$baseSecret.sha256) 'installed API key changed'
    Assert-True $global:NInferTestTaskRunning 'clean install did not start the managed task'
    Assert-True ([bool](Get-Content -LiteralPath $script:OwnerStatePath -Raw | ConvertFrom-Json).paused) 'clean install did not pause the prior GPU owner'
    $keyAcl = @($global:NInferAclCalls | Where-Object { $_ -contains $baseKeyPath })
    Assert-True ($keyAcl.Count -ge 1) 'installer did not apply an explicit API-key ACL'
    Assert-True (@($keyAcl | Where-Object { $_ -contains 'SYSTEM:F' -and $_ -contains 'Administrators:F' }).Count -eq 1) 'API-key ACL does not grant only explicit system/admin rights plus operator read'

    $stateShaBeforeIdempotent = Get-Sha256 (Join-Path $script:StateRoot 'state.json')
    $global:NInferBlockedModelPath = (Resolve-Path -LiteralPath $script:ModelPath).Path
    $global:NInferBlockedModelHashCalls = 0
    function global:Get-FileHash {
        [CmdletBinding()]
        param([Parameter(Mandatory = $true)][string]$LiteralPath, [string]$Algorithm = 'SHA256')
        $resolved = (Resolve-Path -LiteralPath $LiteralPath).Path
        if ($resolved -eq $global:NInferBlockedModelPath) {
            $global:NInferBlockedModelHashCalls++
            throw 'idempotent install attempted to rehash the model'
        }
        return Microsoft.PowerShell.Utility\Get-FileHash -Algorithm $Algorithm -LiteralPath $LiteralPath
    }
    try { $alreadyReceipt = Invoke-FixtureInstall $basePackage $baseSecret -NoStart }
    finally { Remove-Item -LiteralPath Function:\Get-FileHash -Force }
    Assert-Equal ([string]$alreadyReceipt.status) 'already_installed' 'exact reinstall was not idempotent'
    Assert-True (-not [bool]$alreadyReceipt.model_rehashed) 'idempotent receipt claims a model rehash'
    Assert-Equal $global:NInferBlockedModelHashCalls 0 'idempotent install rehashed the model'
    Assert-Equal (Get-Sha256 (Join-Path $script:StateRoot 'state.json')) $stateShaBeforeIdempotent 'idempotent install changed lifecycle pointers'
    Assert-Equal (Get-Sha256 $baseKeyPath) ([string]$baseSecret.sha256) 'idempotent install replaced the release secret'

    $upgradePackage = New-FixturePackage $testRoot 'upgrade-release' $script:ModelPath 48101
    $upgradeSecret = New-SecretFile $testRoot 'upgrade-release'
    $baselineStateSha = Get-Sha256 (Join-Path $script:StateRoot 'state.json')
    $baselineControllerSha = Get-Sha256 (Join-Path $script:StateRoot 'Control-Release.ps1')
    $baselineTaskXml = [string]$global:NInferTestTaskXml

    $preStopInterruption = $null
    try { Invoke-FixtureInstall $upgradePackage $upgradeSecret -Interrupt 'prepared_pointer' | Out-Null }
    catch { $preStopInterruption = $_ }
    Assert-True ($null -ne $preStopInterruption) 'pre-stop interruption did not stop the installer'
    $preparedState = Read-State
    Assert-Equal ([string]$preparedState.prepared_release) ([string]$upgradePackage.instance_id) 'prepared pointer was not durable before incumbent stop'
    Assert-Equal ([string]$preparedState.active_release) $baseId 'pre-stop interruption changed the active pointer'
    Assert-True $global:NInferTestTaskRunning 'pre-stop interruption stopped the incumbent'
    Assert-Equal @(Get-PendingFixtureTransactions).Count 1 'pre-stop interruption did not require repair'
    $preRepair = Invoke-InstallerRepair
    Assert-Equal ([string]$preRepair.status) 'passed' 'pre-stop repair failed'
    Assert-Equal (Get-Sha256 (Join-Path $script:StateRoot 'state.json')) $baselineStateSha 'pre-stop repair did not restore exact state bytes'
    Assert-Equal (Get-Sha256 (Join-Path $script:StateRoot 'Control-Release.ps1')) $baselineControllerSha 'pre-stop repair did not restore the controller'
    Assert-Equal ([string]$global:NInferTestTaskXml) $baselineTaskXml 'pre-stop repair changed the task definition'
    Assert-True $global:NInferTestTaskRunning 'pre-stop repair changed incumbent liveness'

    $postStopInterruption = $null
    try { Invoke-FixtureInstall $upgradePackage $upgradeSecret -Interrupt 'incumbent_stop' | Out-Null }
    catch { $postStopInterruption = $_ }
    Assert-True ($null -ne $postStopInterruption) 'post-stop interruption did not stop the installer'
    $postStopState = Read-State
    Assert-Equal ([string]$postStopState.prepared_release) ([string]$upgradePackage.instance_id) 'post-stop interruption lost the prepared pointer'
    Assert-Equal ([string]$postStopState.active_release) $baseId 'post-stop interruption changed the active pointer'
    Assert-True (-not $global:NInferTestTaskRunning) 'post-stop interruption left the incumbent running'
    $postRepair = Invoke-InstallerRepair
    Assert-Equal ([string]$postRepair.status) 'passed' 'post-stop repair failed'
    Assert-Equal (Get-Sha256 (Join-Path $script:StateRoot 'state.json')) $baselineStateSha 'post-stop repair did not restore exact state bytes'
    Assert-True $global:NInferTestTaskRunning 'post-stop repair did not restart the incumbent'

    $faultPoints = @(
        'model_reference', 'candidate_layout', 'secret_copy', 'acl', 'controller_copy',
        'prepared_pointer', 'incumbent_stop', 'state_activation', 'task_registration', 'candidate_start'
    )
    $faultIndex = 0
    foreach ($fault in $faultPoints) {
        $faultIndex++
        $faultPackage = New-FixturePackage $testRoot "fault-$faultIndex" $script:ModelPath (48110 + $faultIndex)
        $faultSecret = New-SecretFile $testRoot "fault-$faultIndex"
        $stateSha = Get-Sha256 (Join-Path $script:StateRoot 'state.json')
        $controllerSha = Get-Sha256 (Join-Path $script:StateRoot 'Control-Release.ps1')
        $taskXml = [string]$global:NInferTestTaskXml
        $failed = $false
        try { Invoke-FixtureInstall $faultPackage $faultSecret -Fault $fault | Out-Null }
        catch { $failed = $_.Exception.Message -like "*injected install failure after $fault*" }
        Assert-True $failed "injected transaction failure did not surface: $fault"
        Assert-Equal (Get-Sha256 (Join-Path $script:StateRoot 'state.json')) $stateSha "failure changed state bytes: $fault"
        Assert-Equal (Get-Sha256 (Join-Path $script:StateRoot 'Control-Release.ps1')) $controllerSha "failure changed controller bytes: $fault"
        Assert-Equal ([string]$global:NInferTestTaskXml) $taskXml "failure changed task definition: $fault"
        Assert-True $global:NInferTestTaskRunning "failure did not restore incumbent liveness: $fault"
        Assert-True (-not (Test-Path -LiteralPath (Join-Path (Join-Path $script:StateRoot 'releases') ([string]$faultPackage.instance_id)))) "failure left candidate files: $fault"
        Assert-True (-not (Test-Path -LiteralPath (Join-Path (Join-Path $script:StateRoot 'secrets') ([string]$faultPackage.instance_id)))) "failure left candidate secret: $fault"
    }

    $upgradeReceipt = Invoke-FixtureInstall $upgradePackage $upgradeSecret
    $upgradeId = [string]$upgradePackage.instance_id
    $state = Read-State
    Assert-True ([string]::IsNullOrWhiteSpace([string]$state.prepared_release)) 'upgrade left a prepared pointer'
    Assert-Equal ([string]$state.active_release) $upgradeId 'upgrade active pointer mismatch'
    Assert-Equal ([string]$state.previous_release) $baseId 'upgrade previous pointer mismatch'
    Assert-CandidateLayout $state.releases.PSObject.Properties[$upgradeId].Value $upgradeId
    Assert-Equal (Get-Sha256 $baseKeyPath) ([string]$baseSecret.sha256) 'upgrade changed previous release secret'
    $upgradeKeyPath = [string]$state.releases.PSObject.Properties[$upgradeId].Value.api_key_file
    Assert-Equal (Get-Sha256 $upgradeKeyPath) ([string]$upgradeSecret.sha256) 'upgrade secret identity mismatch'
    Assert-Equal ([string]$upgradeReceipt.identity.package_sha256) ([string]$upgradePackage.sha256) 'upgrade receipt lost package identity'
    $stateRootAcl = [Collections.Generic.List[object]]::new()
    foreach ($call in $global:NInferAclCalls) {
        $arguments = @($call)
        if ($arguments -contains $script:StateRoot -and $arguments -contains '/inheritance:r') {
            $stateRootAcl.Add($arguments)
        }
    }
    Assert-True ($stateRootAcl.Count -ge 1) 'installer did not apply the final state-root ACL'
    $protectedRootAclFound = $false
    foreach ($call in $stateRootAcl) {
        $arguments = @($call)
        if ($arguments -contains 'SYSTEM:(OI)(CI)F' -and
            $arguments -contains 'Administrators:(OI)(CI)F' -and
            $arguments -notcontains ([string]::Concat($env:USERNAME, ':(OI)(CI)M'))) {
            $protectedRootAclFound = $true
        }
    }
    Assert-True $protectedRootAclFound 'lifecycle fixture did not preserve the protected root ACL call'

    $global:NInferTestDeadReleaseId = $baseId
    $global:NInferTestDeadStartSleptMilliseconds = 0
    $deadFailure = $null
    try { & (Join-Path $script:StateRoot 'Control-Release.ps1') -Action Rollback -StateRoot $script:StateRoot | Out-Null }
    catch { $deadFailure = $_ }
    finally { $global:NInferTestDeadReleaseId = $null }
    Assert-True ($null -ne $deadFailure) 'rollback accepted a dead release start'
    Assert-True ($deadFailure.Exception.Message -like '*managed scheduled task exited before release became ready*') 'dead start returned the wrong failure'
    $state = Read-State
    Assert-Equal ([string]$state.active_release) $upgradeId 'failed rollback did not restore active pointer'
    Assert-Equal ([string]$state.previous_release) $baseId 'failed rollback did not restore previous pointer'
    Assert-True $global:NInferTestTaskRunning 'failed rollback did not restart the current release'

    & (Join-Path $script:StateRoot 'Control-Release.ps1') -Action Rollback -StateRoot $script:StateRoot | Out-Null
    $state = Read-State
    Assert-Equal ([string]$state.active_release) $baseId 'exact rollback did not activate the previous release'
    Assert-Equal ([string]$state.previous_release) $upgradeId 'exact rollback did not preserve the replaced release'
    Assert-True ([string]::IsNullOrWhiteSpace([string]$state.prepared_release)) 'rollback created a prepared pointer'
    Assert-Equal (Get-Sha256 $baseKeyPath) ([string]$baseSecret.sha256) 'rollback changed base secret'
    Assert-Equal (Get-Sha256 $upgradeKeyPath) ([string]$upgradeSecret.sha256) 'rollback changed upgrade secret'

    $managedController = Join-Path $script:StateRoot 'Control-Release.ps1'
    & $managedController -Action Stop -StateRoot $script:StateRoot | Out-Null
    $activeRelease = (Read-State).releases.PSObject.Properties[$baseId].Value
    $global:NInferBlockedModelPath = (Resolve-Path -LiteralPath ([string]$activeRelease.model_artifact)).Path
    $global:NInferBlockedModelHashCalls = 0
    $global:NInferExecutableHashCalls = 0
    function global:Get-FileHash {
        [CmdletBinding()]
        param([Parameter(Mandatory = $true)][string]$LiteralPath, [string]$Algorithm = 'SHA256')
        $resolved = (Resolve-Path -LiteralPath $LiteralPath).Path
        if ($resolved -eq $global:NInferBlockedModelPath) {
            $global:NInferBlockedModelHashCalls++
            throw 'restart attempted to rehash the installed model'
        }
        if ([IO.Path]::GetExtension($resolved) -ceq '.exe' -or [IO.Path]::GetFileName($resolved) -ceq 'server-config.json') {
            $global:NInferExecutableHashCalls++
        }
        return Microsoft.PowerShell.Utility\Get-FileHash -Algorithm $Algorithm -LiteralPath $LiteralPath
    }
    $runFailure = $null
    try { & $managedController -Action Run -StateRoot $script:StateRoot | Out-Null }
    catch { $runFailure = $_ }
    finally { Remove-Item -LiteralPath Function:\Get-FileHash -Force }
    Assert-True ($null -ne $runFailure) 'invalid fixture executable unexpectedly ran'
    Assert-Equal $global:NInferBlockedModelHashCalls 0 'restart path rehashed the external model'
    Assert-True ($global:NInferExecutableHashCalls -ge 4) 'restart did not verify three binaries and config'
    Assert-True (-not [bool](Get-Content -LiteralPath $script:OwnerStatePath -Raw | ConvertFrom-Json).paused) 'failed run did not restore GPU owner'

    & $managedController -Action Start -StateRoot $script:StateRoot | Out-Null
    $allSecrets = @($baseSecretValue, [IO.File]::ReadAllText($upgradeSecret.path))
    $uninstallOutput = @(& $managedController -Action Uninstall -StateRoot $script:StateRoot)
    $uninstallReceipt = [string]$uninstallOutput[-1] | ConvertFrom-Json
    Assert-Equal ([string]$uninstallReceipt.status) 'passed' 'uninstall did not pass'
    Assert-Equal ([int]$uninstallReceipt.release_count) 2 'uninstall did not clean every installed release'
    Assert-Equal ([int]$uninstallReceipt.external_models_deleted) 0 'uninstall claims it deleted an external model'
    $uninstallText = $uninstallReceipt | ConvertTo-Json -Depth 8 -Compress
    foreach ($secretValue in $allSecrets) {
        Assert-True (-not $uninstallText.Contains($secretValue)) 'uninstall receipt contains a secret value'
    }
    Assert-True (-not (Test-Path -LiteralPath $script:StateRoot)) 'uninstall left lifecycle state behind'
    Assert-True (Test-Path -LiteralPath $script:ModelPath -PathType Leaf) 'uninstall deleted the external model'
    Assert-Equal (Get-Sha256 $script:ModelPath) $modelShaBefore 'lifecycle changed the external model'
    Assert-True (-not $global:NInferTestTaskExists) 'uninstall left the scheduled task registered'
    Assert-True (-not [bool](Get-Content -LiteralPath $script:OwnerStatePath -Raw | ConvertFrom-Json).paused) 'uninstall did not restore the GPU owner'

    [ordered]@{
        artifact_type = 'ninfer_windows_lifecycle_regression'
        schema_version = 2
        status = 'passed'
        clean_installs = 1
        idempotent_installs = 1
        upgrades = 1
        injected_failures = $faultPoints.Count
        interrupted_repairs = 2
        dead_start_rejections = 1
        exact_rollbacks = 1
        restart_model_rehash_calls = $global:NInferBlockedModelHashCalls
        uninstalls = 1
        external_model_copies = 0
        secret_values_recorded = 0
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
        Remove-Item -LiteralPath "Function:\$name" -ErrorAction SilentlyContinue
    }
}
